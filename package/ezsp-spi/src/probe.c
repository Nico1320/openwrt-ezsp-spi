#include "ezsp-spi.h"

// ------------------------------------------------------------------ probes --


enum rsp_class classify(const uint8_t *rx, size_t rxlen, uint8_t *first,
			       uint8_t *data)
{
	int i = spi_first_response(rx, rxlen);

	*first = 0;
	*data = 0;

	if (i < 0)
		return RSP_IDLE;

	*first = rx[i];
	if (i + 1 < (int)rxlen)
		*data = rx[i + 1];

	if (rx[i] == 0xFE)
		return RSP_EZSP;
	if ((rx[i] & 0xC0) == 0x80)
		return RSP_VERSION;
	if ((rx[i] & 0xFE) == 0xC0)
		return RSP_STATUS;
	if (rx[i] == 0x00)
		return RSP_RESET;
	if (rx[i] <= 0x04)
		return RSP_ERROR;

	return RSP_OTHER;
}

// Send one command and collect the reply.
//
// The NCP answers the first command after a reset with the reset notification
// instead of the requested response, so on seeing that we pause and re-send
// once. Transactions are also spaced by gap_us: firing them back to back with
// only ioctl overhead between appears to be what provokes 0x02 (aborted
// transaction) on every command after the first.
enum rsp_class do_cmd(int spi, struct pins *p, const uint8_t *tx,
			     size_t txlen, uint8_t *rx, size_t rxlen,
			     int retry_on_reset)
{
	enum rsp_class cls;
	uint8_t first, data;

	size_t n;

	usleep(cfg.gap_us);
	if (cfg.wake_handshake)
		ncp_wake(p);

	n = spi_txn_framed(spi, p, tx, txlen, rx, rxlen);
	cls = classify(rx, n, &first, &data);

	if (cls == RSP_RESET && retry_on_reset) {
		if (cfg.verbose)
			printf("  (reset notification: %s -- re-sending)\n",
			       reset_reason(data));
		usleep(cfg.gap_us);
		if (cfg.wake_handshake)
			ncp_wake(p);
		n = spi_txn_framed(spi, p, tx, txlen, rx, rxlen);
		cls = classify(rx, n, &first, &data);
	}

	return cls;
}

int cmd_probe(void)
{
	uint8_t rx[512], tx[16];
	struct pins p;
	int spi;

	printf("ezsp-spi probe\n");
	printf("  spidev %s @ %u Hz, mode 0\n", cfg.spidev, cfg.speed);
	printf("  gpios: nRESET=%u nWAKE=%u nHOST_INT=%u nCS=%u (chip %s)\n",
	       cfg.reset_line, cfg.wake_line, cfg.int_line, cfg.nssel_int_line,
	       cfg.gpiochip);

	pins_open(&p);
	spi = spi_open(cfg.spidev, cfg.speed);

	if (!cfg.no_reset)
		ncp_reset(&p);

	// 1. SPI protocol version, expect 0x82.
	printf("\nSPI protocol version query:\n");
	tx[0] = SPI_CMD_VERSION;
	tx[1] = SPI_FRAME_TERMINATOR;
	do_cmd(spi, &p, tx, 2, rx, sizeof(rx), 1);
	decode_response(rx, sizeof(rx), "version");

	// 2. SPI status, expect 0xC1 once the NCP application is running.
	printf("\nSPI status query:\n");
	tx[0] = SPI_CMD_STATUS;
	tx[1] = SPI_FRAME_TERMINATOR;
	do_cmd(spi, &p, tx, 2, rx, sizeof(rx), 1);
	decode_response(rx, sizeof(rx), "status");

	// 4. EZSP version command. Legacy (pre-v8) frame layout:
	//      [sequence] [frameControl] [frameID=0x00] [desiredVersion]
	//    The NCP replies with its own protocol version even when it does
	//    not match ours, which is how we learn what it actually speaks.
	printf("\nEZSP version command:\n");
	tx[0] = SPI_CMD_EZSP_FRAME;
	tx[1] = 0x04;			// EZSP payload length
	tx[2] = 0x00;			// sequence
	tx[3] = 0x00;			// frame control: command, no sleep
	tx[4] = 0x00;			// frame ID: version
	tx[5] = 0x04;			// desired EZSP version
	tx[6] = SPI_FRAME_TERMINATOR;
	do_cmd(spi, &p, tx, 7, rx, sizeof(rx), 1);

	{
		int i = spi_first_response(rx, sizeof(rx));

		if (i >= 0 && rx[i] == 0xFE && i + 6 < (int)sizeof(rx)) {
			const uint8_t *e = &rx[i + 2];	// skip 0xFE, length

			printf("  EZSP protocol version %u\n", e[3]);
			printf("  stack type            %u\n", e[4]);
			printf("  stack version         0x%04X\n",
			       (unsigned)(e[5] | (e[6] << 8)));
		} else {
			decode_response(rx, sizeof(rx), "ezsp version");
		}
	}

	printf("\n");
	return 0;
}
