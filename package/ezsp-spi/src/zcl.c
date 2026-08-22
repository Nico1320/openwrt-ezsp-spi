#include "ezsp-spi.h"

// ---------------------------------------------------------------- ZCL app --
//
// Without this the node is invisible at the application layer: no endpoints, no
// clusters, nothing ever transmitted. A coordinator shows it as an unsupported
// device with linkquality N/A, because linkquality is metadata on frames the
// coordinator *receives*, and this node never sent any.
//
// One endpoint, the Basic cluster, answers to attribute reads, and a periodic
// report so there is something to measure.


// Set transmit power at runtime. The value passed at join time is stored in the
// network parameters; this changes the live radio without a rejoin.
//
// The EM3581 tops out around +8 dBm on its own, so higher values may simply be
// refused -- the stack's answer is reported rather than assumed.
int ezsp_set_power(int spi, struct pins *p, int dbm, char *msg,
			  size_t max)
{
	uint8_t param = (uint8_t)(int8_t)dbm;
	uint8_t out[8];
	int n = ezsp_cmd(spi, p, EZSP_SET_RADIO_POWER, &param, 1, out,
			 sizeof(out));

	if (n < 1 || out[0] != EMBER_SUCCESS) {
		snprintf(msg, max, "power: %d dBm refused (status 0x%02X %s)\n",
			 dbm, n >= 1 ? out[0] : 0xFF,
			 ember_status_name(n >= 1 ? out[0] : 0xFF));
		return -1;
	}

	snprintf(msg, max, "power: set to %d dBm\n", dbm);
	return 0;
}



const char zcl_manufacturer[] = "Linksys";
const char zcl_model[] = "WHW03V2";
const char zcl_swbuild[] = "ezsp-spi 0.1";

// Reporting is jittered. Nodes restored together after a power cut would
// otherwise all fire their first report at once and then tick in lockstep
// forever, which scales badly: ten nodes on a network with a fraction of
// WiFi's bandwidth is a synchronised burst that never disperses.
unsigned report_interval(void)
{
	unsigned base = cfg.report_secs;
	unsigned span = base / 5;		// +/- 20 %

	if (!span)
		return base;

	return base - span + (unsigned)(rand() % (2 * span + 1));
}

unsigned report_count;		// reports handed to the stack
unsigned report_failed;		// rejected or not delivered
time_t report_last;

// -------------------------------------------------------------- identify --
//
// Identify is mandatory on a Home Automation endpoint. The cluster works with
// no indicator at all -- plenty of devices have none -- so the LED is optional
// and named in the config. Nothing is touched unless identify_led is set, and
// whatever the LED was doing before is put back afterwards.

unsigned identify_time;			// seconds left; ZCL attribute 0x0000

#define IDENTIFY_LEDS 4
static char led_path[IDENTIFY_LEDS][64];
static char led_trigger[IDENTIFY_LEDS][32];
static int led_bright[IDENTIFY_LEDS];
static int led_n;
static int led_on;

static void led_write(const char *path, const char *what, const char *val)
{
	char f[96];
	FILE *fp;

	snprintf(f, sizeof(f), "%s/%s", path, what);
	fp = fopen(f, "w");
	if (!fp)
		return;
	fputs(val, fp);
	fclose(fp);
}

static void led_read(const char *path, const char *what, char *buf, size_t max)
{
	char f[96];
	FILE *fp;

	buf[0] = '\0';
	snprintf(f, sizeof(f), "%s/%s", path, what);
	fp = fopen(f, "r");
	if (!fp)
		return;
	if (!fgets(buf, (int)max, fp))
		buf[0] = '\0';
	fclose(fp);
	buf[strcspn(buf, "\n")] = '\0';
}

// The trigger file lists every trigger, with the active one in [brackets].
static void led_active_trigger(const char *path, char *out, size_t max)
{
	char buf[512], *a, *b;

	led_read(path, "trigger", buf, sizeof(buf));
	a = strchr(buf, '[');
	b = a ? strchr(a, ']') : NULL;
	if (!a || !b || (size_t)(b - a) >= max) {
		snprintf(out, max, "none");
		return;
	}
	*b = '\0';
	snprintf(out, max, "%s", a + 1);
}

static void identify_led_claim(void)
{
	char list[256], *tok, *save = NULL;

	led_n = 0;
	if (!cfg.identify_led || !*cfg.identify_led)
		return;

	snprintf(list, sizeof(list), "%s", cfg.identify_led);
	for (tok = strtok_r(list, " ,", &save);
	     tok && led_n < IDENTIFY_LEDS;
	     tok = strtok_r(NULL, " ,", &save)) {
		char b[32];

		snprintf(led_path[led_n], sizeof(led_path[0]),
			 "/sys/class/leds/%s", tok);
		if (access(led_path[led_n], F_OK) != 0) {
			printf("  identify: no led %s\n", tok);
			continue;
		}
		led_active_trigger(led_path[led_n], led_trigger[led_n],
				   sizeof(led_trigger[0]));
		led_read(led_path[led_n], "brightness", b, sizeof(b));
		led_bright[led_n] = atoi(b);
		led_write(led_path[led_n], "trigger", "none");
		led_n++;
	}
}

static void identify_led_release(void)
{
	int i;

	for (i = 0; i < led_n; i++) {
		char v[16];

		snprintf(v, sizeof(v), "%d", led_bright[i]);
		led_write(led_path[i], "brightness", v);
		led_write(led_path[i], "trigger", led_trigger[i]);
	}
	led_n = 0;
	led_on = 0;
}

static void identify_led_toggle(void)
{
	int i;

	led_on = !led_on;
	for (i = 0; i < led_n; i++)
		led_write(led_path[i], "brightness", led_on ? "255" : "0");
}

int identify_active(void)
{
	return identify_time > 0;
}

void identify_start(unsigned secs)
{
	if (!secs) {
		if (identify_time)
			identify_led_release();
		identify_time = 0;
		return;
	}

	if (!identify_time)
		identify_led_claim();

	identify_time = secs;
	printf("  identify for %u s\n", secs);
	fflush(stdout);
}

// Called from the run loop about twice a second.
void identify_tick(void)
{
	static time_t last;
	time_t now = time(NULL);

	if (!identify_time)
		return;

	identify_led_toggle();

	if (now != last) {
		last = now;
		if (--identify_time == 0) {
			identify_led_release();
			printf("  identify done\n");
			fflush(stdout);
		}
	}
}

// The hostname goes in LocationDescription rather than the model, because
// converters match on model: giving each node a different model string would
// need a converter per node, whereas one "WHW03V2" definition covers them all
// and the location still tells them apart.
const char *zcl_location(void)
{
	static char host[32];

	if (!host[0] && gethostname(host, sizeof(host) - 1) != 0)
		snprintf(host, sizeof(host), "openwrt");

	return host;
}

// Endpoints must be registered before the stack comes up.
int ezsp_add_endpoint(int spi, struct pins *p)
{
	uint8_t params[16], out[8];
	size_t n = 0;
	int r;

	params[n++] = ZCL_ENDPOINT;
	params[n++] = ZCL_PROFILE_HA & 0xFF;
	params[n++] = ZCL_PROFILE_HA >> 8;
	params[n++] = ZCL_DEVICE_RANGE_EXTENDER & 0xFF;
	params[n++] = ZCL_DEVICE_RANGE_EXTENDER >> 8;
	params[n++] = 1;			// device version
	params[n++] = 2;			// input cluster count
	params[n++] = 0;			// output cluster count
	params[n++] = ZCL_CLUSTER_BASIC & 0xFF;
	params[n++] = ZCL_CLUSTER_BASIC >> 8;
	params[n++] = ZCL_CLUSTER_IDENTIFY & 0xFF;
	params[n++] = ZCL_CLUSTER_IDENTIFY >> 8;

	r = ezsp_cmd(spi, p, EZSP_ADD_ENDPOINT, params, n, out, sizeof(out));

	// A non-zero status here usually just means the endpoint is already
	// registered from an earlier call, which is harmless.
	return (r >= 1 && out[0] == 0) ? 0 : -1;
}

int zcl_send(int spi, struct pins *p, uint16_t dest, uint16_t cluster,
		    const uint8_t *payload, size_t len)
{
	static uint8_t tag;
	uint8_t params[96], out[8];
	size_t n = 0;

	if (len > 64)
		return -1;

	params[n++] = 0x00;			// EMBER_OUTGOING_DIRECT
	params[n++] = (uint8_t)(dest & 0xFF);
	params[n++] = (uint8_t)(dest >> 8);

	// EmberApsFrame: profileId, clusterId, srcEp, dstEp, options,
	// groupId, sequence
	params[n++] = ZCL_PROFILE_HA & 0xFF;
	params[n++] = ZCL_PROFILE_HA >> 8;
	params[n++] = (uint8_t)(cluster & 0xFF);
	params[n++] = (uint8_t)(cluster >> 8);
	params[n++] = ZCL_ENDPOINT;
	params[n++] = ZCL_ENDPOINT;
	params[n++] = 0x40;			// RETRY |
	params[n++] = 0x01;			// ENABLE_ROUTE_DISCOVERY
	params[n++] = 0x00;
	params[n++] = 0x00;			// groupId
	params[n++] = 0x00;			// sequence

	params[n++] = tag++;			// messageTag
	params[n++] = (uint8_t)len;		// LVBytes
	memcpy(params + n, payload, len);
	n += len;

	if (ezsp_cmd(spi, p, EZSP_SEND_UNICAST, params, n, out, sizeof(out)) < 1)
		return -1;

	return out[0] == EMBER_SUCCESS ? 0 : -1;
}

// Append one Basic-cluster attribute record to a Read Attributes Response.
size_t zcl_append_attr(uint8_t *b, size_t at, size_t max, uint16_t id)
{
	const char *str = NULL;
	uint8_t type, val = 0;

	switch (id) {
	case 0x0000: type = 0x20; val = 3;    break;	// ZCLVersion
	case 0x0001: type = 0x20; val = 1;    break;	// ApplicationVersion
	case 0x0002: type = 0x20; val = 0x58; break;	// StackVersion (5.8)
	case 0x0003: type = 0x20; val = 2;    break;	// HWVersion
	case 0x0004: type = 0x42; str = zcl_manufacturer; break;
	case 0x0005: type = 0x42; str = zcl_model;        break;
	case 0x0007: type = 0x30; val = 0x01; break;	// PowerSource: mains
	case 0x0010: type = 0x42; str = zcl_location();   break;
	case 0x4000: type = 0x42; str = zcl_swbuild;      break;	// SWBuildID
	default:
		if (at + 3 > max)
			return at;
		b[at++] = (uint8_t)(id & 0xFF);
		b[at++] = (uint8_t)(id >> 8);
		b[at++] = 0x86;				// UNSUPPORTED_ATTRIBUTE
		return at;
	}

	if (str) {
		size_t l = strlen(str);

		if (at + 5 + l > max)
			return at;
		b[at++] = (uint8_t)(id & 0xFF);
		b[at++] = (uint8_t)(id >> 8);
		b[at++] = 0x00;				// SUCCESS
		b[at++] = type;
		b[at++] = (uint8_t)l;
		memcpy(b + at, str, l);
		at += l;
	} else {
		if (at + 5 > max)
			return at;
		b[at++] = (uint8_t)(id & 0xFF);
		b[at++] = (uint8_t)(id >> 8);
		b[at++] = 0x00;
		b[at++] = type;
		b[at++] = val;
	}

	return at;
}

// Answer a Read Attributes on the Basic cluster. This is what lets the
// coordinator's interview read modelId and manufacturerName instead of
// finding nothing.
void zcl_handle_incoming(int spi, struct pins *p, const uint8_t *m,
				size_t len)
{
	uint8_t resp[96], fc, seq, cmd;
	const uint8_t *zcl;
	uint16_t profile, cluster, sender;
	size_t zlen, i, at = 0;

	// type(1) apsFrame(11) lqi(1) rssi(1) sender(2) binding(1)
	// address(1) length(1) contents(...)
	if (len < 20)
		return;

	profile = (uint16_t)(m[1] | (m[2] << 8));
	cluster = (uint16_t)(m[3] | (m[4] << 8));
	sender  = (uint16_t)(m[14] | (m[15] << 8));
	zlen    = m[18];
	zcl     = m + 19;

	if (zlen < 3 || len < 19 + zlen)
		return;

	// ZDO payload: seq(1) duration(1) tc_significance(1).
	if (profile == ZCL_PROFILE_ZDO &&
	    cluster == ZDO_MGMT_PERMIT_JOINING_REQ) {
		unsigned dur = zcl[1];

		if (dur)
			printf("  permit-join OPENED for %u s by 0x%04X\n",
			       dur, sender);
		else
			printf("  permit-join closed by 0x%04X\n", sender);
		fflush(stdout);
		return;
	}

	fc = zcl[0];
	i = 1;
	if (fc & 0x04)
		i += 2;				// manufacturer specific
	if (i + 1 >= zlen)
		return;
	seq = zcl[i++];
	cmd = zcl[i++];

	if (cluster == ZCL_CLUSTER_IDENTIFY) {
		// Cluster-specific commands.
		if ((fc & 0x03) == 0x01) {
			if (cmd == ZCL_CMD_IDENTIFY && i + 1 < zlen) {
				identify_start((unsigned)(zcl[i] |
							  (zcl[i + 1] << 8)));
			} else if (cmd == ZCL_CMD_IDENTIFY_QUERY) {
				if (!identify_time)
					return;	// silent unless identifying
				resp[at++] = 0x19;	// cluster specific, s->c
				resp[at++] = seq;
				resp[at++] = 0x00;	// IdentifyQueryResponse
				resp[at++] = (uint8_t)(identify_time & 0xFF);
				resp[at++] = (uint8_t)(identify_time >> 8);
				zcl_send(spi, p, sender, ZCL_CLUSTER_IDENTIFY,
					 resp, at);
			}
			return;
		}

		// Profile-wide Read Attributes: only IdentifyTime exists.
		if ((fc & 0x03) != 0x00 || cmd != 0x00)
			return;

		resp[at++] = 0x18;
		resp[at++] = seq;
		resp[at++] = 0x01;		// Read Attributes Response
		for (; i + 1 < zlen; i += 2) {
			uint16_t id = (uint16_t)(zcl[i] | (zcl[i + 1] << 8));

			if (at + 6 > sizeof(resp))
				break;
			resp[at++] = (uint8_t)(id & 0xFF);
			resp[at++] = (uint8_t)(id >> 8);
			if (id == 0x0000) {
				resp[at++] = 0x00;		// SUCCESS
				resp[at++] = 0x21;		// uint16
				resp[at++] = (uint8_t)(identify_time & 0xFF);
				resp[at++] = (uint8_t)(identify_time >> 8);
			} else {
				resp[at++] = 0x86;	// UNSUPPORTED_ATTRIBUTE
			}
		}
		zcl_send(spi, p, sender, ZCL_CLUSTER_IDENTIFY, resp, at);
		return;
	}

	if (cluster != ZCL_CLUSTER_BASIC)
		return;

	if ((fc & 0x03) != 0x00 || cmd != 0x00)
		return;				// only profile-wide Read Attributes

	resp[at++] = 0x18;			// server->client, no default resp
	resp[at++] = seq;
	resp[at++] = 0x01;			// Read Attributes Response

	for (; i + 1 < zlen; i += 2)
		at = zcl_append_attr(resp, at, sizeof(resp),
				     (uint16_t)(zcl[i] | (zcl[i + 1] << 8)));

	if (!cfg.quiet) {
		printf("  zcl read from 0x%04X, answering %zu bytes\n",
		       sender, at);
		fflush(stdout);
	}

	zcl_send(spi, p, sender, ZCL_CLUSTER_BASIC, resp, at);
}

// Unsolicited attribute report to the coordinator. The content barely matters;
// what matters is that a frame arrives, because that is what carries the LQI
// the coordinator reports as linkquality.
void zcl_report(int spi, struct pins *p)
{
	uint8_t z[8];
	size_t n = 0;

	z[n++] = 0x18;
	z[n++] = 0;				// sequence
	z[n++] = 0x0A;				// Report Attributes
	z[n++] = 0x00;
	z[n++] = 0x00;				// attribute 0x0000 ZCLVersion
	z[n++] = 0x20;				// uint8
	z[n++] = 3;

	// Whether the stack accepted it for sending. Actual delivery shows up
	// later as messageSentHandler, which is the part that matters on a node
	// with no direct route to the coordinator.
	report_last = time(NULL);

	if (zcl_send(spi, p, 0x0000, ZCL_CLUSTER_BASIC, z, n) < 0) {
		report_failed++;
		printf("  report: stack refused to send\n");
		fflush(stdout);
	} else {
		report_count++;
	}
}

// The one true configuration. Every command must apply exactly this set.
//
// These values size the NCP's tables and its NVM layout, so networkInit can
// only restore a stored network when the host presents the same configuration
// the network was joined under. Presenting a different one makes networkInit
// report NOT_JOINED even though the network is still sitting in NVM -- which
// looks exactly like never having joined at all.
void ezsp_apply_config(int spi, struct pins *p, int announce)
{
	ezsp_set_config(spi, p, EZSP_CONFIG_STACK_PROFILE, 2,
			"stack profile", announce);
	ezsp_set_config(spi, p, EZSP_CONFIG_SECURITY_LEVEL, 5,
			"security level", announce);
	ezsp_set_config(spi, p, EZSP_CONFIG_KEY_TABLE_SIZE, 4,
			"key table size", announce);
	ezsp_set_config(spi, p, EZSP_CONFIG_TC_ADDRESS_CACHE_SIZE, 2,
			"trust center address cache", announce);

	// Endpoints must also be registered before the stack starts.
	if (ezsp_add_endpoint(spi, p) == 0 && announce)
		printf("  registered endpoint %u (range extender, Basic)\n",
		       ZCL_ENDPOINT);
}

// Join an existing Zigbee network as a router.
//
// Once joined, the EmberZNet stack on the NCP routes and repeats at the network
// layer by itself -- the host is not in the data path. All this has to do is
// get the node onto the network and then stay alive.
int cmd_join(void)
{
	uint8_t params[48], out[64];
	struct pins p;
	uint16_t bitmask;
	uint8_t nwk_update_id = 0;
	int spi, n, i, attempts_left;

	if (!cfg.channel || !cfg.have_pan_id)
		die("join needs --channel and --pan-id "
		    "(both come from your coordinator)");

	if (!cfg.have_ext_pan_id && cfg.no_precheck)
		die("--no-precheck means the ext pan id cannot be discovered, "
		    "so --ext-pan-id is required");

	printf("ezsp-spi join: channel %u, pan id 0x%04X\n",
	       cfg.channel, cfg.pan_id);

	attempts_left = (int)cfg.join_retries;

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0)
		goto fail;

	// Look at the network before trying to join it. This catches the two
	// failures that otherwise present as an unexplained JOIN_FAILED sixty
	// seconds later: permit-join being closed, and a byte-swapped ext pan
	// id. Both are read directly off the air.
	if (!cfg.no_precheck) {
		uint8_t ext[8];
		int allowing = 0, r;

		printf("  checking the network is joinable...\n");
		r = scan_find(spi, &p, cfg.channel, cfg.pan_id, ext, &allowing,
			      &nwk_update_id);

		if (r < 0) {
			printf("  scan failed; skipping the check\n");
		} else if (r == 0) {
			printf("  pan 0x%04X not heard on channel %u -- wrong "
			       "parameters, or out of range\n",
			       cfg.pan_id, cfg.channel);
			goto fail;
		} else {
			printf("  found it, ext pan id ");
			for (i = 0; i < 8; i++)
				printf("%02X%s", ext[i], i < 7 ? ":" : "\n");

			if (cfg.have_ext_pan_id &&
			    memcmp(ext, cfg.ext_pan_id, 8) != 0) {
				printf("  note: differs from --ext-pan-id; "
				       "using what the radio heard\n");
			}
			memcpy(cfg.ext_pan_id, ext, 8);
			cfg.have_ext_pan_id = 1;

			if (!allowing) {
				printf("\n  permit-join is CLOSED on this "
				       "network.\n");
				printf("  Open permit-join on the coordinator and re-run "
				       "within the join window.\n");
				goto fail;
			}
			printf("  permit-join is open, nwk update id %u\n",
			       nwk_update_id);
		}
	}

	// Configuration has to be set while the stack is down.
	//
	// The key table and trust centre address cache both default to zero on
	// this NCP, which leaves the node nowhere to put the trust centre link
	// key it needs to finish authenticating. Clients set these for the
	// same reason.
	ezsp_apply_config(spi, &p, 1);

	// Read stack profile back: setConfigurationValue reporting success is
	// not proof the value took.
	{
		uint8_t id = EZSP_CONFIG_STACK_PROFILE;

		n = ezsp_cmd(spi, &p, EZSP_GET_CONFIGURATION_VALUE, &id, 1, out,
			     sizeof(out));
		if (n >= 3 && out[0] == EMBER_SUCCESS && out[1] != 2) {
			printf("  stack profile read back as %u, not 2 -- "
			       "the stack will not join\n", out[1]);
			goto fail;
		}
	}

	// Joining with the well-known global trust centre link key: the
	// coordinator hands over the network key encrypted with it. No
	// install code, which this stack could not do anyway.
	bitmask = cfg.sec_bitmask;
	memset(params, 0, sizeof(params));
	params[0] = (uint8_t)(bitmask & 0xFF);
	params[1] = (uint8_t)(bitmask >> 8);
	memcpy(params + 2, zigbee_alliance_09, 16);	// preconfigured key
	// params[18..33] network key: left zero, the trust centre supplies it
	// params[34..41] trust centre EUI64: left zero, learned on join

	n = ezsp_cmd(spi, &p, EZSP_SET_INITIAL_SECURITY_STATE, params, 42, out,
		     sizeof(out));
	if (n < 1 || out[0] != EMBER_SUCCESS) {
		printf("  setInitialSecurityState failed (status 0x%02X)\n",
		       n >= 1 ? out[0] : 0xFF);
		goto fail;
	}
	printf("  security state set (bitmask 0x%04X, global link key)\n",
	       bitmask);

	// joinNetwork: nodeType, then EmberNetworkParameters.
	//
	// Retried on a transient failure. Association can succeed while the
	// trust centre's key never arrives (0xAD), which is a lost frame rather
	// than a misconfiguration -- and gets likelier the more hops separate
	// this node from the coordinator.
attempt:
	memset(params, 0, sizeof(params));
	params[0] = EMBER_NODE_TYPE_ROUTER;
	memcpy(params + 1, cfg.ext_pan_id, 8);		// extendedPanId
	params[9]  = (uint8_t)(cfg.pan_id & 0xFF);	// panId
	params[10] = (uint8_t)(cfg.pan_id >> 8);
	params[11] = (uint8_t)cfg.tx_power;		// radioTxPower
	params[12] = (uint8_t)cfg.channel;		// radioChannel
	params[13] = EMBER_USE_MAC_ASSOCIATION;		// joinMethod
	params[16] = nwk_update_id;			// from the beacon
	// params[14..15] nwkManagerId, [17..20] channels

	n = ezsp_cmd(spi, &p, EZSP_JOIN_NETWORK, params, 21, out, sizeof(out));
	if (n < 1 || out[0] != EMBER_SUCCESS) {
		printf("  joinNetwork rejected (status 0x%02X)\n",
		       n >= 1 ? out[0] : 0xFF);
		goto fail;
	}
	printf("  join started, waiting for the network to come up...\n");

	// The result arrives asynchronously as stackStatusHandler.
	for (i = 0; i < (int)cfg.join_timeout * 4; i++) {
		uint8_t id;

		usleep(250000);

		id = (uint8_t)ezsp_poll_callback(spi, &p, out, sizeof(out));
		if (!id)
			continue;

		if (id == EZSP_STACK_STATUS_HANDLER && n >= 0) {
			if (out[0] == EMBER_NETWORK_UP) {
				printf("\n  NETWORK UP -- joined as a router\n");
				printf("  the NCP now repeats for this network "
				       "on its own\n");
				close(spi);
				pins_close(&p);
				return 0;
			}
			printf("  stack status 0x%02X (%s)\n", out[0],
			       ember_status_name(out[0]));

			// A terminal failure will not improve by waiting, but
			// some are worth a fresh attempt rather than giving up.
			if (out[0] == 0x94 || out[0] == 0xAB ||
			    out[0] == 0x98 || out[0] == 0xAD ||
			    out[0] == 0xAF) {
				int transient = (out[0] == 0xAD ||
						 out[0] == 0x94);

				if (transient && attempts_left-- > 0) {
					printf("  retrying (%d left; keep "
					       "permit-join open)\n",
					       attempts_left + 1);
					sleep(5);
					goto attempt;
				}
				break;
			}
		} else if (cfg.verbose) {
			printf("  callback 0x%02X\n", id);
		}
	}

	printf("\n  no NETWORK_UP. Check \"config\" -- a router needs non-zero\n"
	       "  route, neighbor and packet buffer tables, and several EZSP\n"
	       "  values default to zero until the host sets them.\n");

fail:
	close(spi);
	pins_close(&p);
	return 1;
}

// Undo a join: leave the network and clear the stored state. Needed if the join
// lands on the wrong network, or to start over.
int cmd_leave(void)
{
	uint8_t out[64];
	struct pins p;
	int spi, n, i;

	printf("ezsp-spi leave\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0)
		goto out;

	// Same configuration as the join, or networkInit cannot find the
	// stored network and there would be nothing to leave.
	ezsp_apply_config(spi, &p, 0);

	// The stack has to be up before it can be told to leave.
	n = ezsp_cmd(spi, &p, EZSP_NETWORK_INIT, NULL, 0, out, sizeof(out));
	if (n >= 1 && out[0] != EMBER_SUCCESS)
		printf("  networkInit status 0x%02X (already off a network?)\n",
		       out[0]);

	n = ezsp_cmd(spi, &p, EZSP_LEAVE_NETWORK, NULL, 0, out, sizeof(out));
	if (n < 1 || out[0] != EMBER_SUCCESS) {
		printf("  leaveNetwork failed (status 0x%02X)\n",
		       n >= 1 ? out[0] : 0xFF);
		goto out;
	}

	for (i = 0; i < 40; i++) {
		uint8_t id;

		usleep(250000);
		id = (uint8_t)ezsp_poll_callback(spi, &p, out, sizeof(out));

		if (id == EZSP_STACK_STATUS_HANDLER &&
		    out[0] == EMBER_NETWORK_DOWN) {
			printf("  network down -- left, NVM state cleared\n");
			close(spi);
			pins_close(&p);
			return 0;
		}
	}

	printf("  no NETWORK_DOWN callback; check with \"info\"\n");

out:
	close(spi);
	pins_close(&p);
	return 1;
}
