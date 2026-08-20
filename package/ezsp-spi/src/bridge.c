#include "ezsp-spi.h"

// ------------------------------------------------------------------ bridge --
//
// ASH over TCP, so zigbee-herdsman (zigbee2mqtt) and bellows (ZHA) can drive
// the radio directly, coordinator included:
//
//   zigbee2mqtt:  serial: { adapter: ezsp, port: tcp://<router>:8888 }
//   bellows:      socket://<router>:8888
//
// Relays EZSP frames verbatim in both directions and translates callback
// delivery, which is polled on SPI but pushed on a UART. See DESIGN.md.




// EmberZNet's reset codes. 0x0B is a software/host-requested reset, which is
// what an ASH RST actually is from the NCP's point of view.

uint8_t ash_rand[256];

// Pseudo-random sequence DATA payloads are XORed with (UG101).
void ash_rand_init(void)
{
	uint8_t r = 0x42;
	int i;

	for (i = 0; i < 256; i++) {
		ash_rand[i] = r;
		r = (r & 1) ? (uint8_t)((r >> 1) ^ 0xB8) : (uint8_t)(r >> 1);
	}
}

// CRC-16/CCITT-FALSE, sent high byte first.
uint16_t ash_crc(const uint8_t *d, size_t n)
{
	uint16_t crc = 0xFFFF;
	size_t i;
	int b;

	for (i = 0; i < n; i++) {
		crc ^= (uint16_t)d[i] << 8;
		for (b = 0; b < 8; b++)
			crc = (crc & 0x8000)
				? (uint16_t)((crc << 1) ^ 0x1021)
				: (uint16_t)(crc << 1);
	}

	return crc;
}


int write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		return -1;
	}

	return 0;
}

int ash_send(struct bridge *b, const uint8_t *frame, size_t len)
{
	uint8_t buf[ASH_MAX_FRAME * 2 + 8];
	uint8_t tail[2];
	uint16_t crc;
	size_t pos = 0, i;

	if (b->cli < 0 || len + 2 > ASH_MAX_FRAME + 2)
		return -1;

	crc = ash_crc(frame, len);
	tail[0] = (uint8_t)(crc >> 8);
	tail[1] = (uint8_t)(crc & 0xFF);

	for (i = 0; i < len + 2; i++) {
		uint8_t c = i < len ? frame[i] : tail[i - len];

		switch (c) {
		case ASH_FLAG:
		case ASH_ESC:
		case ASH_XON:
		case ASH_XOFF:
		case ASH_SUB:
		case ASH_CANCEL:
			buf[pos++] = ASH_ESC;
			buf[pos++] = c ^ 0x20;
			break;
		default:
			buf[pos++] = c;
		}
	}
	buf[pos++] = ASH_FLAG;

	return write_all(b->cli, buf, pos);
}

void ash_send_ack(struct bridge *b)
{
	uint8_t c = (uint8_t)(0x80 | (b->ack & 0x07));

	ash_send(b, &c, 1);
}

// Hand one EZSP frame to the client as an ASH DATA frame.
void ash_send_data(struct bridge *b, const uint8_t *ezsp, size_t len)
{
	uint8_t body[ASH_MAX_FRAME];
	size_t i;

	if (len + 1 > sizeof(body))
		return;

	body[0] = (uint8_t)(((b->frm & 0x07) << 4) | (b->ack & 0x07));
	for (i = 0; i < len; i++)
		body[1 + i] = ezsp[i] ^ ash_rand[i];

	memcpy(b->sent[b->frm & 0x07].body, body, len + 1);
	b->sent[b->frm & 0x07].len = len + 1;
	b->frm = (uint8_t)((b->frm + 1) & 0x07);

	ash_send(b, body, len + 1);
}

// Resend everything from the requested frame number up to what we have sent,
// with the retransmit bit set as the protocol requires.
void ash_retransmit(struct bridge *b, uint8_t from)
{
	uint8_t n;

	for (n = from; n != b->frm; n = (uint8_t)((n + 1) & 0x07)) {
		uint8_t body[ASH_MAX_FRAME];
		size_t len = b->sent[n & 0x07].len;

		if (!len)
			continue;

		memcpy(body, b->sent[n & 0x07].body, len);
		body[0] |= 0x08;
		ash_send(b, body, len);
	}
}

// Send an already-encoded EZSP frame to the NCP; return its reply verbatim.
int ezsp_relay(int spi, struct pins *p, const uint8_t *in, size_t inlen,
		      uint8_t *out, size_t outmax)
{
	uint8_t tx[ASH_MAX_FRAME + 8], rx[260];
	enum rsp_class cls;
	size_t txlen = 0;
	int len;

	if (!inlen || inlen > 255 || inlen + 3 > sizeof(tx))
		return -1;

	tx[txlen++] = SPI_CMD_EZSP_FRAME;
	tx[txlen++] = (uint8_t)inlen;
	memcpy(tx + txlen, in, inlen);
	txlen += inlen;
	tx[txlen++] = SPI_FRAME_TERMINATOR;

	cls = do_cmd(spi, p, tx, txlen, rx, sizeof(rx), 1);
	if (cls != RSP_EZSP)
		return -1;

	len = rx[1];
	if ((size_t)len > outmax)
		len = (int)outmax;
	memcpy(out, rx + 2, (size_t)len);

	return len;
}

// Header length and frame-id offset for an EZSP version.
void ezsp_layout(int ver, int *hdr, int *id_off)
{
	if (ver >= 8) {
		*hdr = 5;
		*id_off = 3;
	} else if (ver >= 5) {
		*hdr = 5;
		*id_off = 4;
	} else {
		*hdr = 3;
		*id_off = 2;
	}
}

// One queued callback, ready to forward as-is; 0 when the queue is empty.
int ezsp_relay_callback(struct bridge *b, uint8_t *out, size_t outmax)
{
	uint8_t f[8];
	size_t n = 0;
	int len, hdr, id_off;

	ezsp_layout(b->ver, &hdr, &id_off);

	f[n++] = b->cbseq++;
	if (b->ver >= 8) {
		f[n++] = 0x00;		// frame control, low
		f[n++] = 0x01;		// frame control, high
		f[n++] = EZSP_CALLBACK;
		f[n++] = 0x00;
	} else if (b->ver >= 5) {
		f[n++] = 0x00;
		f[n++] = 0xFF;
		f[n++] = 0x00;
		f[n++] = EZSP_CALLBACK;
	} else {
		f[n++] = 0x00;
		f[n++] = EZSP_CALLBACK;
	}

	len = ezsp_relay(b->spi, b->p, f, n, out, outmax);
	if (len <= id_off)
		return 0;

	// noCallbacks just means the queue is empty; not something to forward.
	if (out[id_off] == EZSP_NO_CALLBACKS)
		return 0;

	return len;
}

void bridge_drain_callbacks(struct bridge *b)
{
	uint8_t frame[ASH_MAX_FRAME];
	int guard;

	if (b->cli < 0 || !b->ready || !b->negotiated || b->p->hint < 0)
		return;

	// Bounded against a stuck nHOST_INT.
	for (guard = 0; guard < 32; guard++) {
		int len;

		if (gpio_get(b->p->hint) != 0)
			return;

		len = ezsp_relay_callback(b, frame, sizeof(frame));
		if (len <= 0)
			return;

		ash_send_data(b, frame, (size_t)len);
	}
}

void bridge_reset(struct bridge *b)
{
	uint8_t rstack[3];

	if (!cfg.quiet)
		printf("  RST from host -- resetting the NCP\n");

	ncp_reset(b->p);

	b->frm = 0;
	b->ack = 0;
	b->ver = 4;
	memset(b->sent, 0, sizeof(b->sent));

	rstack[0] = ASH_CTRL_RSTACK;
	rstack[1] = 0x02;			// ASH protocol version
	rstack[2] = ASH_RESET_SOFTWARE;
	ash_send(b, rstack, sizeof(rstack));
	b->ready = 1;
	b->negotiated = 0;
}

// One complete, unescaped, CRC-checked ASH frame from the client.
void bridge_handle_frame(struct bridge *b, uint8_t *f, size_t len)
{
	uint8_t ctrl = f[0];
	uint8_t ezsp[ASH_MAX_FRAME], resp[ASH_MAX_FRAME];
	size_t plen, i;
	int rlen, hdr, id_off;

	if (ctrl == ASH_CTRL_RST) {
		bridge_reset(b);
		return;
	}

	if ((ctrl & 0xE0) == 0x80) {		// ACK
		return;
	}

	if ((ctrl & 0xE0) == 0xA0) {		// NAK
		ash_retransmit(b, (uint8_t)(ctrl & 0x07));
		return;
	}

	if (ctrl & 0x80)			// RSTACK/ERROR from a host
		return;

	// DATA before RST: the client skipped the handshake.
	if (!b->ready) {
		if (!cfg.quiet)
			printf("  DATA before RST -- ignored\n");
		return;
	}

	// DATA. Everything past the control byte and before the CRC is the
	// EZSP frame, masked.
	plen = len - 1;
	if (!plen || plen > sizeof(ezsp))
		return;

	for (i = 0; i < plen; i++)
		ezsp[i] = f[1 + i] ^ ash_rand[i];

	// Acknowledge before touching SPI, or the client's retransmit timer
	// can fire mid-transaction and we run the command twice.
	b->ack = (uint8_t)((((ctrl >> 4) & 0x07) + 1) & 0x07);
	ash_send_ack(b);

	ezsp_layout(b->ver, &hdr, &id_off);

	rlen = ezsp_relay(b->spi, b->p, ezsp, plen, resp, sizeof(resp));
	if (rlen <= 0) {
		// Say nothing; the client's timeout and NAK handling cover it.
		if (!cfg.quiet)
			printf("  no reply from the NCP for a %zu byte frame\n",
			       plen);
		return;
	}

	// Adopt whatever version the two of them just agreed on.
	if ((size_t)plen > (size_t)id_off && ezsp[id_off] == EZSP_VERSION &&
	    rlen > hdr) {
		int neg = resp[hdr];

		if (neg >= 4 && neg <= 13) {
			if (neg != b->ver && !cfg.quiet)
				printf("  client negotiated EZSP v%d\n", neg);
			b->ver = neg;
			b->negotiated = 1;
		}
	}

	ash_send_data(b, resp, (size_t)rlen);

	// A command often leaves callbacks queued behind it.
	bridge_drain_callbacks(b);
}

void bridge_feed(struct bridge *b, const uint8_t *buf, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		uint8_t c = buf[i];

		switch (c) {
		case ASH_XON:
		case ASH_XOFF:
			continue;		// flow control, not our problem
		case ASH_CANCEL:
			b->rxlen = 0;
			b->esc = 0;
			continue;
		case ASH_SUB:
			// Everything up to the next flag is void.
			b->rxlen = 0;
			b->esc = 1;
			continue;
		case ASH_ESC:
			b->esc = 1;
			continue;
		case ASH_FLAG:
			if (b->rxlen >= 3) {
				uint16_t want = (uint16_t)
					((b->rx[b->rxlen - 2] << 8) |
					 b->rx[b->rxlen - 1]);

				if (ash_crc(b->rx, b->rxlen - 2) == want)
					bridge_handle_frame(b, b->rx,
							    b->rxlen - 2);
				else if (!cfg.quiet)
					printf("  ASH crc mismatch, %zu bytes "
					       "dropped\n", b->rxlen);
			}
			b->rxlen = 0;
			b->esc = 0;
			continue;
		default:
			break;
		}

		if (b->esc) {
			c ^= 0x20;
			b->esc = 0;
		}

		if (b->rxlen < sizeof(b->rx))
			b->rx[b->rxlen++] = c;
		else
			b->rxlen = 0;		// oversized, resync on the flag
	}
}

void bridge_drop_client(struct bridge *b, const char *why)
{
	if (b->cli < 0)
		return;

	close(b->cli);
	b->cli = -1;
	b->rxlen = 0;
	b->esc = 0;

	printf("  client gone (%s)\n", why);
	fflush(stdout);
}

int bridge_listen(const char *addr, int port)
{
	struct sockaddr_in sa;
	int fd, on = 1;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		die("socket: %s", strerror(errno));

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	if (!addr || !*addr || !strcmp(addr, "*"))
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
	else if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1)
		die("bad bind address \"%s\"", addr);

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		die("bind %s:%d: %s", addr && *addr ? addr : "0.0.0.0", port,
		    strerror(errno));

	if (listen(fd, 1) < 0)
		die("listen: %s", strerror(errno));

	return fd;
}

int cmd_bridge(void)
{
	struct bridge b;
	struct pins p;

	printf("ezsp-spi bridge\n");

	ash_rand_init();

	pins_open(&p);

	memset(&b, 0, sizeof(b));
	b.p = &p;
	b.cli = -1;
	b.ver = 4;
	b.spi = spi_open(cfg.spidev, cfg.speed);

	if (p.hint < 0)
		printf("  warning: no nHOST_INT line -- callbacks will be "
		       "polled on a timer, which is slower and noisier\n");

	b.srv = bridge_listen(cfg.bind_addr, cfg.bridge_port);
	printf("  listening on %s:%d\n",
	       cfg.bind_addr && *cfg.bind_addr ? cfg.bind_addr : "0.0.0.0",
	       cfg.bridge_port);
	printf("  zigbee2mqtt: adapter ezsp, port tcp://<this host>:%d\n",
	       cfg.bridge_port);
	fflush(stdout);

	// The client opens with an ASH RST and does its own handshake.
	for (;;) {
		struct pollfd pfd[3];
		int nfds = 0, iv_srv, iv_cli = -1, iv_hint = -1;
		int timeout;

		iv_srv = nfds;
		pfd[nfds].fd = b.srv;
		pfd[nfds].events = POLLIN;
		pfd[nfds].revents = 0;
		nfds++;

		if (b.cli >= 0) {
			iv_cli = nfds;
			pfd[nfds].fd = b.cli;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}

		// Only useful while released; an asserted line has no edge left.
		if (b.cli >= 0 && p.hint >= 0 && gpio_get(p.hint) != 0) {
			iv_hint = nfds;
			pfd[nfds].fd = p.hint;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}

		// Without an interrupt line, poll often enough to stay prompt.
		timeout = (b.cli >= 0 && p.hint < 0) ? 20 : 1000;

		if (poll(pfd, (nfds_t)nfds, timeout) < 0) {
			if (errno == EINTR)
				continue;
			die("poll: %s", strerror(errno));
		}

		if (pfd[iv_srv].revents & POLLIN) {
			struct sockaddr_in ca;
			socklen_t clen = sizeof(ca);
			int fd = accept(b.srv, (struct sockaddr *)&ca, &clen);

			if (fd >= 0) {
				char ip[INET_ADDRSTRLEN] = "?";
				int on = 1;

				// One client owns the radio.
				if (b.cli >= 0)
					bridge_drop_client(&b, "replaced");

				setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on,
					   sizeof(on));

				inet_ntop(AF_INET, &ca.sin_addr, ip,
					  sizeof(ip));
				b.cli = fd;
				b.ready = 0;
				b.negotiated = 0;
				b.rxlen = 0;
				b.esc = 0;
				b.frm = 0;
				b.ack = 0;
				b.ver = 4;
				memset(b.sent, 0, sizeof(b.sent));

				printf("  client %s connected\n", ip);
				fflush(stdout);
			}
		}

		if (iv_hint >= 0 && (pfd[iv_hint].revents & POLLIN)) {
			struct gpio_v2_line_event ev;

			// Consume the edge; the drain below collects.
			(void)!read(p.hint, &ev, sizeof(ev));
		}

		if (iv_cli >= 0 && (pfd[iv_cli].revents & (POLLERR | POLLHUP))) {
			bridge_drop_client(&b, "hangup");
			continue;
		}

		if (iv_cli >= 0 && (pfd[iv_cli].revents & POLLIN)) {
			uint8_t buf[1024];
			ssize_t n = read(b.cli, buf, sizeof(buf));

			if (n > 0)
				bridge_feed(&b, buf, (size_t)n);
			else if (n == 0)
				bridge_drop_client(&b, "closed");
			else if (errno != EINTR && errno != EAGAIN)
				bridge_drop_client(&b, strerror(errno));
		}

		bridge_drain_callbacks(&b);
	}

	return 0;
}
