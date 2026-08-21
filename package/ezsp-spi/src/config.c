#include "ezsp-spi.h"

// ezsp-spi: EZSP-SPI host implementation for the Silicon Labs EM3581 NCP
// on the Linksys WHW03 V2 (Velop), OpenWrt / kernel 6.6.
//
// Replaces the vendor TestNCP_SPI binary with something that does not need a
// uClibc chroot, and that we can extend into an ASH bridge.
//
// Protocol reference: Silabs UG100 (EZSP) and AN711 (EZSP-SPI host interfacing).
//
// An EZSP-SPI transaction is command bytes, then wait states, then the
// response, all inside ONE chip-select assertion, and it must end exactly on
// the 0xA7 frame terminator. Both halves of that matter:
//
//   - Deasserting nSSEL early aborts the transaction, which is why poking at
//     the bus with spi-pipe only ever returns 0xFF.
//   - Clocking past the terminator with nSSEL still asserted looks like the
//     host opening a command it never finishes; the NCP reports that as 0x02
//     (aborted transaction) on the *following* transaction, so one overrun
//     poisons every command after it.
//
// The reply's position drifts by a byte or two, so no precomputed transfer
// length works. Instead spi_txn_framed() holds chip select across several
// ioctls -- the SPI core keeps it asserted when the last transfer in a message
// sets cs_change -- clocks wait states until the NCP answers, then reads
// exactly as many bytes as the response type implies.


// ---------------------------------------------------------------- EZSP-SPI --


// NCP error responses (single byte, some carry one data byte)

// EmberInitialSecurityBitmask (EZSP type tables)

// ---------------------------------------------------------------- defaults --


int cs_follows;

struct cfg cfg = {
	.spidev		= "/dev/spidev1.0",
	.gpiochip	= "/dev/gpiochip0",
	.reset_line	= 49,	// nRESET   (sysfs 561)
	.wake_line	= 31,	// nWAKE    (sysfs 543)
	.int_line	= 50,	// nHOST_INT per the vendor binary
	.nssel_int_line	= 29,	// nSSEL_INT on the V2; gpio56 on the V1
	.cs_line	= 45,	// spiCS -- driven here, not by the kernel
	// Driven with nSSEL, as AN711 requires. See DESIGN.md.
	.nssel_int_mode	= "follow",
	// Commands that touch the radio (joinNetwork, startScan) take far
	// longer to answer than the SPI-level queries; 64 bytes at 1 MHz is
	// only ~0.5 ms and joinNetwork was intermittently missing its reply.
	.wait_bytes	= 512,
	.ezsp_version	= 4,
	.ezsp_want	= 4,
	.tx_power	= 8,
	.sec_bitmask	= SEC_HAVE_PRECONFIGURED_KEY |
			  SEC_TRUST_CENTER_GLOBAL_LINK_KEY,
	.join_timeout	= 60,
	.join_retries	= 2,
	.report_secs	= 300,
	.sock_path	= "/var/run/ezsp-spi.sock",
	.bind_addr	= "0.0.0.0",
	.bridge_port	= 8888,
	.speed		= 1000000,
	.gap_us		= 2000,
	.no_reset	= 0,
	.wake_handshake	= 1,
	.quiet		= 0,
	.verbose	= 0,
};

void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "error: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

void msleep(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

void hexdump(const char *tag, const uint8_t *buf, size_t len)
{
	size_t i;

	printf("  %s [%zu]:", tag, len);
	for (i = 0; i < len; i++) {
		if (i && i % 16 == 0)
			printf("\n%*s", (int)strlen(tag) + 4, "");
		printf(" %02X", buf[i]);
	}
	printf("\n");
}
