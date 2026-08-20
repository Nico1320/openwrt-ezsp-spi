#include "ezsp-spi.h"

// ------------------------------------------------------------------- reset --


// Pin map differs by revision; gpio29 is the eMMC's on the V1.
//
//            nRESET  nWAKE  nHOST_INT  nSSEL_INT  nSSEL
//   V1         49      55       50        56       45
//   V2         49      31       50        29       45
// Boards carrying an EM35x NCP on SPI. Adding one is a data change.
//
// forbid/forbid_why guard a line that is something else entirely on that
// board -- driving it would do real damage, so the tool refuses instead.

const struct board boards[] = {
	{ "whw03v2", "Linksys Velop WHW03 V2",
	  49, 31, 50, 29, 45, 0, NULL },
	{ "whw03",   "Linksys Velop WHW03 V1",
	  49, 55, 50, 56, 45, 29, "an eMMC line on this board" },
	{ NULL, NULL, 0, 0, 0, 0, 0, 0, NULL },
};

const struct board *board_detect(void)
{
	static char name[64];
	const struct board *b;
	FILE *f = fopen("/tmp/sysinfo/board_name", "r");

	if (!f)
		return NULL;
	if (!fgets(name, sizeof(name), f))
		name[0] = 0;
	fclose(f);

	for (b = boards; b->match; b++)
		if (strstr(name, b->match))
			return b;

	return NULL;
}

void pins_autodetect(void)
{
	const struct board *b = board_detect();

	if (!b)
		return;

	if (!cfg.reset_set)	cfg.reset_line = b->reset;
	if (!cfg.wake_set)	cfg.wake_line = b->wake;
	if (!cfg.int_set)	cfg.int_line = b->hostint;
	if (!cfg.nssel_int_set)	cfg.nssel_int_line = b->nssel_int;
	if (!cfg.cs_set)	cfg.cs_line = b->nssel;

	if (b->forbid && cfg.nssel_int_line == b->forbid &&
	    strcmp(cfg.nssel_int_mode, "input") != 0)
		die("refusing to drive gpio%u on %s: it is %s, not nSSEL_INT "
		    "(gpio%u here).", b->forbid, b->desc, b->forbid_why,
		    b->nssel_int);

	if (!cfg.quiet)
		printf("  board %s\n", b->desc);
}

void pins_open(struct pins *p)
{
	pins_autodetect();

	// nRESET and nWAKE idle high (deasserted).
	p->reset = gpio_request_output(cfg.gpiochip, cfg.reset_line, 1, "ezsp-nreset");
	p->wake  = gpio_request_output(cfg.gpiochip, cfg.wake_line, 1, "ezsp-nwake");

	if (!strcmp(cfg.nssel_int_mode, "input")) {
		p->nssel_int = -1;
		if (!cfg.quiet)
			printf("  nSSEL_INT gpio%u left alone\n",
			       cfg.nssel_int_line);
	} else {
		// "follow" starts deasserted; low/high pin it for investigation.
		int v = strcmp(cfg.nssel_int_mode, "low") != 0;

		cs_follows = !strcmp(cfg.nssel_int_mode, "follow");

		p->nssel_int = gpio_request_output(cfg.gpiochip,
						   cfg.nssel_int_line, v,
						   "ezsp-nssel-int");
		if (!cfg.quiet) {
			if (cs_follows)
				printf("  nSSEL_INT gpio%u follows nSSEL\n",
				       cfg.nssel_int_line);
			else
				printf("  nSSEL_INT gpio%u pinned %s\n",
				       cfg.nssel_int_line, v ? "high" : "low");
		}
	}

	// With cs-gpios removed from the device tree the kernel no longer
	// drives nSSEL, so the host owns it. That is the point: a transaction
	// has to stay asserted from the command until the 0xA7 terminator and
	// release exactly there, which a per-ioctl kernel chip select cannot do.
	// Prefer driving chip select ourselves -- that is what lets a
	// transaction end exactly on the frame terminator. But on a device tree
	// that still declares cs-gpios the kernel owns this pin, and the
	// cs_change path works there instead, so a refusal is not an error.
	p->cs = -1;
	if (cfg.cs_line != 0) {
		p->cs = gpio_try_output(cfg.gpiochip, cfg.cs_line, 1,
					"ezsp-spics");
		if (!cfg.quiet)
			printf(p->cs >= 0
			       ? "  spiCS gpio%u driven by host (idle high)\n"
			       : "  spiCS gpio%u held by the kernel; using "
				 "cs_change instead\n", cfg.cs_line);
	}

	p->hint = gpio_request_input_edges(cfg.gpiochip, cfg.int_line, "ezsp-hostint");
	if (p->hint < 0 && !cfg.quiet)
		printf("  warning: cannot watch gpio%u (nHOST_INT): %s\n",
		       cfg.int_line, strerror(errno));

}

void pins_close(struct pins *p)
{
	if (p->cs >= 0)		close(p->cs);
	if (p->reset >= 0)	close(p->reset);
	if (p->wake >= 0)	close(p->wake);
	if (p->nssel_int >= 0)	close(p->nssel_int);
	if (p->hint >= 0)	close(p->hint);

	p->reset = p->wake = p->nssel_int = p->hint = p->cs = -1;
}

// Hardware reset into the NCP application: nWAKE stays HIGH across the release
// of nRESET. Holding nWAKE low there is the standalone-bootloader entry
// condition instead (see zbbootloader.sh), which we deliberately avoid.
void ncp_reset(struct pins *p)
{
	if (!cfg.quiet)
		printf("resetting NCP (nWAKE high -> boots application, "
		       "not bootloader)\n");

	gpio_set(p->wake, 1);
	gpio_set(p->reset, 0);
	msleep(50);

	if (p->hint >= 0)
		gpio_drain_events(p->hint);

	gpio_set(p->reset, 1);

	if (p->hint >= 0) {
		int ms = gpio_wait_falling(p->hint, 2000);

		if (!cfg.quiet) {
			if (ms >= 0)
				printf("  gpio%u asserted %d ms after reset "
				       "release\n", cfg.int_line, ms);
			else
				printf("  gpio%u never asserted within "
				       "2000 ms\n", cfg.int_line);
		}
	} else {
		msleep(500);
	}

}

// EZSP-SPI wake handshake (AN711).
//
// The NCP application sleeps between transactions. A sleeping NCP that finds
// nSSEL asserted underneath it reports 0x02 (aborted transaction) rather than
// answering -- which is exactly what we see on every command after the reset
// notification, when the chip is briefly awake and answers once.
//
// The handshake is: assert nWAKE, wait for the NCP to acknowledge by asserting
// nHOST_INT, release nWAKE. Only then is it safe to start a transaction.
void ncp_wake(struct pins *p)
{
	if (p->hint < 0) {
		// No interrupt line to watch: fall back to a timed pulse.
		gpio_set(p->wake, 0);
		usleep(2000);
		gpio_set(p->wake, 1);
		usleep(2000);
		return;
	}

	// Already asserted means the NCP is awake and has something pending.
	if (gpio_get(p->hint) == 0)
		return;

	gpio_drain_events(p->hint);

	gpio_set(p->wake, 0);
	if (gpio_wait_falling(p->hint, 100) < 0 && cfg.verbose)
		printf("  (no nHOST_INT ack to nWAKE within 100 ms)\n");
	gpio_set(p->wake, 1);

	usleep(500);
}
