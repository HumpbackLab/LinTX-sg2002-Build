/*
 * sample_touch - Touchscreen diagnostic tool for LicheeRV Nano
 *
 * Usage:
 *   sample_touch                     # auto-detect /dev/input/event0, print events
 *   sample_touch /dev/input/event1   # specify device
 *   sample_touch -f                  # draw touch points on /dev/fb0
 *   sample_touch -f /dev/input/event0  # both
 *
 * Functions:
 *   1. Detect and list all input devices, find touchscreen automatically
 *   2. Print raw touch events (coordinates, pressure, slot)
 *   3. Optional: draw touch points on framebuffer for visual feedback (-f)
 *   4. Report I2C bus / device tree status for debugging
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <linux/fb.h>

/* ------------------------------------------------------------------ */
/* Color definitions (RGB565)                                         */
/* ------------------------------------------------------------------ */
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_WHITE     0xFFFF
#define COLOR_BLACK     0x0000
#define COLOR_YELLOW    0xFFE0
#define COLOR_CYAN      0x07FF

#define CROSS_SIZE      15       /* half-size of crosshair */
#define MAX_SLOTS       10

static volatile int g_running = 1;

/* ------------------------------------------------------------------ */
/* Framebuffer context                                                */
/* ------------------------------------------------------------------ */
struct fb_ctx {
	int fd;
	unsigned short *mem;
	unsigned int width;
	unsigned int height;
	unsigned int stride;     /* bytes per line */
	unsigned int size;
};

/* ------------------------------------------------------------------ */
/* Touch state per slot                                               */
/* ------------------------------------------------------------------ */
struct touch_slot {
	int active;
	int x;
	int y;
};

static struct touch_slot g_slots[MAX_SLOTS];
static int g_cur_slot = 0;

/* ------------------------------------------------------------------ */
static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Framebuffer helpers                                                */
/* ------------------------------------------------------------------ */
static int fb_open(struct fb_ctx *fb, const char *dev)
{
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;

	fb->fd = open(dev, O_RDWR);
	if (fb->fd < 0) {
		fprintf(stderr, "[FB] Cannot open %s: %s\n", dev, strerror(errno));
		return -1;
	}

	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		fprintf(stderr, "[FB] FBIOGET_FSCREENINFO failed: %s\n", strerror(errno));
		close(fb->fd);
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		fprintf(stderr, "[FB] FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
		close(fb->fd);
		return -1;
	}

	fb->width  = vinfo.xres;
	fb->height = vinfo.yres;
	fb->stride = finfo.line_length;
	fb->size   = finfo.smem_len;

	printf("[FB] %s: %ux%u, %ubpp, stride=%u, size=%u\n",
	       dev, fb->width, fb->height, vinfo.bits_per_pixel,
	       fb->stride, fb->size);

	if (vinfo.bits_per_pixel != 16) {
		fprintf(stderr, "[FB] Warning: expected 16bpp (RGB565), got %ubpp\n",
			vinfo.bits_per_pixel);
		fprintf(stderr, "[FB] Framebuffer drawing may look incorrect\n");
	}

	fb->mem = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		fprintf(stderr, "[FB] mmap failed: %s\n", strerror(errno));
		close(fb->fd);
		return -1;
	}

	return 0;
}

static void fb_close(struct fb_ctx *fb)
{
	if (fb->mem && fb->mem != MAP_FAILED)
		munmap(fb->mem, fb->size);
	if (fb->fd >= 0)
		close(fb->fd);
}

static inline void fb_pixel(struct fb_ctx *fb, int x, int y,
			     unsigned short color)
{
	if (x < 0 || x >= (int)fb->width || y < 0 || y >= (int)fb->height)
		return;
	/* stride is in bytes, each pixel is 2 bytes for RGB565 */
	unsigned short *row = (unsigned short *)((char *)fb->mem + y * fb->stride);
	row[x] = color;
}

static void fb_clear(struct fb_ctx *fb, unsigned short color)
{
	unsigned int total = fb->size / 2;
	for (unsigned int i = 0; i < total; i++)
		fb->mem[i] = color;
}

static void fb_draw_cross(struct fb_ctx *fb, int cx, int cy,
			   unsigned short color)
{
	int i;
	/* horizontal line */
	for (i = -CROSS_SIZE; i <= CROSS_SIZE; i++)
		fb_pixel(fb, cx + i, cy, color);
	/* vertical line */
	for (i = -CROSS_SIZE; i <= CROSS_SIZE; i++)
		fb_pixel(fb, cx, cy + i, color);
	/* small box at center */
	for (i = -2; i <= 2; i++) {
		fb_pixel(fb, cx + i, cy - 2, color);
		fb_pixel(fb, cx + i, cy + 2, color);
		fb_pixel(fb, cx - 2, cy + i, color);
		fb_pixel(fb, cx + 2, cy + i, color);
	}
}

/* Draw info text area at top of screen */
static void fb_draw_header(struct fb_ctx *fb)
{
	/* Just draw a thin colored bar at top as visual indicator */
	for (unsigned int x = 0; x < fb->width; x++) {
		fb_pixel(fb, x, 0, COLOR_CYAN);
		fb_pixel(fb, x, 1, COLOR_CYAN);
	}
}

/* ------------------------------------------------------------------ */
/* Input device helpers                                               */
/* ------------------------------------------------------------------ */

/* Check if a given input device is a touchscreen */
static int is_touchscreen(int fd)
{
	unsigned long evbit[2] = {0};
	unsigned long absbit[2] = {0};

	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0)
		return 0;

	/* Must have EV_ABS */
	if (!(evbit[0] & (1 << EV_ABS)))
		return 0;

	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) < 0)
		return 0;

	/* Must have ABS_MT_POSITION_X or ABS_X */
	if ((absbit[0] & (1 << ABS_X)) || (absbit[1] & (1 << (ABS_MT_POSITION_X - 32))))
		return 1;

	return 0;
}

/* Auto-detect touchscreen device */
static int find_touchscreen(char *path, size_t pathlen)
{
	char devpath[128];
	char name[256];
	DIR *dir;
	struct dirent *ent;
	int fd;

	dir = opendir("/dev/input");
	if (!dir) {
		fprintf(stderr, "[DETECT] Cannot open /dev/input: %s\n", strerror(errno));
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(devpath, sizeof(devpath), "/dev/input/%s", ent->d_name);
		fd = open(devpath, O_RDONLY);
		if (fd < 0)
			continue;

		name[0] = '\0';
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);

		if (is_touchscreen(fd)) {
			printf("[DETECT] Found touchscreen: %s (%s)\n", devpath, name);
			snprintf(path, pathlen, "%s", devpath);
			close(fd);
			closedir(dir);
			return 0;
		}

		printf("[DETECT] %s: %s (not a touchscreen)\n", devpath, name);
		close(fd);
	}

	closedir(dir);
	return -1;
}

/* List all input devices */
static void list_input_devices(void)
{
	char devpath[128];
	char name[256];
	DIR *dir;
	struct dirent *ent;
	int fd;

	printf("\n=== Input Devices ===\n");
	dir = opendir("/dev/input");
	if (!dir) {
		printf("  Cannot open /dev/input: %s\n", strerror(errno));
		return;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(devpath, sizeof(devpath), "/dev/input/%s", ent->d_name);
		fd = open(devpath, O_RDONLY);
		if (fd < 0) {
			printf("  %s: cannot open (%s)\n", devpath, strerror(errno));
			continue;
		}

		name[0] = '\0';
		ioctl(fd, EVIOCGNAME(sizeof(name)), name);

		printf("  %s: %s %s\n", devpath, name,
		       is_touchscreen(fd) ? "[TOUCHSCREEN]" : "");
		close(fd);
	}
	closedir(dir);
	printf("\n");
}

/* ------------------------------------------------------------------ */
/* Diagnostic: check system status                                    */
/* ------------------------------------------------------------------ */
static void check_system(void)
{
	FILE *fp;
	char buf[512];

	printf("=== System Diagnostics ===\n\n");

	/* Check device tree node */
	printf("[DT] Checking device tree for touch node...\n");
	fp = fopen("/proc/device-tree/soc/i2c@04040000/cst128a@38/compatible", "r");
	if (fp) {
		memset(buf, 0, sizeof(buf));
		fread(buf, 1, sizeof(buf) - 1, fp);
		printf("  Found: compatible = \"%s\"\n", buf);
		fclose(fp);
	} else {
		printf("  NOT found at i2c@04040000/cst128a@38\n");
		printf("  Trying to list i2c@04040000 children...\n");
		/* Try to list what's under i2c4 */
		DIR *dir = opendir("/proc/device-tree/soc/i2c@04040000");
		if (dir) {
			struct dirent *ent;
			while ((ent = readdir(dir)) != NULL) {
				if (ent->d_name[0] != '.')
					printf("    %s\n", ent->d_name);
			}
			closedir(dir);
		} else {
			printf("  Cannot open i2c@04040000 in device tree\n");
		}
	}

	/* Check I2C bus */
	printf("\n[I2C] Checking I2C-4 bus...\n");
	if (access("/dev/i2c-4", F_OK) == 0)
		printf("  /dev/i2c-4 exists\n");
	else
		printf("  /dev/i2c-4 NOT found (I2C4 may not be enabled)\n");

	/* Check driver load */
	printf("\n[DRV] Checking driver status...\n");
	fp = fopen("/proc/modules", "r");
	if (fp) {
		int found = 0;
		while (fgets(buf, sizeof(buf), fp)) {
			if (strstr(buf, "cst128a")) {
				printf("  Module loaded: %s", buf);
				found = 1;
			}
		}
		if (!found)
			printf("  cst128a module not in /proc/modules (may be built-in)\n");
		fclose(fp);
	}

	/* Check dmesg for driver messages */
	printf("\n[DMESG] Searching kernel log for touch-related messages...\n");
	fp = popen("dmesg 2>/dev/null | grep -i -E 'cst128|touch|input.*event|i2c.*4.*error' | tail -20", "r");
	if (fp) {
		int lines = 0;
		while (fgets(buf, sizeof(buf), fp)) {
			printf("  %s", buf);
			lines++;
		}
		if (lines == 0)
			printf("  (no touch-related messages found)\n");
		pclose(fp);
	}

	/* Check GPIO status */
	printf("\n[GPIO] Checking PWRGPIO3 (IRQ) and PWRGPIO4 (RST)...\n");
	fp = fopen("/sys/kernel/debug/gpio", "r");
	if (fp) {
		while (fgets(buf, sizeof(buf), fp)) {
			if (strstr(buf, "355") || strstr(buf, "356") ||
			    strstr(buf, "cst128") || strstr(buf, "porte"))
				printf("  %s", buf);
		}
		fclose(fp);
	} else {
		/* Try sysfs approach */
		printf("  (debugfs not available, checking sysfs)\n");
		if (access("/sys/class/gpio/gpio355", F_OK) == 0)
			printf("  GPIO 355 (IRQ) exported\n");
		if (access("/sys/class/gpio/gpio356", F_OK) == 0)
			printf("  GPIO 356 (RST) exported\n");
	}

	printf("\n");
}

/* ------------------------------------------------------------------ */
/* Print event in human-readable form                                 */
/* ------------------------------------------------------------------ */
static const char *event_type_str(unsigned short type)
{
	switch (type) {
	case EV_SYN: return "SYN";
	case EV_KEY: return "KEY";
	case EV_ABS: return "ABS";
	default:     return "???";
	}
}

static const char *abs_code_str(unsigned short code)
{
	switch (code) {
	case ABS_X:              return "ABS_X";
	case ABS_Y:              return "ABS_Y";
	case ABS_MT_SLOT:        return "MT_SLOT";
	case ABS_MT_POSITION_X:  return "MT_POS_X";
	case ABS_MT_POSITION_Y:  return "MT_POS_Y";
	case ABS_MT_TRACKING_ID: return "MT_TRACK_ID";
	case ABS_MT_TOUCH_MAJOR: return "MT_TOUCH_MAJ";
	case ABS_MT_PRESSURE:    return "MT_PRESSURE";
	default:                 return "???";
	}
}

/* ------------------------------------------------------------------ */
/* Main event loop                                                    */
/* ------------------------------------------------------------------ */
static void print_usage(const char *prog)
{
	printf("Usage: %s [options] [/dev/input/eventX]\n", prog);
	printf("\n");
	printf("Options:\n");
	printf("  -f          Draw touch points on /dev/fb0\n");
	printf("  -d          Run diagnostics only (no event reading)\n");
	printf("  -h          Show this help\n");
	printf("\n");
	printf("If no device is specified, auto-detects the touchscreen.\n");
	printf("Press Ctrl+C to exit.\n");
}

int main(int argc, char *argv[])
{
	char devpath[128] = "";
	int use_fb = 0;
	int diag_only = 0;
	struct fb_ctx fb = { .fd = -1 };
	int evfd;
	struct input_event ev;
	int opt;
	unsigned long event_count = 0;

	while ((opt = getopt(argc, argv, "fdh")) != -1) {
		switch (opt) {
		case 'f':
			use_fb = 1;
			break;
		case 'd':
			diag_only = 1;
			break;
		case 'h':
			print_usage(argv[0]);
			return 0;
		default:
			print_usage(argv[0]);
			return 1;
		}
	}

	/* Remaining non-option argument is device path */
	if (optind < argc)
		snprintf(devpath, sizeof(devpath), "%s", argv[optind]);

	printf("========================================\n");
	printf("  sample_touch - Touch Diagnostic Tool\n");
	printf("========================================\n\n");

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	/* Always run diagnostics first */
	check_system();
	list_input_devices();

	if (diag_only) {
		printf("Diagnostics complete. Exiting.\n");
		return 0;
	}

	/* Find device */
	if (devpath[0] == '\0') {
		if (find_touchscreen(devpath, sizeof(devpath)) < 0) {
			fprintf(stderr, "[ERROR] No touchscreen device found!\n");
			fprintf(stderr, "  Possible causes:\n");
			fprintf(stderr, "  1. Touch driver not loaded (check dmesg)\n");
			fprintf(stderr, "  2. I2C communication failed (check i2c-4)\n");
			fprintf(stderr, "  3. Wrong I2C address (try i2cdetect -y 4)\n");
			return 1;
		}
	}

	/* Open input device */
	evfd = open(devpath, O_RDONLY);
	if (evfd < 0) {
		fprintf(stderr, "[ERROR] Cannot open %s: %s\n",
			devpath, strerror(errno));
		return 1;
	}

	{
		char name[256] = "Unknown";
		ioctl(evfd, EVIOCGNAME(sizeof(name)), name);
		printf("[TOUCH] Opened: %s (%s)\n", devpath, name);

		/* Print ABS info */
		struct input_absinfo absinfo;
		if (ioctl(evfd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo) == 0) {
			printf("[TOUCH] ABS_MT_X: min=%d max=%d\n",
			       absinfo.minimum, absinfo.maximum);
		}
		if (ioctl(evfd, EVIOCGABS(ABS_MT_POSITION_Y), &absinfo) == 0) {
			printf("[TOUCH] ABS_MT_Y: min=%d max=%d\n",
			       absinfo.minimum, absinfo.maximum);
		}
	}

	/* Open framebuffer if requested */
	if (use_fb) {
		if (fb_open(&fb, "/dev/fb0") == 0) {
			fb_clear(&fb, COLOR_BLACK);
			fb_draw_header(&fb);
			printf("[FB] Framebuffer enabled - touch points will be drawn\n");
		} else {
			fprintf(stderr, "[FB] Cannot open framebuffer, continuing without\n");
			use_fb = 0;
		}
	}

	printf("\n--- Reading touch events (Ctrl+C to stop) ---\n\n");

	memset(g_slots, 0, sizeof(g_slots));

	while (g_running) {
		ssize_t n = read(evfd, &ev, sizeof(ev));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[ERROR] read failed: %s\n", strerror(errno));
			break;
		}
		if (n != sizeof(ev))
			continue;

		if (ev.type == EV_ABS) {
			switch (ev.code) {
			case ABS_MT_SLOT:
				g_cur_slot = ev.value;
				if (g_cur_slot >= MAX_SLOTS)
					g_cur_slot = 0;
				break;
			case ABS_MT_TRACKING_ID:
				if (ev.value == -1) {
					/* finger lift */
					g_slots[g_cur_slot].active = 0;
					printf("[SLOT %d] UP\n", g_cur_slot);
				} else {
					g_slots[g_cur_slot].active = 1;
					printf("[SLOT %d] DOWN (id=%d)\n",
					       g_cur_slot, ev.value);
				}
				break;
			case ABS_MT_POSITION_X:
				g_slots[g_cur_slot].x = ev.value;
				printf("[SLOT %d] X=%d\n", g_cur_slot, ev.value);
				break;
			case ABS_MT_POSITION_Y:
				g_slots[g_cur_slot].y = ev.value;
				printf("[SLOT %d] Y=%d\n", g_cur_slot, ev.value);
				break;
			case ABS_X:
				g_slots[0].x = ev.value;
				printf("[ST] X=%d\n", ev.value);
				break;
			case ABS_Y:
				g_slots[0].y = ev.value;
				printf("[ST] Y=%d\n", ev.value);
				break;
			default:
				printf("[ABS] code=%s(%d) value=%d\n",
				       abs_code_str(ev.code), ev.code, ev.value);
				break;
			}
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			printf("[BTN] BTN_TOUCH %s\n",
			       ev.value ? "PRESSED" : "RELEASED");
			if (!ev.value)
				g_slots[0].active = 0;
			else
				g_slots[0].active = 1;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			event_count++;
			/* Print summary line */
			printf("--- SYN #%lu: ", event_count);
			for (int i = 0; i < MAX_SLOTS; i++) {
				if (g_slots[i].active)
					printf("[%d](%d,%d) ", i,
					       g_slots[i].x, g_slots[i].y);
			}
			printf("---\n");

			/* Draw on framebuffer */
			if (use_fb && fb.fd >= 0) {
				/* Fade previous frame slightly instead of clear
				 * for trail effect */
				if (event_count % 60 == 0) {
					fb_clear(&fb, COLOR_BLACK);
					fb_draw_header(&fb);
				}

				unsigned short colors[] = {
					COLOR_RED, COLOR_GREEN, COLOR_BLUE,
					COLOR_YELLOW, COLOR_CYAN, COLOR_WHITE
				};

				for (int i = 0; i < MAX_SLOTS; i++) {
					if (g_slots[i].active) {
						unsigned short c = colors[i % 6];
						fb_draw_cross(&fb,
							      g_slots[i].x,
							      g_slots[i].y, c);
					}
				}
			}
		} else if (ev.type != EV_SYN) {
			printf("[%s] code=%d value=%d\n",
			       event_type_str(ev.type), ev.code, ev.value);
		}
	}

	printf("\n--- Stopped. Total events: %lu ---\n", event_count);

	close(evfd);
	if (use_fb)
		fb_close(&fb);

	return 0;
}
