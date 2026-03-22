/*
 * sample_touch_poll - Poll CST128A touch data over I2C without IRQ/input
 *
 * Usage:
 *   sample_touch_poll
 *   sample_touch_poll -b /dev/i2c-4 -a 0x38 -i 20
 *
 * This is intended for debug when the controller probes successfully but
 * no input events arrive, so we can distinguish "IRQ path is broken" from
 * "controller is not reporting touch data at all".
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CST128A_REG_TD_STATUS 0x02
#define CST128A_MAX_TOUCH_POINTS 5

#define TOUCH_EVENT_DOWN     0x00
#define TOUCH_EVENT_UP       0x01
#define TOUCH_EVENT_CONTACT  0x02
#define TOUCH_EVENT_RESERVED 0x03

static volatile int g_running = 1;

struct touch_point {
	int valid;
	int id;
	int event;
	int x;
	int y;
};

struct touch_state {
	uint8_t status;
	struct touch_point points[CST128A_MAX_TOUCH_POINTS];
};

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("\n");
	printf("Options:\n");
	printf("  -b <dev>   I2C device node (default: /dev/i2c-4)\n");
	printf("  -a <addr>  I2C address (default: 0x38)\n");
	printf("  -i <ms>    Poll interval in milliseconds (default: 20)\n");
	printf("  -r <cnt>   Print raw frame every N polls even if unchanged (default: 50)\n");
	printf("  -h         Show this help\n");
	printf("\n");
	printf("Notes:\n");
	printf("  This tool talks to the controller directly and does not use IRQ/input.\n");
	printf("  If the kernel driver is loaded, this tool uses I2C_SLAVE_FORCE.\n");
}

static int i2c_read_reg(int fd, uint8_t reg, uint8_t *buf, size_t len)
{
	if (write(fd, &reg, 1) != 1) {
		fprintf(stderr, "[I2C] Failed to write register 0x%02x: %s\n",
			reg, strerror(errno));
		return -1;
	}

	if (read(fd, buf, len) != (ssize_t)len) {
		fprintf(stderr, "[I2C] Failed to read register 0x%02x: %s\n",
			reg, strerror(errno));
		return -1;
	}

	return 0;
}

static void parse_frame(const uint8_t *buf, struct touch_state *state)
{
	int i;

	memset(state, 0, sizeof(*state));
	state->status = buf[0];

	for (i = 0; i < CST128A_MAX_TOUCH_POINTS; i++) {
		const uint8_t *p = &buf[i * 6 + 1];
		struct touch_point *tp = &state->points[i];

		tp->event = p[0] >> 6;
		if (tp->event == TOUCH_EVENT_RESERVED)
			continue;

		tp->id = (p[2] >> 4) & 0x0f;
		tp->x = ((p[0] & 0x0f) << 8) | p[1];
		tp->y = ((p[2] & 0x0f) << 8) | p[3];
		tp->valid = 1;
	}
}

static int state_equal(const struct touch_state *a, const struct touch_state *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static const char *event_name(int event)
{
	switch (event) {
	case TOUCH_EVENT_DOWN:
		return "DOWN";
	case TOUCH_EVENT_UP:
		return "UP";
	case TOUCH_EVENT_CONTACT:
		return "CONTACT";
	case TOUCH_EVENT_RESERVED:
	default:
		return "RESV";
	}
}

static void dump_raw_frame(const uint8_t *buf, size_t len)
{
	size_t i;

	printf("[RAW] ");
	for (i = 0; i < len; i++)
		printf("%02x ", buf[i]);
	printf("\n");
}

static void print_state(const struct touch_state *state)
{
	int i;
	int printed = 0;

	printf("[POLL] status=0x%02x", state->status);
	for (i = 0; i < CST128A_MAX_TOUCH_POINTS; i++) {
		const struct touch_point *tp = &state->points[i];

		if (!tp->valid)
			continue;

		printf(" | slot%d id=%d %s x=%d y=%d",
		       i, tp->id, event_name(tp->event), tp->x, tp->y);
		printed = 1;
	}

	if (!printed)
		printf(" | no valid touch points");
	printf("\n");
}

int main(int argc, char *argv[])
{
	const char *i2c_dev = "/dev/i2c-4";
	int addr = 0x38;
	int interval_ms = 20;
	int raw_every = 50;
	int fd;
	int opt;
	unsigned long poll_count = 0;
	uint8_t frame[29];
	struct touch_state cur, prev;

	memset(&prev, 0xff, sizeof(prev));

	while ((opt = getopt(argc, argv, "b:a:i:r:h")) != -1) {
		switch (opt) {
		case 'b':
			i2c_dev = optarg;
			break;
		case 'a':
			addr = (int)strtol(optarg, NULL, 0);
			break;
		case 'i':
			interval_ms = atoi(optarg);
			break;
		case 'r':
			raw_every = atoi(optarg);
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	printf("========================================\n");
	printf("  sample_touch_poll - I2C Polling Tool\n");
	printf("========================================\n\n");
	printf("[CFG] dev=%s addr=0x%02x interval=%dms raw_every=%d\n",
	       i2c_dev, addr, interval_ms, raw_every);
	printf("[CFG] Touch the panel and watch for status/coordinate changes.\n");
	printf("[CFG] Ctrl+C to stop.\n\n");

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	fd = open(i2c_dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "[I2C] Cannot open %s: %s\n", i2c_dev, strerror(errno));
		return 1;
	}

	if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
		fprintf(stderr, "[I2C] Cannot select address 0x%02x: %s\n",
			addr, strerror(errno));
		close(fd);
		return 1;
	}

	while (g_running) {
		poll_count++;
		if (i2c_read_reg(fd, CST128A_REG_TD_STATUS, frame, sizeof(frame)) == 0) {
			parse_frame(frame, &cur);
			if (poll_count == 1 ||
			    !state_equal(&cur, &prev) ||
			    (raw_every > 0 && (poll_count % (unsigned long)raw_every) == 0)) {
				printf("[POLL #%lu] ", poll_count);
				print_state(&cur);
				dump_raw_frame(frame, sizeof(frame));
				prev = cur;
			}
		}

		usleep(interval_ms * 1000);
	}

	close(fd);
	printf("\nStopped.\n");
	return 0;
}
