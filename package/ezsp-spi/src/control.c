#include "ezsp-spi.h"

// ------------------------------------------------------------ control API --
//
// The daemon holds the GPIO lines exclusively for as long as it runs, so no
// second process can query the radio directly. Rather than make callers stop
// the service -- which drops the node off the mesh -- the daemon answers on a
// unix socket. Replies are "key: value" lines, which stay readable by hand and
// parse trivially from LuCI or a shell.

int control_listen(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	if (!path || !*path)
		return -1;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	unlink(path);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, 4) < 0) {
		fprintf(stderr, "control socket %s: %s\n", path,
			strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

// Collect the current network state into the reply buffer.
void control_status(int spi, struct pins *p, char *buf, size_t max)
{
	uint8_t out[64];
	size_t at = 0;
	int n, i;

	n = ezsp_cmd(spi, p, EZSP_NETWORK_STATE, NULL, 0, out, sizeof(out));
	at += snprintf(buf + at, max - at, "state: %s\n",
		       n >= 1 ? network_state_name(out[0]) : "unknown");

	n = ezsp_cmd(spi, p, EZSP_GET_EUI64, NULL, 0, out, sizeof(out));
	if (n >= 8) {
		at += snprintf(buf + at, max - at, "eui64: ");
		for (i = 7; i >= 0; i--)
			at += snprintf(buf + at, max - at, "%02X%s", out[i],
				       i ? ":" : "\n");
	}

	n = ezsp_cmd(spi, p, EZSP_GET_NODE_ID, NULL, 0, out, sizeof(out));
	if (n >= 2)
		at += snprintf(buf + at, max - at, "node_id: 0x%04X\n",
			       (unsigned)(out[0] | (out[1] << 8)));

	n = ezsp_cmd(spi, p, EZSP_GET_NETWORK_PARAMETERS, NULL, 0, out,
		     sizeof(out));
	if (n >= 14 && out[0] == EMBER_SUCCESS) {
		at += snprintf(buf + at, max - at, "node_type: %s\n",
			       node_type_name(out[1]));
		at += snprintf(buf + at, max - at, "ext_pan_id: ");
		for (i = 2; i < 10; i++)
			at += snprintf(buf + at, max - at, "%02X%s", out[i],
				       i < 9 ? ":" : "\n");
		at += snprintf(buf + at, max - at, "pan_id: 0x%04X\n",
			       (unsigned)(out[10] | (out[11] << 8)));
		at += snprintf(buf + at, max - at, "tx_power: %d\n",
			       (int8_t)out[12]);
		at += snprintf(buf + at, max - at, "channel: %u\n", out[13]);
	}

	at += snprintf(buf + at, max - at, "reports: %u sent, %u failed\n",
		       report_count, report_failed);
	if (report_last)
		at += snprintf(buf + at, max - at, "last_report: %lds ago\n",
			       (long)(time(NULL) - report_last));

	snprintf(buf + at, max - at, "ok\n");
}

void control_accept(int listen_fd, int spi, struct pins *p)
{
	char buf[1024], req[64];
	ssize_t got;
	int fd;

	fd = accept(listen_fd, NULL, NULL);
	if (fd < 0)
		return;

	got = read(fd, req, sizeof(req) - 1);
	if (got <= 0) {
		close(fd);
		return;
	}
	req[got] = '\0';
	req[strcspn(req, "\r\n")] = '\0';

	buf[0] = '\0';

	if (!strcmp(req, "status")) {
		control_status(spi, p, buf, sizeof(buf));
	} else if (!strcmp(req, "scan")) {
		// Let the stack answer rather than assuming: an active scan
		// takes the radio off channel, and EmberZNet normally refuses
		// it while the network is up.
		uint8_t params[6], out[32];
		uint32_t mask = 0x07FFF800u;
		int n;

		params[0] = EZSP_ACTIVE_SCAN;
		params[1] = (uint8_t)(mask & 0xFF);
		params[2] = (uint8_t)((mask >> 8) & 0xFF);
		params[3] = (uint8_t)((mask >> 16) & 0xFF);
		params[4] = (uint8_t)((mask >> 24) & 0xFF);
		params[5] = 3;

		n = ezsp_cmd(spi, p, EZSP_START_SCAN, params, 6, out,
			     sizeof(out));
		snprintf(buf, sizeof(buf),
			 "scan: refused while joined (status 0x%02X %s)\n"
			 "hint: stop the service to scan, it leaves the mesh\n",
			 n >= 1 ? out[0] : 0xFF,
			 ember_status_name(n >= 1 ? out[0] : 0xFF));
	} else if (!strcmp(req, "neighbors")) {
		uint8_t o[64];
		size_t at = 0;
		int n2, cnt, i2;

		// outCost is the mesh's own view of how well a neighbour hears
		// *us*, learned from link status exchanges. That makes it the
		// honest measure of this node's transmit path -- unlike a
		// coordinator-reported linkquality, which only reflects the
		// last hop and so says nothing about a multi-hop node.
		n2 = ezsp_cmd(spi, p, EZSP_NEIGHBOR_COUNT, NULL, 0, o, sizeof(o));
		cnt = (n2 >= 1) ? o[0] : 0;
		at += snprintf(buf + at, sizeof(buf) - at, "neighbors: %d\n", cnt);

		for (i2 = 0; i2 < cnt && at < sizeof(buf) - 80; i2++) {
			uint8_t idx = (uint8_t)i2;

			n2 = ezsp_cmd(spi, p, EZSP_GET_NEIGHBOR, &idx, 1, o,
				      sizeof(o));
			if (n2 < 15 || o[0] != EMBER_SUCCESS)
				continue;

			at += snprintf(buf + at, sizeof(buf) - at,
				       "neighbor: 0x%04X lqi %u in %u out %u\n",
				       (unsigned)(o[1] | (o[2] << 8)),
				       o[3], o[4], o[5]);
		}

		// Routes matter as much as neighbours here: a coordinator's
		// link quality reading describes the last hop, so a node that
		// reaches it through a relay is not being measured at all.
		for (i2 = 0; i2 < 16 && at < sizeof(buf) - 80; i2++) {
			uint8_t idx = (uint8_t)i2;

			n2 = ezsp_cmd(spi, p, EZSP_GET_ROUTE_TABLE_ENTRY, &idx,
				      1, o, sizeof(o));
			if (n2 < 7 || o[0] != EMBER_SUCCESS || o[5] == 2)
				continue;
			if ((o[1] | (o[2] << 8)) == 0xFFFF)
				continue;		// empty slot

			at += snprintf(buf + at, sizeof(buf) - at,
				       "route: dest 0x%04X via 0x%04X state %u\n",
				       (unsigned)(o[1] | (o[2] << 8)),
				       (unsigned)(o[3] | (o[4] << 8)), o[5]);
		}
	} else if (!strcmp(req, "children")) {
		uint8_t out2[64];
		size_t at = 0;
		int n2, c, i2;

		n2 = ezsp_cmd(spi, p, EZSP_GET_PARENT_CHILD_PARAMS, NULL, 0,
			      out2, sizeof(out2));
		c = (n2 >= 1) ? out2[0] : 0;
		at += snprintf(buf + at, sizeof(buf) - at, "children: %d\n", c);

		for (i2 = 0; i2 < c && i2 < 8; i2++) {
			uint8_t idx = (uint8_t)i2;
			int j;

			n2 = ezsp_cmd(spi, p, EZSP_GET_CHILD_DATA, &idx, 1,
				      out2, sizeof(out2));
			if (n2 < 12 || out2[0] != EMBER_SUCCESS)
				continue;

			at += snprintf(buf + at, sizeof(buf) - at,
				       "child: 0x%04X ",
				       (unsigned)(out2[1] | (out2[2] << 8)));
			for (j = 10; j >= 3; j--)
				at += snprintf(buf + at, sizeof(buf) - at,
					       "%02X%s", out2[j],
					       j > 3 ? ":" : "\n");
		}
	} else if (!strncmp(req, "power", 5)) {
		int dbm = 8;

		if (req[5] == ' ')
			dbm = (int)strtol(req + 6, NULL, 10);

		ezsp_set_power(spi, p, dbm, buf, sizeof(buf));
	} else if (!strncmp(req, "permit", 6)) {
		unsigned secs = 180;

		if (req[6] == ' ')
			secs = strtoul(req + 7, NULL, 10);

		ezsp_permit(spi, p, secs, buf, sizeof(buf));
	} else {
		snprintf(buf, sizeof(buf),
			 "error: unknown command\n"
			 "commands: status scan children neighbors permit [seconds]\n"
			 "          power [dbm]\n");
	}

	// If the client went away mid-reply there is nothing useful to do.
	(void)!write(fd, buf, strlen(buf));

	close(fd);
}

// Ask a running daemon instead of touching the radio. Returns 0 if a daemon
// answered, -1 if there is no socket to talk to.
int control_query(const char *path, const char *cmd)
{
	struct sockaddr_un addr;
	char buf[1024];
	ssize_t got;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	if (write(fd, cmd, strlen(cmd)) < 0) {
		close(fd);
		return -1;
	}

	while ((got = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[got] = '\0';
		fputs(buf, stdout);
	}

	close(fd);
	return 0;
}

// Always emits the same "key: value" shape, whether a daemon answered or we
// had to take the radio ourselves. Callers parse this -- the LuCI backend
// among them -- so the two paths must not drift in format.
int cmd_status(void)
{
	char buf[512];
	struct pins p;
	uint8_t out[64];
	int spi, n;

	if (control_query(cfg.sock_path, "status\n") == 0)
		return 0;

	// No daemon: bring the stack up ourselves and report the same fields.
	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		printf("state: unavailable\n");
		close(spi);
		pins_close(&p);
		return 1;
	}

	ezsp_apply_config(spi, &p, 0);
	n = ezsp_cmd(spi, &p, EZSP_NETWORK_INIT, NULL, 0, out, sizeof(out));
	if (n >= 1 && out[0] != EMBER_SUCCESS && out[0] != EMBER_NOT_JOINED)
		printf("networkinit: 0x%02X\n", out[0]);

	control_status(spi, &p, buf, sizeof(buf));
	fputs(buf, stdout);

	close(spi);
	pins_close(&p);
	return 0;
}

// Open the join window on this node. Goes through the daemon when one is
// running, so the node keeps routing while the window is open.
int cmd_permit(const char *arg)
{
	unsigned secs = arg ? strtoul(arg, NULL, 10) : 180;
	char req[64], msg[128];
	struct pins p;
	uint8_t out[64];
	int spi, n;

	if (!secs)
		secs = 180;

	snprintf(req, sizeof(req), "permit %u\n", secs);
	if (control_query(cfg.sock_path, req) == 0)
		return 0;

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
		printf("not on a network (status 0x%02X)\n", out[0]);
		close(spi);
		pins_close(&p);
		return 1;
	}

	n = ezsp_permit(spi, &p, secs, msg, sizeof(msg));
	fputs(msg, stdout);

	close(spi);
	pins_close(&p);
	return n < 0 ? 1 : 0;
}

// Change transmit power on the live radio, through the daemon so the node keeps
// routing while it happens.
int cmd_power(const char *arg)
{
	int dbm = arg ? (int)strtol(arg, NULL, 10) : 8;
	char req[64], msg[128];
	struct pins p;
	uint8_t out[64];
	int spi, n;

	snprintf(req, sizeof(req), "power %d\n", dbm);
	if (control_query(cfg.sock_path, req) == 0)
		return 0;

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

	n = ezsp_set_power(spi, &p, dbm, msg, sizeof(msg));
	fputs(msg, stdout);

	close(spi);
	pins_close(&p);
	return n < 0 ? 1 : 0;
}

// Check an EBL image against the chip before anyone writes it.
//
// The EBL header carries the target's platform/micro/phy at 0x28 and the stack
// version at 0x2C, and the NCP reports its own platform/micro/phy over EZSP.
// Comparing the two is the difference between "this image is for this chip"
// and "this image is 150 KB of something". Cheap, and the only check that
// exists before a flash starts.
int cmd_verify(const char *path)
{
	static const uint8_t ebl_sig[4] = { 0x02, 0x02, 0xE3, 0x50 };
	uint8_t hdr[0x40], out[16];
	struct pins p;
	FILE *f;
	int spi, n, ok = 1;
	unsigned ver;

	if (!path)
		die("verify needs a path to an .ebl file");

	f = fopen(path, "rb");
	if (!f)
		die("open %s: %s", path, strerror(errno));
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
		fclose(f);
		die("%s: too short to be an EBL image", path);
	}
	fclose(f);

	printf("image: %s\n", path);

	if (memcmp(hdr + 4, ebl_sig, sizeof(ebl_sig)) != 0) {
		printf("  header signature   %02X %02X %02X %02X  "
		       "NOT an EM35x EBL image\n",
		       hdr[4], hdr[5], hdr[6], hdr[7]);
		return 1;
	}
	printf("  header signature   ok (EM35x EBL)\n");

	ver = (unsigned)(hdr[0x2C] | (hdr[0x2D] << 8));
	printf("  stack version      0x%04X (EmberZNet %u.%u.%u.%u)\n", ver,
	       (ver >> 12) & 0xF, (ver >> 8) & 0xF,
	       (ver >> 4) & 0xF, ver & 0xF);
	printf("  built for          platform %u / micro %u / phy %u\n",
	       hdr[0x28], hdr[0x29], hdr[0x2A]);

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		close(spi);
		pins_close(&p);
		printf("\n  cannot ask the NCP what it is -- not verified\n");
		return 1;
	}

	n = ezsp_cmd(spi, &p, EZSP_GET_BOOTLOADER_VERSION, NULL, 0, out,
		     sizeof(out));
	if (n < 5) {
		printf("\n  NCP did not report its platform -- not verified\n");
		close(spi);
		pins_close(&p);
		return 1;
	}

	printf("  this chip is       platform %u / micro %u / phy %u\n",
	       out[2], out[3], out[4]);

	if (out[2] != hdr[0x28] || out[3] != hdr[0x29] || out[4] != hdr[0x2A]) {
		printf("\n  MISMATCH -- this image is not for this chip. "
		       "Do not flash it.\n");
		ok = 0;
	} else {
		printf("\n  match: safe to flash on this chip\n");
	}

	close(spi);
	pins_close(&p);
	return ok ? 0 : 1;
}

// Report the standalone bootloader, read-only. Entering it is a separate and
// deliberate act; this only asks what is there.
int cmd_bootloader(void)
{
	uint8_t out[16];
	struct pins p;
	int spi, n;

	printf("ezsp-spi bootloader (read-only)\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0) {
		close(spi);
		pins_close(&p);
		return 1;
	}

	n = ezsp_cmd(spi, &p, EZSP_GET_BOOTLOADER_VERSION, NULL, 0, out,
		     sizeof(out));
	if (n < 5) {
		printf("  no answer -- no standalone bootloader reported\n");
		close(spi);
		pins_close(&p);
		return 1;
	}

	{
		unsigned ver = (unsigned)(out[0] | (out[1] << 8));

		printf("  bootloader version   0x%04X%s\n", ver,
		       ver == 0xFFFF ? "  (none / not present)" : "");
		printf("  platform             %u\n", out[2]);
		printf("  micro                %u\n", out[3]);
		printf("  phy                  %u\n", out[4]);
	}

	printf("\n  Note: the standalone bootloader lives in its own flash\n"
	       "  region. Writing an application image does not touch it, so a\n"
	       "  failed application flash leaves the bootloader able to retry.\n");

	close(spi);
	pins_close(&p);
	return 0;
}

// Restore the stored network on boot and keep servicing callbacks. The NCP
// remembers its network in NVM, so after a successful join this is all that is
// needed to bring the radio back up as a router.
int cmd_run(void)
{
	uint8_t out[64];
	struct pins p;
	int spi, n, listen_fd = -1;
	time_t next_report = 0;

	printf("ezsp-spi run\n");

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	if (ezsp_init(spi, &p) < 0)
		goto out;

	ezsp_apply_config(spi, &p, 1);

	// Seed from this node's own EUI64 so a fleet restored together still
	// gets different schedules -- boot time and pid can coincide, the
	// EUI64 cannot.
	{
		uint8_t eui[16];
		unsigned seed = (unsigned)time(NULL);
		int e = ezsp_cmd(spi, &p, EZSP_GET_EUI64, NULL, 0, eui,
				 sizeof(eui));
		int k;

		for (k = 0; k < e && k < 8; k++)
			seed = seed * 31u + eui[k];
		srand(seed);
	}

	// Stagger the first report too, or a fleet still transmits as one.
	next_report = time(NULL) + (cfg.report_secs
				    ? (time_t)(rand() % 30) : 0);

	n = ezsp_cmd(spi, &p, EZSP_NETWORK_INIT, NULL, 0, out, sizeof(out));
	if (n < 1 || out[0] != EMBER_SUCCESS) {
		uint8_t st[8];
		int s = ezsp_cmd(spi, &p, EZSP_NETWORK_STATE, NULL, 0, st,
				 sizeof(st));

		// With --no-reset the stack is still up from the command that
		// ran before this one, and networkInit refuses because there is
		// nothing to restore. Ask what state the NCP is actually in
		// before calling that "not joined".
		if (s >= 1 && st[0] == 0x02) {
			printf("  network already up, routing as a router\n");
		} else {
			// Not joined is a normal state for a fresh install, not
			// an error: exiting here would leave procd respawning in
			// a loop and nothing able to answer status. Stay up
			// unjoined so the control socket works and "join" has
			// something to talk to.
			printf("  not on a network yet (status 0x%02X) -- run "
			       "\"/etc/init.d/ezsp-spi join\" with permit-join "
			       "open\n", n >= 1 ? out[0] : 0xFF);
		}
	} else {
		printf("  network restored from NVM, routing as a router\n");

	}

	// The daemon owns the radio exclusively, so anything else wanting to
	// look at it has to ask through here.
	listen_fd = control_listen(cfg.sock_path);
	if (listen_fd >= 0)
		printf("  control socket %s\n", cfg.sock_path);
	fflush(stdout);

	// The NCP routes on its own; this loop exists only to drain callbacks
	// so the queue cannot fill, and to answer status requests. nHOST_INT
	// says when there is something to collect, so block rather than poll:
	// idle cost is one poll() instead of SPI traffic forever.
	for (;;) {
		struct pollfd pfd[2];
		uint8_t id;
		int nfds = 0, iv_hint = -1, iv_ctl = -1, timeout;

		if (listen_fd >= 0) {
			iv_ctl = nfds;
			pfd[nfds].fd = listen_fd;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}
		if (p.hint >= 0 && gpio_get(p.hint) != 0) {
			iv_hint = nfds;
			pfd[nfds].fd = p.hint;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}

		// Sleep until the next report is due rather than a flat 30 s,
		// otherwise a report can land up to a whole poll period late.
		timeout = 30000;
		if (cfg.report_secs) {
			long due = (long)(next_report - time(NULL));

			if (due < 0)
				due = 0;
			if (due * 1000 < timeout)
				timeout = (int)(due * 1000);
		}

		// Identifying needs a blink, so wake often enough to toggle.
		if (identify_active() && timeout > 400)
			timeout = 400;

		if (nfds)
			poll(pfd, nfds, timeout);
		else
			usleep(250000);

		if (iv_hint >= 0 && (pfd[iv_hint].revents & POLLIN)) {
			struct gpio_v2_line_event ev;

			// Consume the edge. A short read changes nothing --
			// the NCP gets drained below either way.
			(void)!read(p.hint, &ev, sizeof(ev));
		}

		if (iv_ctl >= 0 && (pfd[iv_ctl].revents & POLLIN))
			control_accept(listen_fd, spi, &p);

		identify_tick();

		// Drain everything queued before sleeping again.
		do {
			id = (uint8_t)ezsp_poll_callback(spi, &p, out,
							 sizeof(out));

			if (id == EZSP_STACK_STATUS_HANDLER) {
				printf("  stack status 0x%02X (%s)\n", out[0],
				       ember_status_name(out[0]));
				fflush(stdout);
			} else if (id == EZSP_CHILD_JOIN_HANDLER) {
				// index(1) joining(1) childId(2) eui64(8) type(1)
				int j;

				printf("  child %s: 0x%04X ",
				       out[1] ? "JOINED" : "left",
				       (unsigned)(out[2] | (out[3] << 8)));
				for (j = 11; j >= 4; j--)
					printf("%02X%s", out[j],
					       j > 4 ? ":" : "");
				printf(" (%s)\n", node_type_name(out[12]));
				fflush(stdout);
			} else if (id == EZSP_TRUST_CENTER_JOIN_HANDLER) {
				// nodeId(2) eui64(8) status(1) decision(1)
				// parent(2)
				int j;

				printf("  trust centre join: 0x%04X ",
				       (unsigned)(out[0] | (out[1] << 8)));
				for (j = 9; j >= 2; j--)
					printf("%02X%s", out[j],
					       j > 2 ? ":" : "");
				printf(" update 0x%02X decision 0x%02X "
				       "parent 0x%04X\n", out[10], out[11],
				       (unsigned)(out[12] | (out[13] << 8)));
				fflush(stdout);
			} else if (id == EZSP_INCOMING_MESSAGE_HANDLER) {
				zcl_handle_incoming(spi, &p, out, sizeof(out));
			} else if (id == EZSP_MESSAGE_SENT_HANDLER) {
				// type(1) dest(2) apsFrame(11) tag(1) status(1)
				if (out[15] != EMBER_SUCCESS) {
					report_failed++;
					printf("  send to 0x%04X FAILED "
					       "(status 0x%02X %s)\n",
					       (unsigned)(out[1] | (out[2] << 8)),
					       out[15],
					       ember_status_name(out[15]));
					fflush(stdout);
				} else if (cfg.verbose) {
					printf("  send to 0x%04X ok\n",
					       (unsigned)(out[1] | (out[2] << 8)));
					fflush(stdout);
				}
			} else if (id && cfg.verbose) {
				printf("  callback 0x%02X\n", id);
				fflush(stdout);
			}
		} while (id);

		// Periodic report, so the coordinator has a recent frame to
		// take a link quality reading from.
		if (cfg.report_secs && time(NULL) >= next_report) {
			zcl_report(spi, &p);
			next_report = time(NULL) + report_interval();
		}
	}

out:
	close(spi);
	pins_close(&p);
	return 1;
}

// --------------------------------------------------------------------------

int reverse_ext_pan_id;

// Accepts both shapes people actually have to hand:
//   colon separated hex   AA:BB:CC:DD:EE:FF:00:11
//   comma separated decimal
//                         139,170,157,111,100,162,112,45
void parse_ext_pan_id(const char *s)
{
	int base = strchr(s, ':') ? 16 : 10;
	const char *q = s;
	unsigned i = 0;

	while (*q && i < 8) {
		char *end;
		unsigned long v;

		while (*q == ' ' || *q == '[' || *q == ',' || *q == ':')
			q++;
		if (!*q)
			break;

		v = strtoul(q, &end, base);
		if (end == q)
			break;
		if (v > 0xFF)
			die("--ext-pan-id byte %lu out of range", v);

		cfg.ext_pan_id[i++] = (uint8_t)v;
		q = end;
	}

	if (i != 8)
		die("--ext-pan-id needs 8 bytes, parsed %u", i);
}
