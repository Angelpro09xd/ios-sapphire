/* camd kernel adapter, see camk.h */
#include "camk.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>

CamCaps camk;
char camk_log[2048];

#define PIECE (1u << 20) /* the driver hands out at most 1 MB per read, and wants room for all of it */

static bool exists(const char *name)
{
	int mib[CTL_MAXNAME];
	size_t n = CTL_MAXNAME;
	return sysctlnametomib(name, mib, &n) == 0;
}

void camk_detect(void)
{
	camk.present = exists("kern.sweet_cam");
	camk.ring = exists("kern.sweet_cam_latest") && exists("kern.sweet_cam_sel");
	camk.kexp = exists("kern.sweet_cam_exp");
	camk.i2c = exists("kern.sweet_cam_i2c");
}

static int set_int(const char *name, int v)
{
	return sysctlbyname(name, NULL, NULL, &v, sizeof v) == 0 ? 0 : errno;
}

static void grab_log(void)
{
	size_t n = 0;
	camk_log[0] = 0;
	if (sysctlbyname("kern.sweet_cam_log", NULL, &n, NULL, 0) != 0 || n == 0) return;
	char *b = malloc(n + 1);
	if (!b) return;
	if (sysctlbyname("kern.sweet_cam_log", b, &n, NULL, 0) == 0) {
		b[n < 1 ? 0 : n - 1] = 0;
		size_t l = strlen(b);
		const char *tail = l >= sizeof camk_log ? b + l - (sizeof camk_log - 1) : b; /* the end matters */
		snprintf(camk_log, sizeof camk_log, "%s", tail);
	}
	free(b);
}

int camk_op(int op, int arg)
{
	int r = 0;
	if (arg >= 0) r = set_int("kern.sweet_cam_arg", arg);
	if (r == 0) r = set_int("kern.sweet_cam", op);
	if (r != 0) grab_log();
	return r;
}

long camk_frame_len(void)
{
	uint64_t v = 0;
	size_t l = sizeof v;
	if (sysctlbyname("kern.sweet_cam_frame_len", &v, &l, NULL, 0) != 0) return -1;
	return l == 4 ? (long)*(uint32_t *)&v : (long)v;
}

int camk_select(int sel)
{
	return set_int("kern.sweet_cam_sel", sel);
}

static int seek0(void)
{
	int64_t z = 0;
	return sysctlbyname("kern.sweet_cam_seek", NULL, NULL, &z, sizeof z) == 0 ? 0 : errno;
}

int camk_read(uint8_t *dst, size_t len)
{
	int r = seek0();
	if (r) return r;
	size_t got = 0;
	while (got < len) {
		size_t want = len - got < PIECE ? len - got : PIECE, n = want;
		if (sysctlbyname("kern.sweet_cam_frame", dst + got, &n, NULL, 0) != 0) return errno;
		if (n == 0) return EIO;
		got += n;
	}
	return 0;
}

int camk_read_to_fd(int fd, size_t len, camk_progress pr, void *ctx)
{
	int r = seek0();
	if (r) return r;
	uint8_t *b = malloc(PIECE);
	if (!b) return ENOMEM;
	size_t got = 0;
	while (got < len) {
		size_t want = len - got < PIECE ? len - got : PIECE, n = want;
		if (sysctlbyname("kern.sweet_cam_frame", b, &n, NULL, 0) != 0) {
			r = errno;
			break;
		}
		if (n == 0) {
			r = EIO;
			break;
		}
		for (size_t w = 0; w < n;) {
			ssize_t k = write(fd, b + w, n - w);
			if (k <= 0) {
				r = k < 0 ? errno : EIO;
				break;
			}
			w += (size_t)k;
		}
		if (r) break;
		got += n;
		if (pr) pr(got, len, ctx);
	}
	free(b);
	return r;
}

static int read_str(const char *name, char *buf, size_t cap)
{
	size_t n = cap - 1;
	if (sysctlbyname(name, buf, &n, NULL, 0) != 0) return errno;
	buf[n < cap ? n : cap - 1] = 0;
	return 0;
}

/* the number after "key " in s (0 if absent); hex with 0x */
static unsigned long long field(const char *s, const char *key)
{
	char k[32];
	snprintf(k, sizeof k, "%s ", key);
	for (const char *p = s; (p = strstr(p, k)) != NULL; p++) {
		if (p != s && p[-1] != ' ' && p[-1] != '(') continue;
		return strtoull(p + strlen(k), NULL, 0);
	}
	return 0;
}

int camk_latest(CamLatest *l)
{
	char b[256];
	int r = read_str("kern.sweet_cam_latest", b, sizeof b);
	if (r) return r;
	memset(l, 0, sizeof *l);
	l->running = field(b, "running") != 0;
	l->seq = field(b, "seq");
	const char *p = strstr(b, " buf ");
	l->buf = p ? atoi(p + 5) : -1;
	l->bytes = (uint32_t)field(b, "bytes");
	l->w = (uint32_t)field(b, "w");
	l->h = (uint32_t)field(b, "h");
	l->mode = (uint32_t)field(b, "mode");
	l->t_ns = field(b, "t_ns");
	l->ovf = field(b, "ovf");
	l->fault = (uint32_t)field(b, "fault");
	return 0;
}

/* "mode %u %ux%u fll %u llp %u line_ns %u coarse %u (min 12 max %u) gain_x1000 %u (1000-64000) fll_set %u set %u streaming %u" */
int camk_exp_read(CamExp *e)
{
	char b[300];
	int r = read_str("kern.sweet_cam_exp", b, sizeof b);
	if (r) return r;
	memset(e, 0, sizeof *e);
	e->mode = (unsigned)field(b, "mode");
	const char *p = strstr(b, "mode ");
	if (p) {
		p = strchr(p + 5, ' ');
		if (p) sscanf(p, " %ux%u", &e->w, &e->h);
	}
	e->fll = (unsigned)field(b, "fll");
	e->line_ns = (unsigned)field(b, "line_ns");
	e->coarse = (unsigned)field(b, "coarse");
	e->max_lines = (unsigned)field(b, "max");
	e->gain_x1000 = (unsigned)field(b, "gain_x1000");
	e->streaming = field(b, "streaming") != 0;
	return 0;
}

int camk_exp_write(unsigned lines, unsigned gain_x1000)
{
	char b[64];
	int n = snprintf(b, sizeof b, "%u %u", lines, gain_x1000);
	return sysctlbyname("kern.sweet_cam_exp", NULL, NULL, b, (size_t)n + 1) == 0 ? 0 : errno;
}

static int i2c(unsigned reg, unsigned val)
{
	char b[48];
	int n = snprintf(b, sizeof b, "0 10 %x %x", reg, val);
	return sysctlbyname("kern.sweet_cam_i2c", NULL, NULL, b, (size_t)n + 1) == 0 ? 0 : errno;
}

int camk_i2c_exposure(unsigned lines, unsigned gain_x1000)
{
	/* the driver writes values <= 0xff as one byte: split every 16-bit register into two byte writes */
	unsigned code = gain_x1000 * 32 / 1000;
	code = code < 0x20 ? 0x20 : code > 0x800 ? 0x800 : code;
	if (lines < 12) lines = 12;
	int r = i2c(0xFCFC, 0x4000);         /* page 0x4000 (as the 0276 driver does) */
	r = r ? r : i2c(0x0104, 0x01);       /* group hold */
	r = r ? r : i2c(0x0202, lines >> 8);
	r = r ? r : i2c(0x0203, lines & 0xff);
	r = r ? r : i2c(0x0204, code >> 8);
	r = r ? r : i2c(0x0205, code & 0xff);
	int h = i2c(0x0104, 0x00);           /* release, even after a failure */
	return r ? r : h;
}
