#include "ezsp-spi.h"

// -------------------------------------------------------------------- GPIO --
//
// The character device is used rather than the deprecated /sys/class/gpio, so
// line offsets here are raw board GPIO numbers (49, 31, 50, 29) with no 512
// base offset, and nHOST_INT can be edge-watched instead of polled.
//
// Values are physical levels throughout. These signals are all active low, but
// tracking logical polarity on top of that just makes debugging harder.

// A line exported through the legacy /sys/class/gpio interface is claimed by
// the kernel and cannot also be requested through the chardev, so the vendor
// zbshim.sh setup (which exports 541/543/561/562) locks this tool out with
// EBUSY. Point at the culprit instead of just reporting "Resource busy".
const char *current_cmd;

void explain_busy(unsigned offset)
{
	static const unsigned bases[] = { 512, 0 };
	char path[64];
	unsigned i;

	for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u",
			 bases[i] + offset);

		if (access(path, F_OK) == 0) {
			fprintf(stderr,
"\n%s exists: gpio%u is exported through legacy sysfs, which blocks the\n"
"gpio chardev. This is what zbshim.sh does. Release it with:\n\n"
"    echo %u > /sys/class/gpio/unexport\n\n"
"or clear the whole vendor set:\n\n"
"    /root/zbshim.sh stop      # releases every line the chroot took\n"
"    for g in 541 543 557 561 562; do\n"
"        echo $g > /sys/class/gpio/unexport 2>/dev/null; done\n\n",
				path, offset, bases[i] + offset);
			return;
		}
	}

	fprintf(stderr,
"\ngpio%u is claimed by something else.\n\n"
"Most likely the router service already owns it -- \"run\" holds these lines\n"
"for as long as it is up, and the gpio chardev is exclusive. That is correct\n"
"behaviour, not a fault. To run a one-off command, stop it first:\n\n"
"    /etc/init.d/ezsp-spi stop\n"
"    ezsp-spi %s\n"
"    /etc/init.d/ezsp-spi start\n\n"
"Note this drops the radio off the mesh for the duration. Otherwise, find the\n"
"owner with:\n\n"
"    mount -t debugfs none /sys/kernel/debug 2>/dev/null\n"
"    grep -iE 'ezsp|gpio-%u' /sys/kernel/debug/gpio\n\n"
"(debugfs labels lines by global number, so gpio%u shows up as gpio-%u.)\n\n",
		offset, current_cmd ? current_cmd : "info",
		512 + offset, offset, 512 + offset);
}

int gpio_request_output(const char *chip, unsigned offset, int value,
			       const char *consumer)
{
	struct gpio_v2_line_request req;
	int fd, ret;

	fd = open(chip, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("open %s: %s", chip, strerror(errno));

	memset(&req, 0, sizeof(req));
	req.offsets[0] = offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = value ? 1 : 0;
	req.config.attrs[0].mask = 1;
	snprintf(req.consumer, sizeof(req.consumer), "%s", consumer);

	ret = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
	close(fd);
	if (ret < 0) {
		int err = errno;

		if (err == EBUSY)
			explain_busy(offset);
		die("request gpio%u as output (%s): %s", offset, consumer,
		    strerror(err));
	}

	return req.fd;
}

// Like gpio_request_output but returns -1 instead of dying, for lines that may
// legitimately belong to someone else.
int gpio_try_output(const char *chip, unsigned offset, int value,
			   const char *consumer)
{
	struct gpio_v2_line_request req;
	int fd, ret;

	fd = open(chip, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	memset(&req, 0, sizeof(req));
	req.offsets[0] = offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	req.config.num_attrs = 1;
	req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
	req.config.attrs[0].attr.values = value ? 1 : 0;
	req.config.attrs[0].mask = 1;
	snprintf(req.consumer, sizeof(req.consumer), "%s", consumer);

	ret = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
	close(fd);

	return ret < 0 ? -1 : req.fd;
}

int gpio_request_input_edges(const char *chip, unsigned offset,
				    const char *consumer)
{
	struct gpio_v2_line_request req;
	int fd, ret;

	fd = open(chip, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("open %s: %s", chip, strerror(errno));

	memset(&req, 0, sizeof(req));
	req.offsets[0] = offset;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_INPUT |
			   GPIO_V2_LINE_FLAG_EDGE_RISING |
			   GPIO_V2_LINE_FLAG_EDGE_FALLING;
	req.event_buffer_size = 64;
	snprintf(req.consumer, sizeof(req.consumer), "%s", consumer);

	ret = ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req);
	close(fd);
	if (ret < 0)
		return -1;	// non-fatal: caller decides

	return req.fd;
}

void gpio_set(int fd, int value)
{
	struct gpio_v2_line_values vals = {
		.bits = value ? 1 : 0,
		.mask = 1,
	};

	if (ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &vals) < 0)
		die("gpio set: %s", strerror(errno));
}

int gpio_get(int fd)
{
	struct gpio_v2_line_values vals = { .mask = 1 };

	if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0)
		return -1;

	return vals.bits & 1;
}

// Wait for nHOST_INT to go low (asserted). Returns ms waited, or -1 on timeout.
int gpio_wait_falling(int fd, int timeout_ms)
{
	struct gpio_v2_line_event ev;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	struct timespec t0, t1;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	for (;;) {
		ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (ret == 0)
			return -1;

		if (read(fd, &ev, sizeof(ev)) != sizeof(ev))
			return -1;

		if (ev.id != GPIO_V2_LINE_EVENT_FALLING_EDGE)
			continue;	// rising edge, keep waiting

		clock_gettime(CLOCK_MONOTONIC, &t1);
		return (int)((t1.tv_sec - t0.tv_sec) * 1000 +
			     (t1.tv_nsec - t0.tv_nsec) / 1000000);
	}
}

void gpio_drain_events(int fd)
{
	struct gpio_v2_line_event ev;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	while (poll(&pfd, 1, 0) == 1)
		if (read(fd, &ev, sizeof(ev)) != sizeof(ev))
			break;
}


// Defined with the other pin handling below.
void cs_assert(struct pins *p);
void cs_release(struct pins *p);
