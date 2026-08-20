#include "ezsp-spi.h"

// ---------------------------------------------------------------- EZSP v4 --
//
// Frame IDs are version specific; these are EZSP v4, which is what this NCP
// reports (EmberZNet 5.8.1.0). Cross-checked against bellows' v4 command table.





// EmberStatus

// EmberJoinMethod: plain MAC association, the normal permit-join path.

// The well-known global trust centre link key, ASCII "ZigBeeAlliance09".
const uint8_t zigbee_alliance_09[16] = {
	0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C,
	0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39,
};

// Send an EZSP frame and return the response payload length, or -1. The frame
// ID that actually came back is reported through out_id, which matters for
// callbacks: those arrive in response to EZSP_CALLBACK but carry their own ID.
//
// v4 layout is [sequence][frameControl][frameID][parameters], wrapped for SPI
// as 0xFE <len> <frame> 0xA7. The reply carries the same shape back with
// frameControl bit 7 set.
int ezsp_xfer(int spi, struct pins *p, uint8_t frame_id,
		     const uint8_t *params, size_t plen, uint8_t *out_id,
		     uint8_t *payload, size_t payload_max);

int ezsp_cmd(int spi, struct pins *p, uint8_t frame_id,
		    const uint8_t *params, size_t plen,
		    uint8_t *payload, size_t payload_max)
{
	uint8_t got = 0;
	int n = ezsp_xfer(spi, p, frame_id, params, plen, &got, payload,
			  payload_max);

	if (n >= 0 && got != frame_id)
		printf("  warning: asked for frame 0x%02X, got 0x%02X\n",
		       frame_id, got);

	return n;
}

int ezsp_xfer(int spi, struct pins *p, uint8_t frame_id,
		     const uint8_t *params, size_t plen, uint8_t *out_id,
		     uint8_t *payload, size_t payload_max)
{
	static uint8_t seq;
	uint8_t tx[160], rx[260];
	enum rsp_class cls;
	uint8_t first, data;
	size_t txlen = 0;
	int len, hdr, id_off;

	if (plen + 5 > sizeof(tx))
		die("ezsp command too large");

	// The EZSP frame header is version dependent. Command *ids* are stable
	// from v4 to v8 -- only this layout changes, and getting it wrong shifts
	// every reply, which shows up as "frame 0xFF" and garbage payloads.
	//
	//   v4      [seq][ctrl][id]                        id 8-bit  at +2
	//   v5..v7  [seq][ctrl][0xFF][0x00][id]            id 8-bit  at +4
	//   v8      [seq][ctrl lo][ctrl hi][id lo][id hi]  id 16-bit at +3
	if (cfg.ezsp_version >= 8) {
		hdr = 5;
		id_off = 3;
	} else if (cfg.ezsp_version >= 5) {
		hdr = 5;
		id_off = 4;
	} else {
		hdr = 3;
		id_off = 2;
	}

	tx[txlen++] = SPI_CMD_EZSP_FRAME;
	tx[txlen++] = (uint8_t)(plen + hdr);
	tx[txlen++] = seq++;

	if (cfg.ezsp_version >= 8) {
		tx[txlen++] = 0x00;	// frame control, low
		tx[txlen++] = 0x01;	// frame control, high
		tx[txlen++] = frame_id;	// frame id, low
		tx[txlen++] = 0x00;	// frame id, high
	} else if (cfg.ezsp_version >= 5) {
		tx[txlen++] = 0x00;	// frame control
		tx[txlen++] = 0xFF;	// legacy frame id marker
		tx[txlen++] = 0x00;	// extended frame control
		tx[txlen++] = frame_id;
	} else {
		tx[txlen++] = 0x00;	// frame control
		tx[txlen++] = frame_id;
	}
	if (plen) {
		memcpy(tx + txlen, params, plen);
		txlen += plen;
	}
	tx[txlen++] = SPI_FRAME_TERMINATOR;

	cls = do_cmd(spi, p, tx, txlen, rx, sizeof(rx), 1);

	if (cls != RSP_EZSP) {
		classify(rx, sizeof(rx), &first, &data);
		printf("  ezsp 0x%02X: no frame back (%s 0x%02X)\n", frame_id,
		       cls == RSP_IDLE ? "idle" : "response", first);
		return -1;
	}

	// rx: FE <len> <header> <payload...> A7, header as above
	len = (int)rx[1] - hdr;
	if (len < 0)
		return -1;

	*out_id = rx[2 + id_off];	// v8 id is 16-bit; low byte identifies it

	if ((size_t)len > payload_max)
		len = (int)payload_max;
	memcpy(payload, rx + 2 + hdr, (size_t)len);

	return len;
}

// The NCP must be told which EZSP version the host speaks before anything else.
int ezsp_init(int spi, struct pins *p)
{
	uint8_t params[1] = { (uint8_t)cfg.ezsp_want }, out[8];
	int n;

	// The first version() must use the v4 layout: that is the only format
	// every NCP understands before a version has been agreed.
	cfg.ezsp_version = 4;
	n = ezsp_cmd(spi, p, EZSP_VERSION, params, 1, out, sizeof(out));

	if (n < 3) {
		printf("  EZSP version negotiation failed\n");
		return -1;
	}

	// Negotiation has to converge. If the NCP answers with a version other
	// than the one we asked for, the exchange is incomplete and it rejects
	// every subsequent command with frame 0xFF -- so re-issue version()
	// with whatever it reported.
	if (out[0] != params[0]) {
		uint8_t want = out[0];

		// version() is only accepted as the first command after a
		// reset -- a second one in the same session comes back 0xFF.
		// So reset and ask again, this time for what it reported.
		if (!cfg.quiet)
			printf("  NCP speaks EZSP v%u, resetting to "
			       "re-negotiate\n", want);

		ncp_reset(p);

		// Still v4 framing for the re-ask; the NCP has just reset and
		// no version is agreed yet.
		cfg.ezsp_version = 4;
		params[0] = want;
		n = ezsp_cmd(spi, p, EZSP_VERSION, params, 1, out, sizeof(out));
		if (n < 3 || out[0] != want) {
			printf("  EZSP version negotiation did not converge "
			       "(wanted v%u)\n", want);
			return -1;
		}
	}

	// v4..v8 are additive: each version appends commands without renumbering
	// the existing ones -- verified against bellows' tables, every id this
	// tool uses is identical across v4 and v8. The frame header above is
	// the entire difference.
	cfg.ezsp_version = out[0];

	if (out[0] > 8) {
		printf("  EZSP v%u is newer than this tool knows about\n",
		       out[0]);
		return -1;
	}

	printf("  EZSP v%u, stack type %u, stack version 0x%04X "
	       "(EmberZNet %u.%u.%u.%u)\n",
	       out[0], out[1], (unsigned)(out[2] | (out[3] << 8)),
	       (out[3] >> 4) & 0xF, out[3] & 0xF,
	       (out[2] >> 4) & 0xF, out[2] & 0xF);

	return out[0];
}



int ezsp_poll_callback(int spi, struct pins *p, uint8_t *payload,
			      size_t max);
void ezsp_apply_config(int spi, struct pins *p, int announce);
int control_query(const char *path, const char *cmd);

// EmberStatus codes worth naming: these are what a failed join actually
// reports, and the difference between them is the whole diagnosis.
const char *ember_status_name(uint8_t s)
{
	switch (s) {
	case 0x00: return "success";
	case 0x90: return "network up";
	case 0x91: return "network down";
	case 0x93: return "not joined";
	case 0x94: return "join failed";
	case 0x96: return "move failed";
	case 0x98: return "cannot join as router";
	case 0x99: return "node id changed";
	case 0x9A: return "pan id changed";
	case 0xAB: return "no beacons heard";
	case 0xAC: return "received key in the clear";
	case 0xAD: return "no network key received";
	case 0xAE: return "no link key received";
	case 0xAF: return "preconfigured key required";
	default:   return "see EmberStatus";
	}
}

// Scan one channel looking for a specific pan id. Returns 1 if found, and
// reports the ext pan id exactly as it appears on the air plus the join flag.
//
// Reading the ext pan id off the air removes the byte-order guesswork: the
// zigbee2mqtt config lists it in the opposite order to the wire format, which
// is not something the host can infer.
int scan_find(int spi, struct pins *p, unsigned channel, uint16_t pan_id,
		     uint8_t ext[8], int *allowing, uint8_t *nwk_update_id)
{
	uint8_t params[8], out[64];
	uint32_t mask = 1u << channel;
	int n, i;

	params[0] = EZSP_ACTIVE_SCAN;
	params[1] = (uint8_t)(mask & 0xFF);
	params[2] = (uint8_t)((mask >> 8) & 0xFF);
	params[3] = (uint8_t)((mask >> 16) & 0xFF);
	params[4] = (uint8_t)((mask >> 24) & 0xFF);
	params[5] = 3;

	n = ezsp_cmd(spi, p, EZSP_START_SCAN, params, 6, out, sizeof(out));
	if (n < 1 || out[0] != EMBER_SUCCESS)
		return -1;

	for (i = 0; i < 80; i++) {
		uint8_t id;

		usleep(250000);
		id = (uint8_t)ezsp_poll_callback(spi, p, out, sizeof(out));

		if (id == EZSP_NETWORK_FOUND_HANDLER) {
			uint16_t found = (uint16_t)(out[1] | (out[2] << 8));

			if (found != pan_id)
				continue;

			memcpy(ext, out + 3, 8);
			*allowing = out[11] ? 1 : 0;
			*nwk_update_id = out[13];
			return 1;
		}

		if (id == EZSP_SCAN_COMPLETE_HANDLER)
			break;
	}

	return 0;
}

// Active scan: ask the radio what networks it can actually hear. This replaces
// guessing at channel, pan id and ext pan id byte order, and allowingJoin
// answers whether permit-join is really open on the coordinator.
int cmd_scan(void)
{
	uint8_t params[8], out[64];
	struct pins p;
	uint32_t mask;
	int spi, n, i, found = 0;

	mask = cfg.channel ? (1u << cfg.channel) : 0x07FFF800u;  // 11..26

	printf("ezsp-spi scan (active, channel mask 0x%08X)\n", mask);

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0)
		goto out;

	params[0] = EZSP_ACTIVE_SCAN;
	params[1] = (uint8_t)(mask & 0xFF);
	params[2] = (uint8_t)((mask >> 8) & 0xFF);
	params[3] = (uint8_t)((mask >> 16) & 0xFF);
	params[4] = (uint8_t)((mask >> 24) & 0xFF);
	params[5] = 4;					// duration exponent

	n = ezsp_cmd(spi, &p, EZSP_START_SCAN, params, 6, out, sizeof(out));
	if (n < 1 || out[0] != EMBER_SUCCESS) {
		printf("  startScan failed (status 0x%02X %s)\n",
		       n >= 1 ? out[0] : 0xFF,
		       ember_status_name(n >= 1 ? out[0] : 0xFF));
		goto out;
	}

	printf("  scanning...\n\n");

	for (i = 0; i < 240; i++) {
		uint8_t id;

		usleep(250000);
		id = (uint8_t)ezsp_poll_callback(spi, &p, out, sizeof(out));

		if (id == EZSP_NETWORK_FOUND_HANDLER) {
			// channel(1) panId(2) extPanId(8) allowingJoin(1)
			// stackProfile(1) nwkUpdateId(1) lqi(1) rssi(1)
			int j;

			found++;
			printf("  network on channel %u\n", out[0]);
			printf("    pan id          0x%04X\n",
			       (unsigned)(out[1] | (out[2] << 8)));
			printf("    ext pan id      ");
			for (j = 3; j < 11; j++)
				printf("%02X%s", out[j], j < 10 ? ":" : "\n");
			printf("    allowing join   %s\n",
			       out[11] ? "YES" : "no");
			printf("    stack profile   %u\n", out[12]);
			printf("    lqi / rssi      %u / %d dBm\n",
			       out[14], (int8_t)out[15]);
			printf("\n");
		} else if (id == EZSP_SCAN_COMPLETE_HANDLER) {
			// channel(1) status(1)
			if (out[1] == EMBER_SUCCESS) {
				printf("  scan complete: %d network(s) found\n",
				       found);
				break;
			}
			printf("  scan failed on channel %u (0x%02X %s)\n",
			       out[0], out[1], ember_status_name(out[1]));
			break;
		}
	}

	if (!found)
		printf("  nothing heard -- wrong channel, out of range, or the "
		       "antenna is not connected\n");

	close(spi);
	pins_close(&p);
	return found ? 0 : 1;

out:
	close(spi);
	pins_close(&p);
	return 1;
}

const char *ezsp_config_name(uint8_t id)
{
	switch (id) {
	case 0x01: return "packet buffer count";
	case 0x02: return "neighbor table size";
	case 0x03: return "aps unicast message count";
	case 0x04: return "binding table size";
	case 0x05: return "address table size";
	case 0x06: return "multicast table size";
	case 0x07: return "route table size";
	case 0x08: return "discovery table size";
	case 0x0C: return "stack profile";
	case 0x0D: return "security level";
	case 0x10: return "max hops";
	case 0x11: return "max end device children";
	case 0x12: return "indirect transmission timeout";
	case 0x13: return "end device poll timeout";
	case 0x17: return "tx power mode";
	case 0x18: return "disable relay";
	case 0x19: return "trust center address cache size";
	case 0x1A: return "source route table size";
	case 0x1E: return "key table size";
	case 0x1F: return "aps ack timeout";
	case 0x20: return "active scan duration";
	case 0x22: return "pan id conflict report threshold";
	case 0x24: return "request key timeout";
	case 0x2A: return "application zdo flags";
	case 0x2B: return "broadcast table size";
	case 0x2C: return "mac filter table size";
	case 0x2D: return "supported networks";
	case 0x36: return "transient key timeout";
	default:   return "";
	}
}


// Open this router's own join window.
//
// A coordinator's permit-join broadcast should reach every router, but relying
// on that makes a failed pairing ambiguous. Opening the window here directly
// removes the doubt, and lets a device pair through this node specifically.
int ezsp_permit(int spi, struct pins *p, unsigned seconds, char *msg,
		       size_t max)
{
	uint8_t param = (uint8_t)(seconds > 254 ? 254 : seconds);
	uint8_t out[8];
	int n = ezsp_cmd(spi, p, EZSP_PERMIT_JOINING, &param, 1, out,
			 sizeof(out));

	if (n < 1 || out[0] != EMBER_SUCCESS) {
		snprintf(msg, max, "permit: failed (status 0x%02X %s)\n",
			 n >= 1 ? out[0] : 0xFF,
			 ember_status_name(n >= 1 ? out[0] : 0xFF));
		return -1;
	}

	snprintf(msg, max, "permit: open for %u s\n", param);
	return 0;
}


// Show whether this node is actually part of the mesh: who its neighbours are,
// which devices have parented onto it, and what routes it holds.
//
// This is the check that matters for a repeater. Joining proves it got on the
// network; neighbours and routes prove it is carrying traffic for it.
int cmd_neighbors(void)
{
	uint8_t out[64];
	struct pins p;
	int spi, n, i;

	// Prefer the running daemon so this can be polled while the node keeps
	// routing -- taking the radio away would reset the very tables we want
	// to read.
	if (control_query(cfg.sock_path, "neighbors\n") == 0)
		return 0;

	printf("ezsp-spi neighbors\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		close(spi);
		pins_close(&p);
		return 1;
	}

	ezsp_apply_config(spi, &p, 0);
	n = ezsp_cmd(spi, &p, EZSP_NETWORK_INIT, NULL, 0, out, sizeof(out));
	if (n >= 1 && out[0] != EMBER_SUCCESS) {
		printf("  not on a network (status 0x%02X)\n", out[0]);
		close(spi);
		pins_close(&p);
		return 1;
	}

	// The stack needs a moment after networkInit before the tables are
	// populated from NVM and the radio has heard anything.
	sleep(3);

	n = ezsp_cmd(spi, &p, EZSP_GET_PARENT_CHILD_PARAMS, NULL, 0, out,
		     sizeof(out));
	if (n >= 11)
		printf("  children: %u\n", out[0]);

	n = ezsp_cmd(spi, &p, EZSP_NEIGHBOR_COUNT, NULL, 0, out, sizeof(out));
	if (n >= 1) {
		int count = out[0];

		printf("\n  neighbour table (%d)\n", count);
		if (!count)
			printf("    empty -- no Zigbee devices heard yet\n");

		for (i = 0; i < count; i++) {
			uint8_t idx = (uint8_t)i;
			int j;

			n = ezsp_cmd(spi, &p, EZSP_GET_NEIGHBOR, &idx, 1, out,
				     sizeof(out));
			if (n < 15 || out[0] != EMBER_SUCCESS)
				continue;

			// status(1) shortId(2) avgLqi(1) inCost(1)
			// outCost(1) age(1) longId(8)
			printf("    0x%04X  lqi %-3u  cost in %u out %u  ",
			       (unsigned)(out[1] | (out[2] << 8)),
			       out[3], out[4], out[5]);
			for (j = 14; j >= 7; j--)
				printf("%02X%s", out[j], j > 7 ? ":" : "\n");
		}
	}

	printf("\n  route table\n");
	for (i = 0; i < 16; i++) {
		uint8_t idx = (uint8_t)i;
		static const char *st[] = { "active", "discovering",
					    "unused", "validating" };

		n = ezsp_cmd(spi, &p, EZSP_GET_ROUTE_TABLE_ENTRY, &idx, 1, out,
			     sizeof(out));
		if (n < 7 || out[0] != EMBER_SUCCESS)
			continue;

		// status(1) destination(2) nextHop(2) status(1) age(1)
		if (out[5] == 2 || (out[1] | (out[2] << 8)) == 0xFFFF)
			continue;			// unused entry

		printf("    dest 0x%04X via 0x%04X  %s\n",
		       (unsigned)(out[1] | (out[2] << 8)),
		       (unsigned)(out[3] | (out[4] << 8)),
		       out[5] < 4 ? st[out[5]] : "?");
	}

	printf("\n");
	close(spi);
	pins_close(&p);
	return 0;
}



// Report what keys this node holds, without printing them.
//
// The question is whether the trust centre and this node still agree on this
// node's trust centre link key. If the coordinator issued a unique key after
// joining and this node never stored it, the coordinator would encrypt its
// tunnelled messages with a key this node cannot read -- which would stop it
// relaying a join while leaving everything else working.
int cmd_keys(void)
{
	static const struct { uint8_t type; const char *name; } want[] = {
		{ EMBER_TRUST_CENTER_LINK_KEY, "trust centre link key" },
		{ EMBER_CURRENT_NETWORK_KEY,   "current network key" },
	};
	uint8_t out[64];
	struct pins p;
	int spi;
	unsigned k;

	printf("ezsp-spi keys\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		close(spi);
		pins_close(&p);
		return 1;
	}

	ezsp_apply_config(spi, &p, 0);
	ezsp_cmd(spi, &p, EZSP_NETWORK_INIT, NULL, 0, out, sizeof(out));

	for (k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
		uint8_t param = want[k].type;
		int n, i;

		printf("\n  %s\n", want[k].name);

		n = ezsp_cmd(spi, &p, EZSP_GET_KEY, &param, 1, out, sizeof(out));
		if (n < 1 || out[0] != EMBER_SUCCESS) {
			printf("    not available (status 0x%02X)\n",
			       n >= 1 ? out[0] : 0xFF);
			continue;
		}

		// status(1) bitmask(2) type(1) key(16) outFC(4) inFC(4)
		// seq(1) partnerEUI64(8)
		if (n < 37) {
			printf("    short reply (%d bytes)\n", n);
			continue;
		}

		printf("    type            0x%02X\n", out[3]);
		printf("    bitmask         0x%04X\n",
		       (unsigned)(out[1] | (out[2] << 8)));

		if (want[k].type == EMBER_TRUST_CENTER_LINK_KEY) {
			int wellknown = (memcmp(out + 4, zigbee_alliance_09,
						16) == 0);

			printf("    key             %s\n", wellknown
			       ? "the well-known ZigBeeAlliance09 key"
			       : "a unique key (not the well-known one)");
		} else {
			printf("    key             (not shown)\n");
		}

		printf("    frame counters  out %u / in %u\n",
		       (unsigned)(out[20] | (out[21] << 8) |
				  (out[22] << 16) | ((unsigned)out[23] << 24)),
		       (unsigned)(out[24] | (out[25] << 8) |
				  (out[26] << 16) | ((unsigned)out[27] << 24)));
		printf("    sequence        %u\n", out[28]);
		printf("    partner         ");
		for (i = 36; i >= 29; i--)
			printf("%02X%s", out[i], i > 29 ? ":" : "\n");
	}

	printf("\n");
	close(spi);
	pins_close(&p);
	return 0;
}


const char *ezsp_policy_name(uint8_t id)
{
	switch (id) {
	case 0x00: return "trust center";
	case 0x01: return "binding modification";
	case 0x02: return "unicast replies";
	case 0x03: return "poll handler";
	case 0x04: return "message contents in callback";
	case 0x05: return "tc key request";
	case 0x06: return "app key request";
	case 0x07: return "packet validate library";
	case 0x08: return "zll";
	default:   return "";
	}
}

// Decisions are per-policy, but the ones that matter here are the "who answers"
// pair on the unicast replies policy.
const char *ezsp_decision_hint(uint8_t policy, uint8_t decision)
{
	if (policy != 0x02)
		return "";

	switch (decision) {
	case 0x20: return "  <- NCP replies (wanted)";
	case 0x21: return "  <- HOST must reply (breaks relaying)";
	default:   return "";
	}
}

// Dump the EZSP policies. Nothing in this tool sets them, so these are whatever
// the NCP booted with -- and if the unicast replies policy says the host will
// supply replies, the NCP waits forever on a host that never answers, which
// would stop this node relaying anything it is asked to forward.
int cmd_policy(void)
{
	uint8_t out[8];
	struct pins p;
	int spi, id;

	printf("ezsp-spi policy\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		close(spi);
		pins_close(&p);
		return 1;
	}

	printf("\n  %-5s %-30s %s\n", "id", "policy", "decision");
	printf("  %-5s %-30s %s\n", "-----", "------", "--------");

	for (id = 0x00; id <= 0x08; id++) {
		uint8_t param = (uint8_t)id;
		int n;

		n = ezsp_cmd(spi, &p, EZSP_GET_POLICY, &param, 1, out,
			     sizeof(out));
		if (n < 2 || out[0] != EMBER_SUCCESS)
			continue;

		printf("  0x%02X  %-30s 0x%02X%s\n", param,
		       ezsp_policy_name(param), out[1],
		       ezsp_decision_hint(param, out[1]));
	}

	close(spi);
	pins_close(&p);
	return 0;
}

// Dump the NCP's EZSP configuration. Many of these default to zero and have to
// be set by the host before the stack will come up. A router with no route or
// neighbor table cannot join, and the stack reports that only as a generic
// JOIN_FAILED, so the values matter more than the error code does.
int cmd_config(void)
{
	uint8_t out[8];
	struct pins p;
	int spi, id;

	printf("ezsp-spi config\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		close(spi);
		pins_close(&p);
		return 1;
	}

	printf("\n  %-5s %-34s %s\n", "id", "name", "value");
	printf("  %-5s %-34s %s\n", "-----", "----", "-----");

	for (id = 0x01; id <= 0x40; id++) {
		uint8_t param = (uint8_t)id;
		int n;

		n = ezsp_cmd(spi, &p, EZSP_GET_CONFIGURATION_VALUE, &param, 1,
			     out, sizeof(out));
		if (n < 3 || out[0] != EMBER_SUCCESS)
			continue;	// not supported on this stack

		printf("  0x%02X  %-34s %u\n", param, ezsp_config_name(param),
		       (unsigned)(out[1] | (out[2] << 8)));
	}

	close(spi);
	pins_close(&p);
	return 0;
}

const char *network_state_name(uint8_t s)
{
	switch (s) {
	case 0x00: return "no network";
	case 0x01: return "joining";
	case 0x02: return "joined";
	case 0x03: return "joined, no parent";
	case 0x04: return "leaving";
	default:   return "unknown";
	}
}

const char *node_type_name(uint8_t t)
{
	switch (t) {
	case 0x00: return "unknown";
	case 0x01: return "coordinator";
	case 0x02: return "router";
	case 0x03: return "end device";
	case 0x04: return "sleepy end device";
	default:   return "unrecognised";
	}
}

// Read-only: what is this radio, and is it already on a network?
int cmd_info(void)
{
	uint8_t out[64];
	struct pins p;
	int spi, n, i;

	printf("ezsp-spi info\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		pins_close(&p);
		close(spi);
		return 1;
	}

	// After a reset the stack is down and reports "no network" whatever is
	// stored, so bring it up first -- with the same configuration the join
	// used, or the stored network will not be found.
	ezsp_apply_config(spi, &p, 0);
	n = ezsp_cmd(spi, &p, EZSP_NETWORK_INIT, NULL, 0, out, sizeof(out));
	if (n >= 1 && out[0] != EMBER_SUCCESS && out[0] != EMBER_NOT_JOINED)
		printf("  networkInit status 0x%02X (%s)\n", out[0],
		       ember_status_name(out[0]));

	n = ezsp_cmd(spi, &p, EZSP_GET_EUI64, NULL, 0, out, sizeof(out));
	if (n >= 8) {
		printf("  EUI64                 ");
		for (i = 7; i >= 0; i--)		// wire order is little endian
			printf("%02X%s", out[i], i ? ":" : "\n");
	}

	n = ezsp_cmd(spi, &p, EZSP_GET_NODE_ID, NULL, 0, out, sizeof(out));
	if (n >= 2)
		printf("  node id               0x%04X\n",
		       (unsigned)(out[0] | (out[1] << 8)));

	n = ezsp_cmd(spi, &p, EZSP_NETWORK_STATE, NULL, 0, out, sizeof(out));
	if (n >= 1)
		printf("  network state         %s (0x%02X)\n",
		       network_state_name(out[0]), out[0]);

	// status(1) nodeType(1) extendedPanId(8) panId(2) txPower(1) channel(1)
	n = ezsp_cmd(spi, &p, EZSP_GET_NETWORK_PARAMETERS, NULL, 0, out,
		     sizeof(out));
	// The parameters are only meaningful when status is EMBER_SUCCESS.
	// Unjoined, the NCP returns EMBER_NOT_JOINED and leaves the rest as
	// uninitialised fill, which is not worth printing.
	if (n >= 14 && out[0] != 0x00) {
		printf("  network parameters    none (status 0x%02X, "
		       "not joined)\n", out[0]);
	} else if (n >= 14) {
		printf("  node type             %s (0x%02X)\n",
		       node_type_name(out[1]), out[1]);
		// Wire order, matching "scan" and the control socket. Reversing
		// it here made the same network look like two different ones.
		printf("  extended pan id       ");
		for (i = 2; i < 10; i++)
			printf("%02X%s", out[i], i < 9 ? ":" : "\n");
		printf("  pan id                0x%04X\n",
		       (unsigned)(out[10] | (out[11] << 8)));
		printf("  radio tx power        %d dBm\n", (int8_t)out[12]);
		printf("  radio channel         %u\n", out[13]);
	}

	close(spi);
	pins_close(&p);

	return 0;
}

// Pull one queued callback. Returns the callback's frame ID, or 0 if none.
int ezsp_poll_callback(int spi, struct pins *p, uint8_t *payload,
			      size_t max)
{
	uint8_t id = 0;
	int n = ezsp_xfer(spi, p, EZSP_CALLBACK, NULL, 0, &id, payload, max);

	if (n < 0 || id == EZSP_NO_CALLBACKS)
		return 0;

	return id;
}

int ezsp_set_config(int spi, struct pins *p, uint8_t id, uint16_t value,
			   const char *what, int announce)
{
	uint8_t params[3] = { id, (uint8_t)(value & 0xFF),
			      (uint8_t)(value >> 8) };
	uint8_t out[4];
	int n = ezsp_cmd(spi, p, EZSP_SET_CONFIGURATION_VALUE, params, 3, out,
			 sizeof(out));

	if (n < 1 || out[0] != EMBER_SUCCESS) {
		printf("  set %s = %u failed (status 0x%02X)\n", what, value,
		       n >= 1 ? out[0] : 0xFF);
		return -1;
	}

	if (announce)
		printf("  set %s = %u\n", what, value);

	return 0;
}
