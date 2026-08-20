#include "ezsp-spi.h"

// --------------------------------------------------------------------- SPI --

int spi_open(const char *path, uint32_t speed)
{
	uint8_t mode = SPI_MODE_0, bits = 8;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("open %s: %s -- is spidev bound? check "
		    "/sys/bus/spi/devices/spi1.0/driver", path, strerror(errno));

	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0)
		die("set spi mode 0x%02X: %s", mode, strerror(errno));
	if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
		die("set spi bits: %s", strerror(errno));
	if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
		die("set spi speed: %s", strerror(errno));

	return fd;
}

// One transfer. keep_cs leaves chip select asserted after the message ends:
// the SPI core's spi_transfer_one_message() sets keep_cs when the last transfer
// in a message has cs_change set, and skips the closing spi_set_cs(). That is
// what lets a single transaction span several ioctls without the kernel
// dropping nSSEL in between, and it needs no device tree change.
void spi_raw(int fd, const uint8_t *tx, uint8_t *rx, size_t len,
		    int keep_cs)
{
	struct spi_ioc_transfer xf;

	memset(&xf, 0, sizeof(xf));
	xf.tx_buf = (unsigned long)tx;
	xf.rx_buf = (unsigned long)rx;
	xf.len = len;
	xf.speed_hz = cfg.speed;
	xf.bits_per_word = 8;
	xf.cs_change = keep_cs ? 1 : 0;

	if (ioctl(fd, SPI_IOC_MESSAGE(1), &xf) < 0)
		die("spi transfer: %s", strerror(errno));
}

// A whole EZSP-SPI transaction, ending exactly on the frame terminator.
//
// Clocking even one byte past 0xA7 with nSSEL still asserted reads, to the NCP,
// as the host opening a command it never terminates; it reports that as 0x02 on
// the *following* transaction. Nor can the length be precomputed -- the reply
// drifts by a byte or two, so a fixed window lands on the terminator for one
// command and overruns the next.
//
// So: hold CS, clock idle bytes until the NCP starts answering, then use the
// response type to work out exactly how many bytes are left, and fetch precisely
// that many in a final transfer that releases CS on the terminator.
size_t spi_txn_framed(int spi, struct pins *p, const uint8_t *tx,
			     size_t txlen, uint8_t *rx, size_t rxlen)
{
	const uint8_t idle = SPI_IDLE;
	uint8_t pad[260];
	size_t remain, n = 0;
	uint8_t b = SPI_IDLE;
	unsigned i;

	memset(rx, 0, rxlen);
	memset(pad, SPI_IDLE, sizeof(pad));

	// Assert nSSEL for the whole transaction and release it on the
	// terminator -- the entire reason the device tree hands us this pin.
	cs_assert(p);

	if (txlen)
		spi_raw(spi, tx, NULL, txlen, 1);

	// Wait states. These are legitimate: they come before the response.
	for (i = 0; i < cfg.wait_bytes; i++) {
		spi_raw(spi, &idle, &b, 1, 1);
		if (b != SPI_IDLE)
			break;
	}

	if (b == SPI_IDLE) {
		if (p->cs >= 0)
			cs_release(p);
		else
			spi_raw(spi, &idle, &b, 1, 0);
		if (cfg.verbose)
			printf("  (no response within %u wait bytes)\n",
			       cfg.wait_bytes);
		return 0;
	}

	rx[n++] = b;

	if (b == SPI_CMD_EZSP_FRAME) {
		// 0xFE <len> <payload> 0xA7
		if (n < rxlen) {
			spi_raw(spi, &idle, &b, 1, 1);
			rx[n++] = b;
			remain = (size_t)b + 1;
		} else {
			remain = 1;
		}
	} else if (b <= SPI_ERR_UNSUPPORTED) {
		remain = 2;			// error data byte + terminator
	} else {
		remain = 1;			// terminator only
	}

	if (remain > rxlen - n)
		remain = rxlen - n;
	if (remain > sizeof(pad))
		remain = sizeof(pad);

	if (remain) {
		spi_raw(spi, pad, rx + n, remain, p->cs >= 0 ? 1 : 0);
		n += remain;
	} else if (p->cs < 0) {
		spi_raw(spi, &idle, &b, 1, 0);
	}

	cs_release(p);				// release on the terminator

	if (cfg.verbose) {
		if (txlen)
			hexdump("tx", tx, txlen);
		hexdump("rx", rx, n);
		if (n && rx[n - 1] != SPI_FRAME_TERMINATOR)
			printf("  (warning: transaction did not end on 0xA7)\n");
	}

	return n;
}

// Skip leading idle bytes; return index of the first real response byte.
//
// Only 0xFF is idle. 0x00 is a legitimate response byte -- it is the "reset
// occurred" error, which the NCP reports for the first command after a reset.
// Treating 0x00 as padding here misreads "00 02 A7" (reset, reason 2) as the
// error 0x02 that happens to follow it.
int spi_first_response(const uint8_t *rx, size_t rxlen)
{
	size_t i;

	for (i = 0; i < rxlen; i++)
		if (rx[i] != SPI_IDLE)
			return (int)i;

	return -1;
}

const char *reset_reason(uint8_t code)
{
	switch (code) {
	case 0x00: return "unknown";
	case 0x01: return "external";
	case 0x02: return "power on";
	case 0x03: return "watchdog";
	case 0x04: return "assert";
	case 0x05: return "bootloader";
	case 0x06: return "software";
	// EM35x halGetResetInfo extended codes, as printed by TestNCP_SPI
	case 0x0B: return "SOFTWARE (extended code)";
	default:   return "unrecognised";
	}
}

const char *spi_error_name(uint8_t code)
{
	switch (code) {
	case SPI_ERR_RESET:		return "reset occurred";
	case SPI_ERR_OVERSIZED:		return "oversized EZSP frame";
	case SPI_ERR_ABORTED:		return "aborted transaction";
	case SPI_ERR_MISSING_TERM:	return "missing frame terminator";
	case SPI_ERR_UNSUPPORTED:	return "unsupported SPI command";
	default:			return "unknown error";
	}
}

// Decode one NCP response. Returns the response byte, or -1 if nothing but idle.
int decode_response(const uint8_t *rx, size_t rxlen, const char *what)
{
	int i = spi_first_response(rx, rxlen);
	uint8_t b;

	if (i < 0) {
		printf("  %-22s no response (bus idle, all 0x%02X)\n",
		       what, SPI_IDLE);
		return -1;
	}

	b = rx[i];

	if (b == 0xFE) {
		uint8_t len = (i + 1 < (int)rxlen) ? rx[i + 1] : 0;
		printf("  %-22s EZSP frame, %u bytes\n", what, len);
		return b;
	}
	if ((b & 0xC0) == 0x80) {
		printf("  %-22s SPI protocol version %u (0x%02X)%s\n", what,
		       b & 0x3F, b, (b == 0x82) ? " -- expected" : "");
		return b;
	}
	if ((b & 0xFE) == 0xC0) {
		printf("  %-22s SPI status 0x%02X -- NCP %s\n", what, b,
		       (b & 1) ? "alive and ready" : "NOT ready");
		return b;
	}
	if (b <= 0x04) {
		uint8_t data = (i + 1 < (int)rxlen) ? rx[i + 1] : 0;
		printf("  %-22s error 0x%02X: %s", what, b, spi_error_name(b));
		if (b == SPI_ERR_RESET)
			printf(" -- reset info %u (%s)", data,
			       reset_reason(data));
		printf("\n");
		return b;
	}

	printf("  %-22s unrecognised response 0x%02X\n", what, b);
	return b;
}

// nSSEL and nSSEL_INT are separate host GPIOs here and must move together.
void cs_assert(struct pins *p)
{
	if (p->cs >= 0)
		gpio_set(p->cs, 0);
	if (p->nssel_int >= 0 && cs_follows)
		gpio_set(p->nssel_int, 0);
}

void cs_release(struct pins *p)
{
	if (p->cs >= 0)
		gpio_set(p->cs, 1);
	if (p->nssel_int >= 0 && cs_follows)
		gpio_set(p->nssel_int, 1);
}
