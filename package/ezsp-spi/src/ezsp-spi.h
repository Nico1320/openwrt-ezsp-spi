#ifndef EZSP_SPI_H
#define EZSP_SPI_H

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#define SPI_CMD_VERSION		0x0A	// query SPI protocol version
#define SPI_CMD_STATUS		0x0B	// query NCP status
#define SPI_CMD_EZSP_FRAME	0xFE	// EZSP frame follows: <len> <payload>
#define SPI_CMD_BTL_FRAME	0xFD	// bootloader frame
#define SPI_FRAME_TERMINATOR	0xA7
#define SPI_IDLE		0xFF	// NCP not ready, keep clocking
#define SPI_ERR_RESET		0x00	// followed by reset reason
#define SPI_ERR_OVERSIZED	0x01
#define SPI_ERR_ABORTED		0x02
#define SPI_ERR_MISSING_TERM	0x03
#define SPI_ERR_UNSUPPORTED	0x04
#define SEC_TRUST_CENTER_GLOBAL_LINK_KEY	0x0004
#define SEC_HAVE_PRECONFIGURED_KEY		0x0100

struct cfg {
	const char *spidev;
	const char *gpiochip;
	unsigned reset_line;
	unsigned wake_line;
	unsigned int_line;
	unsigned nssel_int_line;
	int reset_set, wake_set, int_set, nssel_int_set, cs_set;
	unsigned cs_line;
	const char *nssel_int_mode;
	const char *identify_led;	// space separated /sys/class/leds names
	unsigned wait_bytes;
	unsigned ezsp_version;	// negotiated; picks the frame layout
	unsigned ezsp_want;	// version to ask for first
	unsigned channel;
	uint16_t pan_id;
	uint8_t ext_pan_id[8];
	int have_pan_id;
	int have_ext_pan_id;
	int tx_power;
	uint16_t sec_bitmask;
	unsigned join_timeout;
	unsigned join_retries;
	unsigned report_secs;
	int no_precheck;
	const char *sock_path;
	const char *bind_addr;	// bridge listen address
	unsigned bridge_port;
	uint32_t speed;
	uint32_t gap_us;
	int no_reset;
	int wake_handshake;
	int quiet;
	int verbose;
};

struct pins {
	int reset, wake, nssel_int, cs;
	int hint;			// -1 if unavailable
};

struct board {
	const char *match;		// substring of /tmp/sysinfo/board_name
	const char *desc;
	unsigned reset, wake, hostint, nssel_int, nssel;
	unsigned forbid;		// line that must never be driven, 0 = none
	const char *forbid_why;
};

enum rsp_class {
	RSP_IDLE,	// nothing but 0xFF came back
	RSP_RESET,	// 0x00, reset occurred (+ reason byte)
	RSP_ERROR,	// 0x01..0x04
	RSP_VERSION,	// 0b10xxxxxx, SPI protocol version
	RSP_STATUS,	// 0b1100000x, NCP status
	RSP_EZSP,	// 0xFE, EZSP frame follows
	RSP_OTHER,
};

#define EZSP_VERSION			0x00
#define EZSP_CALLBACK			0x06
#define EZSP_NETWORK_INIT		0x17
#define EZSP_NETWORK_STATE		0x18
#define EZSP_STACK_STATUS_HANDLER	0x19
#define EZSP_JOIN_NETWORK		0x1F
#define EZSP_LEAVE_NETWORK		0x20
#define EZSP_GET_EUI64			0x26
#define EZSP_GET_NODE_ID		0x27
#define EZSP_GET_NETWORK_PARAMETERS	0x28
#define EZSP_GET_CONFIGURATION_VALUE	0x52
#define EZSP_SET_CONFIGURATION_VALUE	0x53
#define EMBER_NODE_TYPE_ROUTER		0x02
#define EZSP_NO_CALLBACKS		0x07
#define EZSP_SET_INITIAL_SECURITY_STATE	0x68
#define EZSP_CONFIG_PACKET_BUFFER_COUNT	0x01
#define EZSP_CONFIG_STACK_PROFILE	0x0C
#define EZSP_CONFIG_SECURITY_LEVEL	0x0D
#define EZSP_CONFIG_TC_ADDRESS_CACHE_SIZE 0x19
#define EZSP_CONFIG_KEY_TABLE_SIZE	0x1E
#define EMBER_SUCCESS			0x00
#define EMBER_NETWORK_UP		0x90
#define EMBER_NETWORK_DOWN		0x91
#define EMBER_NOT_JOINED		0x93
#define EMBER_USE_MAC_ASSOCIATION	0x00
#define EZSP_START_SCAN			0x1A
#define EZSP_NETWORK_FOUND_HANDLER	0x1B
#define EZSP_SCAN_COMPLETE_HANDLER	0x1C
#define EZSP_ACTIVE_SCAN		0x01
#define EZSP_PERMIT_JOINING		0x22
#define EZSP_CHILD_JOIN_HANDLER		0x23
#define EZSP_TRUST_CENTER_JOIN_HANDLER	0x24
#define EZSP_GET_CHILD_DATA		0x4A
#define EZSP_GET_PARENT_CHILD_PARAMS	0x29
#define EZSP_GET_NEIGHBOR		0x79
#define EZSP_NEIGHBOR_COUNT		0x7A
#define EZSP_GET_ROUTE_TABLE_ENTRY	0x7B
#define EZSP_GET_KEY			0x6A
#define EMBER_TRUST_CENTER_LINK_KEY	0x01
#define EMBER_CURRENT_NETWORK_KEY	0x03
#define EZSP_GET_POLICY			0x56
#define EZSP_SET_POLICY			0x55
#define EZSP_SET_RADIO_POWER		0x99
#define EZSP_GET_BOOTLOADER_VERSION	0x91
#define EZSP_ADD_ENDPOINT		0x02
#define EZSP_SEND_UNICAST		0x34
#define EZSP_INCOMING_MESSAGE_HANDLER	0x45
#define EZSP_MESSAGE_SENT_HANDLER	0x3F
#define ZCL_PROFILE_HA			0x0104
#define ZCL_PROFILE_ZDO			0x0000
#define ZCL_DEVICE_RANGE_EXTENDER	0x0008	// what this node actually is
#define ZCL_CLUSTER_BASIC		0x0000
#define ZCL_CLUSTER_IDENTIFY		0x0003
#define ZCL_CMD_IDENTIFY		0x00
#define ZCL_CMD_IDENTIFY_QUERY		0x01
#define ZDO_MGMT_PERMIT_JOINING_REQ	0x0036
#define ZCL_ENDPOINT			1
#define ASH_FLAG		0x7E
#define ASH_ESC			0x7D
#define ASH_XON			0x11
#define ASH_XOFF		0x13
#define ASH_SUB			0x18
#define ASH_CANCEL		0x1A
#define ASH_CTRL_RST		0xC0
#define ASH_CTRL_RSTACK		0xC1
#define ASH_CTRL_ERROR		0xC2
#define ASH_MAX_FRAME		140	// EZSP frame plus control byte
#define ASH_WINDOW		8	// frame numbers are 3 bits
#define ASH_RESET_SOFTWARE	0x0B
struct bridge {
	int spi;
	struct pins *p;

	int srv;			// listening socket
	int cli;			// connected client, -1 if none

	int ready;		// RST answered with RSTACK; may send
	int negotiated;		// EZSP version agreed; may poll callbacks

	uint8_t frm;			// next frame number we send
	uint8_t ack;			// next frame number we expect
	uint8_t cbseq;			// sequence for our own callback polls

	int ver;		// negotiated EZSP version, sniffed in passing

	uint8_t rx[ASH_MAX_FRAME * 2];
	size_t rxlen;
	int esc;

	// Unacked DATA frames, indexed by frame number, for NAK replies.
	struct {
		uint8_t body[ASH_MAX_FRAME];
		size_t len;
	} sent[ASH_WINDOW];
};


// Cross-module declarations.
extern int cs_follows;
extern struct cfg cfg;
void die(const char *fmt, ...);
void msleep(unsigned ms);
void hexdump(const char *tag, const uint8_t *buf, size_t len);
extern const char *current_cmd;
void explain_busy(unsigned offset);
int gpio_request_output(const char *chip, unsigned offset, int value, const char *consumer);
int gpio_try_output(const char *chip, unsigned offset, int value, const char *consumer);
int gpio_request_input_edges(const char *chip, unsigned offset, const char *consumer);
void gpio_set(int fd, int value);
int gpio_get(int fd);
int gpio_wait_falling(int fd, int timeout_ms);
void gpio_drain_events(int fd);
int spi_open(const char *path, uint32_t speed);
void spi_raw(int fd, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs);
size_t spi_txn_framed(int spi, struct pins *p, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen);
int spi_first_response(const uint8_t *rx, size_t rxlen);
const char *reset_reason(uint8_t code);
const char *spi_error_name(uint8_t code);
int decode_response(const uint8_t *rx, size_t rxlen, const char *what);
void cs_assert(struct pins *p);
void cs_release(struct pins *p);
extern const struct board boards[];
const struct board *board_detect(void);
void pins_autodetect(void);
void pins_open(struct pins *p);
void pins_close(struct pins *p);
void ncp_reset(struct pins *p);
void ncp_wake(struct pins *p);
enum rsp_class classify(const uint8_t *rx, size_t rxlen, uint8_t *first, uint8_t *data);
enum rsp_class do_cmd(int spi, struct pins *p, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen, int retry_on_reset);
int cmd_probe(void);
extern const uint8_t zigbee_alliance_09[16];
int ezsp_cmd(int spi, struct pins *p, uint8_t frame_id, const uint8_t *params, size_t plen, uint8_t *payload, size_t payload_max);
int ezsp_xfer(int spi, struct pins *p, uint8_t frame_id, const uint8_t *params, size_t plen, uint8_t *out_id, uint8_t *payload, size_t payload_max);
int ezsp_init(int spi, struct pins *p);
const char *ember_status_name(uint8_t s);
int scan_find(int spi, struct pins *p, unsigned channel, uint16_t pan_id, uint8_t ext[8], int *allowing, uint8_t *nwk_update_id);
int cmd_scan(void);
const char *ezsp_config_name(uint8_t id);
int ezsp_permit(int spi, struct pins *p, unsigned seconds, char *msg, size_t max);
int cmd_neighbors(void);
int cmd_keys(void);
const char *ezsp_policy_name(uint8_t id);
const char *ezsp_decision_hint(uint8_t policy, uint8_t decision);
int cmd_policy(void);
int cmd_config(void);
const char *network_state_name(uint8_t s);
const char *node_type_name(uint8_t t);
int cmd_info(void);
int ezsp_poll_callback(int spi, struct pins *p, uint8_t *payload, size_t max);
int ezsp_set_config(int spi, struct pins *p, uint8_t id, uint16_t value, const char *what, int announce);
int ezsp_set_power(int spi, struct pins *p, int dbm, char *msg, size_t max);
extern const char zcl_manufacturer[];
extern const char zcl_model[];
extern const char zcl_swbuild[];
unsigned report_interval(void);
void zcl_default_response(int spi, struct pins *p, uint16_t sender, uint16_t cluster, uint8_t in_fc, uint8_t seq, uint8_t cmd, uint8_t status);
void identify_start(unsigned secs);
void identify_tick(void);
int identify_active(void);
extern unsigned identify_time;
extern time_t report_last;
const char *zcl_location(void);
int ezsp_add_endpoint(int spi, struct pins *p);
int zcl_send(int spi, struct pins *p, uint16_t dest, uint16_t cluster, const uint8_t *payload, size_t len);
size_t zcl_append_attr(uint8_t *b, size_t at, size_t max, uint16_t id);
void zcl_handle_incoming(int spi, struct pins *p, const uint8_t *m, size_t len);
void zcl_report(int spi, struct pins *p);
void ezsp_apply_config(int spi, struct pins *p, int announce);
int cmd_join(void);
int cmd_leave(void);
int control_listen(const char *path);
void control_status(int spi, struct pins *p, char *buf, size_t max);
void control_accept(int listen_fd, int spi, struct pins *p);
int control_query(const char *path, const char *cmd);
int cmd_status(void);
int cmd_permit(const char *arg);
int cmd_power(const char *arg);
int cmd_verify(const char *path);
int cmd_bootloader(void);
int cmd_run(void);
extern int reverse_ext_pan_id;
void parse_ext_pan_id(const char *s);
extern uint8_t ash_rand[256];
void ash_rand_init(void);
uint16_t ash_crc(const uint8_t *d, size_t n);
int write_all(int fd, const uint8_t *buf, size_t len);
int ash_send(struct bridge *b, const uint8_t *frame, size_t len);
void ash_send_ack(struct bridge *b);
void ash_send_data(struct bridge *b, const uint8_t *ezsp, size_t len);
void ash_retransmit(struct bridge *b, uint8_t from);
int ezsp_relay(int spi, struct pins *p, const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax);
void ezsp_layout(int ver, int *hdr, int *id_off);
int ezsp_relay_callback(struct bridge *b, uint8_t *out, size_t outmax);
void bridge_drain_callbacks(struct bridge *b);
void bridge_reset(struct bridge *b);
void bridge_handle_frame(struct bridge *b, uint8_t *f, size_t len);
void bridge_feed(struct bridge *b, const uint8_t *buf, size_t n);
void bridge_drop_client(struct bridge *b, const char *why);
int bridge_listen(const char *addr, int port);
int cmd_bridge(void);
void usage(void);
int main(int argc, char **argv);

extern unsigned report_count;
extern unsigned report_failed;

#endif
