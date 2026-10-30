/*
 * camd: the main camera (S5KHM6 through kern.sweet_cam) for apps. The only process that runs
 * the camera ops; apps talk to it over /var/run/camd.sock and read the live preview from shared
 * memory (protocol and layout: camd_proto.h).
 *
 *   preview   kernel 0276: op 30 ring (mode 3 1280x720 by default), newest frame read through
 *             kern.sweet_cam_sel -1; older kernels: the sensor streams mode 1 and op 23 grabs one
 *             frame at a time (slower). Each frame -> BGRX (preview.c), upright, into the shm.
 *   AE / AWB  metered mean green (centre-weighted or a spot) to ~16% with highlight protection,
 *             50 Hz anti-banding, through kern.sweet_cam_exp (0276) or the sensor registers over
 *             kern.sweet_cam_i2c; gray-world white balance with clamps. Both feed the stills.
 *   stills    108 MP: op 32 (0276) or stream mode 0 + op 23; 12 MP: one mode 1 frame. The raw
 *             goes to /var/tmp, a forked child develops it with the pipeline library (develop.c)
 *             into Files' "On My iPhone"/Camara/IMG_YYYYMMDD_HHMMSS.jpg (+ .dng), Exif with the
 *             orientation; a small thumbnail from the preview goes to Camara/.thumbs/.
 *   power     ops 1, 2 on demand; op 9 after 30 s without preview or capture.
 * Log: /var/tmp/camd.log (256 KB, then .1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <dispatch/dispatch.h>

#include "camd_proto.h"
#include "camk.h"
#include "preview.h"
#include "develop.h"
#include "exif.h"
#include "../out.h"

#define LOG_PATH   "/var/tmp/camd.log"
#define CONF_PATH  "/var/root/camd.conf"
#define RAW_PATH   "/var/tmp/camd-raw.bin"
#define IDLE_OFF_S 30
#define MOBILE_UID 501

/* ================================================================== log */
static void
logf_(const char *fmt, ...)
{
	char msg[1200], line[1300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	struct stat st;
	if (stat(LOG_PATH, &st) == 0 && st.st_size > 256 * 1024) rename(LOG_PATH, LOG_PATH ".1");
	int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) return;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	struct tm tm;
	localtime_r(&tv.tv_sec, &tm);
	int n = snprintf(line, sizeof line, "%02d-%02d %02d:%02d:%02d.%03d %s\n", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
	    tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), msg);
	if (n > 0) (void)!write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
	close(fd);
}
#define LOG(...) logf_(__VA_ARGS__)

static double
now_s(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) mach_timebase_info(&tb);
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

/* ================================================================== sensor modes */
typedef struct {
	int w, h, cell;
	double line_us;   /* used when the kernel does not report it */
	unsigned fll;
	double fps;
} ModeInfo;

static const ModeInfo MODES[5] = {
	[0] = { 12000, 9000, 3, 1e6 / (7.04 * 9248), 9248, 7.04 },
	[1] = { 4000, 3000, 1, 1e6 / (30.04 * 4296), 4296, 30.04 },
	[3] = { 1280, 720, 1, 0, 0, 120 },
	[4] = { 4000, 3000, 3, 1e6 / (30.0 * 3234), 3234, 30.0 },
};
#define BLACK 64
#define WHITE 1023

/* ================================================================== configuration */
static struct {
	int dng, rotate, quality, preview_mode; /* preview_mode 0: per kernel (3 with the ring, else 1) */
	double cal[5][5];                         /* still brightness / preview brightness, [still][preview] */
} conf = { 0, 90, 92, 0, { { 0 } } };

static void
conf_defaults_cal(void)
{
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 5; j++) conf.cal[i][j] = 1.0;
}

static void
conf_load(void)
{
	conf_defaults_cal();
	FILE *f = fopen(CONF_PATH, "r");
	if (!f) return;
	char k[64];
	double v;
	int a, b;
	char line[160];
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "cal_%d_%d=%lf", &a, &b, &v) == 3) {
			if (a >= 0 && a < 5 && b >= 0 && b < 5 && v > 0.05 && v < 20) conf.cal[a][b] = v;
		} else if (sscanf(line, "%63[a-z_]=%lf", k, &v) == 2) {
			if (!strcmp(k, "dng")) conf.dng = v != 0;
			else if (!strcmp(k, "rotate")) conf.rotate = (int)v;
			else if (!strcmp(k, "quality")) conf.quality = (int)v;
			else if (!strcmp(k, "preview_mode")) conf.preview_mode = (int)v;
		}
	}
	fclose(f);
	if (conf.rotate != 0 && conf.rotate != 90 && conf.rotate != 180 && conf.rotate != 270) conf.rotate = 90;
	if (conf.quality < 50 || conf.quality > 100) conf.quality = 92;
}

static void
conf_save(void)
{
	FILE *f = fopen(CONF_PATH ".tmp", "w");
	if (!f) return;
	fprintf(f, "dng=%d\nrotate=%d\nquality=%d\npreview_mode=%d\n", conf.dng, conf.rotate, conf.quality, conf.preview_mode);
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 5; j++)
			if (conf.cal[i][j] != 1.0) fprintf(f, "cal_%d_%d=%.4f\n", i, j, conf.cal[i][j]);
	fclose(f);
	rename(CONF_PATH ".tmp", CONF_PATH);
}

/* ================================================================== clients */
struct client {
	int fd;
	dispatch_source_t rs;
	char buf[1024];
	size_t n;
	bool gone, wants_preview;
	struct client *next;
};
static struct client *clients;
static dispatch_queue_t q;

static void
send_line(struct client *c, const char *fmt, ...)
{
	if (c == NULL || c->gone) return;
	char b[1200];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
	b[n++] = '\n';
	/* the socket is non-blocking; replies are short, wait a little if the buffer is full */
	for (int off = 0, tries = 0; off < n && tries < 200;) {
		ssize_t k = write(c->fd, b + off, (size_t)(n - off));
		if (k > 0) off += (int)k;
		else if (k < 0 && (errno == EAGAIN || errno == EINTR)) {
			usleep(5000);
			tries++;
		} else break;
	}
}

/* ================================================================== state */
static bool powered;
static int stream_cur = -1;      /* frames path: the mode the sensor streams with WM7 set up */
static bool ring_on;             /* ring path: op 30 running */
static int ring_mode = -1;
static bool pv_want;             /* a client wants the preview */
static int pv_mode;              /* its sensor mode */
static unsigned pv_gen;
static uint64_t pv_seq, ring_last_seq;
static double last_activity;
static bool cap_busy;            /* a still is being taken or developed */
static bool sensor_busy;         /* the camera ops are in use by a still */
static uint8_t *frame;
static size_t frame_cap;
static CamdShm *shm;
static int notify_tok = -1;

/* exposure */
static struct {
	bool manual;
	double E;            /* wanted exposure (us x gain) */
	double t_us, gain;   /* last applied */
	unsigned lines, gain_x1000, fll;
	double line_us;      /* of the mode streaming now */
	unsigned def_max_lines, def_fll;
	int mode;
	float ev;
	float mx, my;        /* spot in raw coordinates, < 0: centre-weighted */
	int skip;
	double gplain, gmean, E_at_stats;
	bool have_stats;
	bool wb_manual;
	float wb[3];
} ae = { .E = 8000, .gain = 1, .ev = 0, .mx = -1, .my = -1, .wb = { 1.9f, 1.0f, 1.6f } };

static int
default_preview_mode(void)
{
	if (conf.preview_mode == 1 || conf.preview_mode == 4) return conf.preview_mode;
	if (conf.preview_mode == 3 && camk.ring) return 3;
	return camk.ring ? 3 : 1;
}

/* ================================================================== camera ops */
static int
op(int o, int arg, const char *what)
{
	double t0 = now_s();
	int r = camk_op(o, arg);
	double dt = now_s() - t0;
	if (r != 0) {
		LOG("op %d (%s, arg %#x) failed: %s (%.0f ms); kernel log: %s", o, what, arg < 0 ? 0 : arg, strerror(r), dt * 1000,
		    camk_log);
	} else if (dt > 0.5) {
		LOG("op %d (%s) took %.0f ms", o, what, dt * 1000);
	}
	return r;
}

static int
power_on(void)
{
	if (powered) return 0;
	int r = op(1, -1, "camss on");
	r = r ? r : op(2, -1, "main sensor power");
	if (r == 0) {
		powered = true;
		LOG("camera powered");
	}
	return r;
}

static void
power_off(const char *why)
{
	if (!powered) return;
	if (ring_on) op(31, -1, "preview off");
	op(9, -1, "all off");
	powered = false;
	ring_on = false;
	ring_mode = -1;
	stream_cur = -1;
	LOG("camera off (%s)", why);
}

/* frames path: the sensor streaming mode m with the capture path configured */
static int
stream_mode(int m)
{
	if (stream_cur == m) return 0;
	if (stream_cur >= 0) op(13, -1, "stop");
	stream_cur = -1;
	int arg = m << 4;
	int r = op(10, arg, "sensor init + mode");
	r = r ? r : op(11, arg, "CSIPHY/CSID");
	r = r ? r : op(20, arg, "SMMU");
	r = r ? r : op(21, arg, "TFE");
	r = r ? r : op(22, arg, "WM7");
	if (r == 0) stream_cur = m;
	return r;
}

/* ================================================================== exposure */
static void
exp_refresh_mode(int m)
{
	ae.mode = m;
	ae.line_us = MODES[m].line_us;
	ae.def_fll = MODES[m].fll;
	ae.def_max_lines = MODES[m].fll > 16 ? MODES[m].fll - 16 : 12;
	if (camk.kexp) {
		CamExp e;
		if (camk_exp_read(&e) == 0 && e.line_ns > 0) {
			ae.line_us = e.line_ns / 1000.0;
			ae.def_fll = e.fll;
			ae.def_max_lines = e.fll > 16 ? e.fll - 16 : 12;
		}
	}
	if (ae.line_us <= 0) ae.line_us = 10;
	ae.lines = 0; /* force the next write */
}

#define GAIN_MAX 32.0
#define T_MAX_PREVIEW 33333.0

/* split E into time and gain for the current mode, anti-banding (50 Hz) above 10 ms, and write it */
static void
exp_apply(double t_max)
{
	unsigned lines, gx;
	double t, g;
	if (ae.manual) {
		t = ae.t_us;
		g = ae.gain;
	} else {
		double E = ae.E;
		t = E < t_max ? E : t_max;
		if (t >= 10000) t = floor(t / 10000) * 10000;
		g = E / t;
		if (g < 1) g = 1;
		if (g > GAIN_MAX) g = GAIN_MAX;
	}
	lines = (unsigned)(t / ae.line_us + 0.5);
	if (lines < 12) lines = 12;
	unsigned fll = 0;
	if (lines > ae.def_max_lines) {
		if (camk.kexp) fll = lines + 16;  /* longer frame (lower fps) for the longer exposure */
		else lines = ae.def_max_lines;
	}
	gx = (unsigned)(g * 1000 + 0.5);
	if (gx < 1000) gx = 1000;
	if (gx > 64000) gx = 64000;
	if (lines == ae.lines && gx == ae.gain_x1000 && fll == ae.fll) return;
	int r;
	if (camk.kexp) {
		char b[64];
		int n = fll ? snprintf(b, sizeof b, "%u %u %u", lines, gx, fll) : snprintf(b, sizeof b, "%u %u", lines, gx);
		r = sysctlbyname("kern.sweet_cam_exp", NULL, NULL, b, (size_t)n + 1) == 0 ? 0 : errno;
	} else if (camk.i2c) {
		r = camk_i2c_exposure(lines, gx);
	} else {
		r = ENOTSUP;
	}
	if (r != 0) {
		static int errs;
		if (errs++ < 20) LOG("exposure %u lines gain %.2f: %s", lines, gx / 1000.0, strerror(r));
		return;
	}
	ae.lines = lines;
	ae.gain_x1000 = gx;
	ae.fll = fll;
	ae.t_us = lines * ae.line_us;
	ae.gain = gx / 1000.0;
}

/* auto exposure + white balance from one preview frame's statistics */
static void
ae_update(const PvStats *st)
{
	double Eapplied = ae.t_us * ae.gain;
	if (ae.skip > 0) {
		ae.skip--;
		return;
	}
	ae.gplain = st->gplain;
	ae.gmean = st->gmean;
	ae.E_at_stats = Eapplied;
	ae.have_stats = true;
	if (!ae.wb_manual && st->n > 200 && st->mean[0] > 1e-4 && st->mean[2] > 1e-4) {
		double gr = st->mean[1] / st->mean[0], gb = st->mean[1] / st->mean[2];
		gr = gr < 0.8 ? 0.8 : gr > 3.5 ? 3.5 : gr;
		gb = gb < 0.8 ? 0.8 : gb > 3.5 ? 3.5 : gb;
		ae.wb[0] = (float)(0.85 * ae.wb[0] + 0.15 * gr);
		ae.wb[2] = (float)(0.85 * ae.wb[2] + 0.15 * gb);
		ae.wb[1] = 1;
	}
	if (ae.manual || Eapplied <= 0) return;
	double target = 0.16 * pow(2, ae.ev);
	double m = st->gmean > 1e-4 ? st->gmean : 1e-4;
	double ratio = target / m;
	if (st->gp98 * ratio > 0.95) {          /* keep the highlights, up to 1 EV below the target */
		double hr = 0.95 / st->gp98;
		ratio = hr > ratio * 0.5 ? (hr < ratio ? hr : ratio) : ratio * 0.5;
	}
	ratio = ratio < 0.125 ? 0.125 : ratio > 8 ? 8 : ratio;
	if (fabs(log2(ratio)) < 0.1) return;
	double E = Eapplied * pow(ratio, 0.65);
	double Emax = T_MAX_PREVIEW * GAIN_MAX, Emin = 12 * ae.line_us;
	ae.E = E < Emin ? Emin : E > Emax ? Emax : E;
	exp_apply(T_MAX_PREVIEW);
	ae.skip = camk.ring ? 2 : 1;
}

/* ================================================================== shared memory */
static int
shm_open_file(void)
{
	size_t size = CAMD_SHM_HEADER + (size_t)CAMD_SHM_SLOTS * CAMD_SHM_SLOT_SIZE;
	int fd = open(CAMD_SHM_PATH, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		LOG("%s: %s", CAMD_SHM_PATH, strerror(errno));
		return -1;
	}
	fchmod(fd, 0644);
	if (ftruncate(fd, (off_t)size) != 0) {
		LOG("%s: ftruncate: %s", CAMD_SHM_PATH, strerror(errno));
		close(fd);
		return -1;
	}
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED) {
		LOG("%s: mmap: %s", CAMD_SHM_PATH, strerror(errno));
		return -1;
	}
	shm = p;
	memset(shm, 0, sizeof *shm);
	shm->magic = CAMD_SHM_MAGIC;
	shm->version = CAMD_SHM_VERSION;
	shm->header_size = CAMD_SHM_HEADER;
	shm->slot_size = CAMD_SHM_SLOT_SIZE;
	shm->nslots = CAMD_SHM_SLOTS;
	return 0;
}

static uint8_t *
shm_slot(unsigned i)
{
	return (uint8_t *)shm + CAMD_SHM_HEADER + (size_t)i * CAMD_SHM_SLOT_SIZE;
}

/* ================================================================== preview */
typedef struct {
	const uint8_t *raw;
	PvRaw r;
	PvParams p;
	uint8_t *dst;
	long dst_stride;
	int rows, chunks;
	PvStats st[16];
} RenderJob;

static bool
render_frame(const uint8_t *raw, int mode, uint64_t ts_ns, PvStats *out)
{
	const ModeInfo *mi = &MODES[mode];
	static RenderJob j;
	memset(&j, 0, sizeof j);
	j.raw = raw;
	j.r = (PvRaw){ mi->w, mi->h, (long)mi->w * 5 / 4, mi->cell, BLACK, WHITE };
	j.p.wb[0] = ae.wb[0];
	j.p.wb[1] = ae.wb[1];
	j.p.wb[2] = ae.wb[2];
	j.p.gain = 1;
	j.p.rotate = conf.rotate;
	j.p.max_out = CAMD_SHM_MAXDIM;
	if (mode != 3) j.p.max_out = 1000;
	j.p.meter_x = ae.mx;
	j.p.meter_y = ae.my;
	int B, S, ow, oh;
	pv_geometry(&j.r, &j.p, &B, &S, &ow, &oh);
	if (ow > CAMD_SHM_MAXDIM || oh > CAMD_SHM_MAXDIM) return false;
	unsigned slot = (shm->latest + 1) % CAMD_SHM_SLOTS;
	j.dst = shm_slot(slot);
	j.dst_stride = ((long)ow * 4 + 63) & ~63L;
	j.rows = (j.r.h - B) / S + 1;
	j.chunks = 16;
	dispatch_apply((size_t)j.chunks, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t k) {
		int a = (int)((long)j.rows * (long)k / j.chunks), b = (int)((long)j.rows * (long)(k + 1) / j.chunks);
		pv_render_rows(j.raw, &j.r, &j.p, a, b, j.dst, j.dst_stride, &j.st[k]);
	});
	PvStats st;
	memset(&st, 0, sizeof st);
	for (int k = 0; k < j.chunks; k++) pv_stats_merge(&st, &j.st[k]);
	pv_stats_finish(&st);
	CamdSlot *s = &shm->slot[slot];
	s->seq = ++pv_seq;
	s->ts_ns = ts_ns;
	s->width = (uint32_t)ow;
	s->height = (uint32_t)oh;
	s->stride = (uint32_t)j.dst_stride;
	s->mode = (uint32_t)mode;
	s->exposure_us = (float)ae.t_us;
	s->gain = (float)ae.gain;
	memcpy(s->wb, ae.wb, sizeof s->wb);
	s->luma = (float)st.gmean;
	__atomic_store_n(&shm->latest, slot, __ATOMIC_RELEASE);
	__atomic_store_n(&shm->seq, pv_seq, __ATOMIC_RELEASE);
	shm->running = 1;
	if (notify_tok >= 0) notify_set_state(notify_tok, pv_seq);
	notify_post(CAMD_NOTIFY_FRAME);
	*out = st;
	return true;
}

static bool
frame_buf(size_t len)
{
	if (frame_cap >= len) return true;
	free(frame);
	frame = malloc(len);
	frame_cap = frame ? len : 0;
	return frame != NULL;
}

static int preview_start_hw(int mode);
static void pv_schedule(double delay_s);

static int pv_errors;

/* one preview frame (on q) */
static void
pv_step(unsigned gen)
{
	if (gen != pv_gen || !pv_want || sensor_busy) return;
	double t0 = now_s();
	int mode = pv_mode, r = 0;
	size_t len = 0;
	uint64_t ts = 0;
	if (camk.ring) {
		if (!ring_on || ring_mode != mode) r = preview_start_hw(mode);
		CamLatest l;
		r = r ? r : camk_latest(&l);
		if (r == 0 && (!l.running || l.fault)) {
			LOG("preview ring stopped (running %d fault %#x ovf %llu): restarting", l.running, l.fault,
			    (unsigned long long)l.ovf);
			ring_on = false;
			r = EIO;
		}
		if (r == 0 && l.seq == ring_last_seq) {
			pv_schedule(0.004);
			return;
		}
		if (r == 0) {
			len = l.bytes;
			r = frame_buf(len) ? 0 : ENOMEM;
			r = r ? r : camk_select(-1);
			r = r ? r : camk_read(frame, len);
			CamLatest l2;
			if (r == 0 && camk_latest(&l2) == 0 && l2.seq >= l.seq + 3) {
				/* the buffer may have been rewritten while being read: drop it */
				ring_last_seq = l.seq;
				pv_schedule(0);
				return;
			}
			ring_last_seq = l.seq;
			ts = l.t_ns;
		}
	} else {
		r = stream_cur == mode ? 0 : preview_start_hw(mode);
		r = r ? r : op(23, -1, "frame");
		if (r == 0) {
			long fl = camk_frame_len();
			len = fl > 0 ? (size_t)fl : 0;
			r = len ? 0 : EIO;
		}
		r = r ? r : (frame_buf(len) ? 0 : ENOMEM);
		r = r ? r : camk_read(frame, len);
		ts = (uint64_t)(now_s() * 1e9);
	}
	const ModeInfo *mi = &MODES[mode];
	if (r == 0 && len < (size_t)mi->w * mi->h * 5 / 4) {
		LOG("preview frame of %zu bytes, %dx%d needs %d", len, mi->w, mi->h, mi->w * mi->h * 5 / 4);
		r = EIO;
	}
	if (r != 0) {
		if (++pv_errors > 5) {
			LOG("preview: %d errors in a row, power cycling", pv_errors);
			power_off("preview errors");
			pv_errors = 0;
			pv_schedule(1.0);
		} else {
			pv_schedule(0.2);
		}
		return;
	}
	pv_errors = 0;
	PvStats st;
	if (render_frame(frame, mode, ts, &st)) ae_update(&st);
	last_activity = now_s();
	double dt = now_s() - t0, period = 1.0 / 30;
	pv_schedule(dt < period ? period - dt : 0);
}

static void
pv_schedule(double delay_s)
{
	unsigned gen = pv_gen;
	if (delay_s <= 0) dispatch_async(q, ^{ pv_step(gen); });
	else dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay_s * 1e9)), q, ^{ pv_step(gen); });
}

static int
preview_start_hw(int mode)
{
	int r = power_on();
	if (r) return r;
	if (camk.ring) {
		r = op(30, mode << 4, "preview ring");
		ring_on = r == 0;
		ring_mode = r == 0 ? mode : -1;
		ring_last_seq = 0;
	} else {
		r = stream_mode(mode);
	}
	if (r == 0) {
		exp_refresh_mode(mode);
		exp_apply(T_MAX_PREVIEW);
		ae.skip = 2;
		LOG("preview mode %d running (%s), line %.2f us, max %u lines", mode, camk.ring ? "ring" : "single frames", ae.line_us,
		    ae.def_max_lines);
	}
	return r;
}

static void
preview_stop_hw(void)
{
	if (ring_on) op(31, -1, "preview off");
	ring_on = false;
	ring_mode = -1;
	if (stream_cur >= 0) op(13, -1, "stop");
	stream_cur = -1;
	if (shm) shm->running = 0;
}

static void
update_preview_want(void)
{
	bool want = false;
	for (struct client *c = clients; c; c = c->next)
		if (!c->gone && c->wants_preview) want = true;
	if (want == pv_want) return;
	pv_want = want;
	pv_gen++;
	if (want) {
		pv_schedule(0);
	} else if (!sensor_busy) {
		preview_stop_hw();
		LOG("preview stopped");
	}
	last_activity = now_s();
}

/* ================================================================== files */
static char files_dir[1024]; /* .../File Provider Storage/Camara */

static bool
find_files_dir(void)
{
	const char *base = "/var/mobile/Containers/Shared/AppGroup";
	DIR *d = opendir(base);
	if (!d) return false;
	struct dirent *e;
	bool found = false;
	while (!found && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char p[1024];
		struct stat st;
		snprintf(p, sizeof p, "%s/%s/File Provider Storage/Downloads", base, e->d_name);
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(files_dir, sizeof files_dir, "%s/%s/File Provider Storage/Camara", base, e->d_name);
			found = true;
		}
	}
	closedir(d);
	if (!found) {
		snprintf(files_dir, sizeof files_dir, "/var/mobile/Media/Camara");
	}
	char thumbs[1100];
	mkdir(files_dir, 0755);
	chown(files_dir, MOBILE_UID, MOBILE_UID);
	snprintf(thumbs, sizeof thumbs, "%s/.thumbs", files_dir);
	mkdir(thumbs, 0755);
	chown(thumbs, MOBILE_UID, MOBILE_UID);
	return found;
}

/* IMG_YYYYMMDD_HHMMSS[_n]: the base path (no extension) of a name not taken yet */
static void
new_photo_base(char *out, size_t cap, struct tm *tm)
{
	char stamp[32];
	strftime(stamp, sizeof stamp, "IMG_%Y%m%d_%H%M%S", tm);
	for (int n = 0; n < 100; n++) {
		char j[1200];
		if (n == 0) snprintf(out, cap, "%s/%s", files_dir, stamp);
		else snprintf(out, cap, "%s/%s_%d", files_dir, stamp, n);
		snprintf(j, sizeof j, "%s.jpg", out);
		if (access(j, F_OK) != 0) return;
	}
}

/* the newest preview frame, halved, as a JPEG (the gallery's thumbnail) */
static void
write_thumb(const char *path)
{
	if (!shm || shm->seq == 0) return;
	const CamdSlot *s = &shm->slot[shm->latest];
	const uint8_t *src = shm_slot(shm->latest);
	int w = (int)s->width / 2, h = (int)s->height / 2;
	if (w < 16 || h < 16) return;
	FILE *f = fopen(path, "wb");
	if (!f) return;
	uint8_t *rgb = malloc((size_t)w * 3 * 16);
	JpegEnc *e = jpeg_new(w, h, 85);
	const uint8_t *keep = g_jpeg_app1;
	g_jpeg_app1 = NULL;
	jpeg_write_header(e, f);
	g_jpeg_app1 = keep;
	int my = jpeg_mcu_rows(e);
	for (int m = 0; m < my; m++) {
		const uint8_t *rows[16];
		for (int k = 0; k < 16; k++) {
			int y = m * 16 + k;
			if (y >= h) y = h - 1;
			uint8_t *d = rgb + (size_t)w * 3 * k;
			const uint8_t *a = src + (size_t)s->stride * (2 * y), *b = a + s->stride;
			for (int x = 0; x < w; x++) {
				const uint8_t *p = a + 8 * x, *q2 = b + 8 * x;
				d[3 * x] = (uint8_t)((p[2] + p[6] + q2[2] + q2[6] + 2) / 4);
				d[3 * x + 1] = (uint8_t)((p[1] + p[5] + q2[1] + q2[5] + 2) / 4);
				d[3 * x + 2] = (uint8_t)((p[0] + p[4] + q2[0] + q2[4] + 2) / 4);
			}
			rows[k] = d;
		}
		size_t len;
		uint8_t *buf = jpeg_encode_mcu_row(e, rows, &len);
		jpeg_write_mcu_row(e, f, m, buf, len);
		free(buf);
	}
	jpeg_finish(e, f);
	fclose(f);
	free(rgb);
	free(e);
	chown(path, MOBILE_UID, MOBILE_UID);
}

/* ================================================================== stills */
typedef struct {
	struct client *who;
	int mp;              /* 108 or 12 */
	int mode;            /* sensor mode of the raw */
	int pv_mode;
	bool dng;
	char base[1100];
	char jpeg[1200], dngp[1200], thumb[1200];
	double t0;
	double E_still, E_preview, gplain;
	double cal_g;        /* the still's measured mean green */
	pid_t pid;
	int pfd;
	char pbuf[256];
	size_t pn;
	dispatch_source_t rs, ps;
	uint8_t app1[1024];
	size_t app1_len;
	float wb[3];
	float expect_g;
	struct tm tm;
} Still;
static Still *cur_still;

static void
still_reply(Still *s, const char *fmt, ...)
{
	if (!s->who || s->who->gone) return;
	char b[1200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	send_line(s->who, "%s", b);
}

static void
dump_progress(size_t done, size_t total, void *ctx)
{
	Still *s = ctx;
	static int last = -1;
	if (done <= (1u << 20)) last = -1;
	int p = (int)(done * 18 / (total ? total : 1)) + 1;
	if (p != last && s->mp == 108) still_reply(s, "PROGRESS %d", p);
	last = p;
}

static void still_finish(Still *s, int status);

static void
still_child_line(Still *s, char *line)
{
	int p;
	double g;
	if (sscanf(line, "P %d", &p) == 1) {
		int base = s->mp == 108 ? 20 : 10;
		still_reply(s, "PROGRESS %d", base + p * (99 - base) / 100);
	} else if (sscanf(line, "CAL %lf", &g) == 1) {
		s->cal_g = g;
	}
}

static void
still_develop(Still *s)
{
	int pfd[2];
	if (pipe(pfd) != 0) {
		still_reply(s, "ERR pipe: %s", strerror(errno));
		still_finish(s, -1);
		return;
	}
	const ModeInfo *mi = &MODES[s->mode];
	char dt[32];
	strftime(dt, sizeof dt, "%Y:%m:%d %H:%M:%S", &s->tm);
	ExifInfo ei = { conf.rotate == 90 ? 6 : conf.rotate == 180 ? 3 : conf.rotate == 270 ? 8 : 1, dt, "Xiaomi",
		"Redmi Note 13", "sapphire camd 1.0", (unsigned)(ae.t_us + 0.5), (unsigned)(ae.gain * 50 + 0.5),
		(unsigned)mi->w, (unsigned)mi->h };
	s->app1_len = exif_build(s->app1, sizeof s->app1, &ei);
	DevJob j;
	memset(&j, 0, sizeof j);
	j.raw = RAW_PATH;
	j.w = mi->w;
	j.h = mi->h;
	j.cell = mi->cell;
	j.stride = (long)mi->w * 5 / 4;
	j.black = BLACK;
	j.white = WHITE;
	memcpy(j.wb, s->wb, sizeof j.wb);
	j.ev = 0;
	j.expect_g = s->expect_g;
	j.jpeg = s->jpeg;
	j.dng = s->dng ? s->dngp : NULL;
	j.quality = conf.quality;
	j.chunk = s->mp == 108 ? 16 : 32;
	j.threads = 0;
	j.sharpen = s->mp == 12 ? 0.3f : 0;
	j.app1 = s->app1_len ? s->app1 : NULL;
	j.app1_len = s->app1_len;
	j.sensor = "S5KHM6";
	pid_t pid = fork();
	if (pid == 0) {
		/* the child: no dispatch, only the library */
		close(pfd[0]);
		j.progress_fd = pfd[1];
		signal(SIGPIPE, SIG_IGN);
		int r = develop_run(&j);
		_exit(r == 0 ? 0 : 1);
	}
	close(pfd[1]);
	if (pid < 0) {
		close(pfd[0]);
		still_reply(s, "ERR fork: %s", strerror(errno));
		still_finish(s, -1);
		return;
	}
	s->pid = pid;
	s->pfd = pfd[0];
	fcntl(s->pfd, F_SETFL, fcntl(s->pfd, F_GETFL) | O_NONBLOCK);
	s->rs = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)s->pfd, 0, q);
	dispatch_source_set_event_handler(s->rs, ^{
		ssize_t k = read(s->pfd, s->pbuf + s->pn, sizeof s->pbuf - 1 - s->pn);
		if (k <= 0) {
			if (k < 0 && (errno == EAGAIN || errno == EINTR)) return;
			dispatch_source_cancel(s->rs);
			return;
		}
		s->pn += (size_t)k;
		s->pbuf[s->pn] = 0;
		char *start = s->pbuf, *nl;
		while ((nl = strchr(start, '\n')) != NULL) {
			*nl = 0;
			still_child_line(s, start);
			start = nl + 1;
		}
		s->pn = strlen(start);
		memmove(s->pbuf, start, s->pn + 1);
		if (s->pn >= sizeof s->pbuf - 1) s->pn = 0;
	});
	int rfd = s->pfd;
	dispatch_source_set_cancel_handler(s->rs, ^{ close(rfd); });
	dispatch_resume(s->rs);
	s->ps = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, q);
	dispatch_source_set_event_handler(s->ps, ^{
		int st = 0;
		waitpid(s->pid, &st, 0);
		dispatch_source_cancel(s->ps);
		dispatch_release(s->ps);
		s->ps = NULL;
		/* the last progress lines first */
		dispatch_async(q, ^{ still_finish(s, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0)); });
	});
	dispatch_resume(s->ps);
}

static void
still_finish(Still *s, int status)
{
	if (s->rs) {
		dispatch_source_cancel(s->rs);
		dispatch_release(s->rs);
		s->rs = NULL;
	}
	unlink(RAW_PATH);
	double dt = now_s() - s->t0;
	if (status == 0) {
		chown(s->jpeg, MOBILE_UID, MOBILE_UID);
		chmod(s->jpeg, 0644);
		if (s->dng) {
			chown(s->dngp, MOBILE_UID, MOBILE_UID);
			chmod(s->dngp, 0644);
		}
		/* brightness calibration of the still against the preview it was metered on */
		if (s->cal_g > 0.005 && s->cal_g < 0.8 && s->gplain > 0.005 && s->E_preview > 0) {
			double r = s->cal_g / (s->gplain * s->E_still / s->E_preview);
			r = r < 0.05 ? 0.05 : r > 20 ? 20 : r;
			double *c = &conf.cal[s->mode][s->pv_mode];
			*c = sqrt(*c * r);
			conf_save();
			LOG("still mean green %.4f, preview %.4f (exposure %.0f vs %.0f): ratio %.3f, calibration now %.3f", s->cal_g,
			    s->gplain, s->E_still, s->E_preview, r, *c);
		}
		LOG("still %d MP done in %.1f s: %s%s", s->mp, dt, s->jpeg, s->dng ? " + DNG" : "");
		if (s->dng) still_reply(s, "DNG %s", s->dngp);
		still_reply(s, "DONE %s", s->jpeg);
		notify_post(CAMD_NOTIFY_PHOTO);
	} else {
		LOG("still %d MP failed (develop status %d) after %.1f s", s->mp, status, dt);
		unlink(s->jpeg);
		if (s->dng) unlink(s->dngp);
		unlink(s->thumb);
		if (status >= 0) still_reply(s, "ERR develop failed (%d)", status);
	}
	if (cur_still == s) cur_still = NULL;
	cap_busy = false;
	last_activity = now_s();
	free(s);
}

/* the raw of a still into RAW_PATH (on q, the preview paused); 0 or an errno */
static int
still_take(Still *s)
{
	int r = power_on();
	if (r) return r;
	sensor_busy = true;
	pv_gen++;
	size_t len = 0;
	const ModeInfo *mi = &MODES[s->mode];
	s->E_preview = ae.E_at_stats > 0 ? ae.E_at_stats : ae.t_us * ae.gain;
	s->gplain = ae.have_stats ? ae.gplain : 0;
	double cal = conf.cal[s->mode][s->pv_mode];
	if (camk.ring) {
		if (s->mode == 0) {
			r = op(32, 0, "108 MP still");
			ring_on = false;
			ring_mode = -1;
			r = r ? r : camk_select(100);
		} else {
			/* 12 MP: a fresh mode 1 frame from the ring (the exposure time carries over) */
			if (!ring_on || ring_mode != 1) r = op(30, 1 << 4, "preview ring mode 1");
			ring_on = r == 0;
			ring_mode = r == 0 ? 1 : -1;
			CamLatest l0, l;
			r = r ? r : camk_latest(&l0);
			double t0 = now_s();
			while (r == 0) {
				r = camk_latest(&l);
				if (r == 0 && l.seq >= l0.seq + 2) break;
				if (now_s() - t0 > 2) r = ETIMEDOUT;
				else usleep(5000);
			}
			r = r ? r : camk_select(-1);
		}
		s->E_still = ae.t_us * ae.gain;
	} else {
		r = stream_mode(s->mode);
		if (r == 0) {
			exp_refresh_mode(s->mode);
			/* the still's exposure: the preview's, corrected by the calibration, up to 1/15 s */
			double E = (ae.manual ? ae.t_us * ae.gain : ae.E) / cal;
			bool man = ae.manual;
			ae.manual = false;
			double keepE = ae.E;
			ae.E = E;
			exp_apply(66000);
			ae.E = keepE;
			ae.manual = man;
			s->E_still = ae.t_us * ae.gain;
			usleep((useconds_t)(2.2e6 / mi->fps));   /* two frames for the new exposure */
			r = op(23, -1, "still frame");
		}
	}
	if (r == 0) {
		long fl = camk_frame_len();
		len = fl > 0 ? (size_t)fl : 0;
		if (len < (size_t)mi->w * mi->h * 5 / 4) {
			LOG("still: %zu bytes, expected %d", len, mi->w * mi->h * 5 / 4);
			r = EIO;
		}
	}
	if (r == 0) {
		still_reply(s, "SHUTTER");
		int fd = open(RAW_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) r = errno;
		else {
			double t0 = now_s();
			r = camk_read_to_fd(fd, len, dump_progress, s);
			close(fd);
			LOG("still raw %zu bytes read in %.2f s", len, now_s() - t0);
		}
	}
	s->expect_g = (float)(s->gplain > 0 ? s->gplain * (s->E_still / (s->E_preview > 0 ? s->E_preview : s->E_still)) * cal : 0);
	sensor_busy = false;
	/* back to the preview */
	if (camk.ring) {
		ring_on = false;
		ring_mode = -1;
	} else if (stream_cur != s->pv_mode) {
		/* restarted by the next preview step; without a preview, stop streaming */
		if (!pv_want && stream_cur >= 0) {
			op(13, -1, "stop");
			stream_cur = -1;
		}
	}
	if (pv_want) {
		pv_gen++;
		pv_schedule(0);
	} else if (ring_on) {
		preview_stop_hw();
	}
	return r;
}

static void
cmd_capture(struct client *c, char *args)
{
	if (cap_busy) {
		send_line(c, "ERR busy");
		return;
	}
	if (!camk.present) {
		send_line(c, "ERR no camera driver in this kernel");
		return;
	}
	int mp = 108;
	bool dng = conf.dng;
	for (char *t = strtok(args, " "); t; t = strtok(NULL, " ")) {
		if (!strcmp(t, "12")) mp = 12;
		else if (!strcmp(t, "108")) mp = 108;
		else if (!strcmp(t, "dng")) dng = true;
		else if (!strcmp(t, "nodng")) dng = false;
	}
	Still *s = calloc(1, sizeof *s);
	if (!s) {
		send_line(c, "ERR no memory");
		return;
	}
	cap_busy = true;
	cur_still = s;
	s->who = c;
	s->mp = mp;
	s->mode = mp == 108 ? 0 : 1;
	s->pv_mode = pv_want ? pv_mode : default_preview_mode();
	s->dng = dng;
	s->t0 = now_s();
	s->pid = -1;
	memcpy(s->wb, ae.wb, sizeof s->wb);
	time_t now = time(NULL);
	localtime_r(&now, &s->tm);
	find_files_dir();
	new_photo_base(s->base, sizeof s->base, &s->tm);
	snprintf(s->jpeg, sizeof s->jpeg, "%s.jpg", s->base);
	snprintf(s->dngp, sizeof s->dngp, "%s.dng", s->base);
	const char *name = strrchr(s->base, '/');
	snprintf(s->thumb, sizeof s->thumb, "%s/.thumbs/%s.jpg", files_dir, name ? name + 1 : s->base);
	send_line(c, "OK capture %d", mp);
	LOG("still %d MP%s -> %s (preview mode %d, exposure %.0f us x %.2f)", mp, dng ? " + DNG" : "", s->jpeg, s->pv_mode, ae.t_us,
	    ae.gain);
	write_thumb(s->thumb);
	int r = still_take(s);
	if (r != 0) {
		still_reply(s, "ERR capture failed: %s", strerror(r));
		still_finish(s, -1);
		return;
	}
	still_reply(s, "PROGRESS %d", mp == 108 ? 20 : 10);
	still_develop(s);
}

/* ================================================================== commands */
static void
cmd_status(struct client *c)
{
	send_line(c, "STATUS state=%s kernel=%s exp=%s powered=%d preview=%d mode=%d seq=%llu exp_us=%.0f gain=%.2f ae=%s ev=%.1f "
	    "luma=%.3f wb=%.2f,%.2f,%.2f busy=%d dng=%d rotate=%d quality=%d preview_mode=%d dir=%s",
	    cap_busy ? "capture" : pv_want ? "preview" : powered ? "on" : "off",
	    camk.ring ? "ring" : camk.present ? "frames" : "none", camk.kexp ? "sysctl" : camk.i2c ? "i2c" : "none", powered,
	    pv_want, pv_mode, (unsigned long long)pv_seq, ae.t_us, ae.gain, ae.manual ? "manual" : "auto", ae.ev, ae.gmean,
	    ae.wb[0], ae.wb[1], ae.wb[2], cap_busy, conf.dng, conf.rotate, conf.quality, default_preview_mode(), files_dir);
}

/* a point of the portrait preview (0..1) -> raw coordinates */
static void
portrait_to_raw(double px, double py, float *rx, float *ry)
{
	double x, y;
	switch (conf.rotate) {
	case 90: x = py; y = 1 - px; break;
	case 180: x = 1 - px; y = 1 - py; break;
	case 270: x = 1 - py; y = px; break;
	default: x = px; y = py; break;
	}
	*rx = (float)x;
	*ry = (float)y;
}

static void
handle_line(struct client *c, char *line)
{
	size_t l = strlen(line);
	while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
	char *args = strchr(line, ' ');
	if (args) *args++ = 0;
	else args = line + l;
	last_activity = now_s();
	if (!strcmp(line, "HELLO")) {
		send_line(c, "OK camd 1 kernel=%s exp=%s", camk.ring ? "ring" : camk.present ? "frames" : "none",
		    camk.kexp ? "sysctl" : camk.i2c ? "i2c" : "none");
	} else if (!strcmp(line, "STATUS")) {
		cmd_status(c);
	} else if (!strcmp(line, "START")) {
		if (!camk.present) {
			send_line(c, "ERR no camera driver in this kernel");
			return;
		}
		int m = atoi(args);
		if (m != 1 && m != 3 && m != 4) m = default_preview_mode();
		if (m == 3 && !camk.ring) m = 1;
		if (pv_want && m != pv_mode) {
			pv_mode = m;
			pv_gen++;
			pv_schedule(0);
		}
		pv_mode = m;
		c->wants_preview = true;
		update_preview_want();
		PvRaw r = { MODES[m].w, MODES[m].h, MODES[m].w * 5 / 4, MODES[m].cell, BLACK, WHITE };
		PvParams p = { .rotate = conf.rotate, .max_out = m == 3 ? CAMD_SHM_MAXDIM : 1000 };
		int ow, oh;
		pv_geometry(&r, &p, NULL, NULL, &ow, &oh);
		send_line(c, "OK preview %d %d %s", ow, oh, CAMD_SHM_PATH);
	} else if (!strcmp(line, "STOP")) {
		c->wants_preview = false;
		update_preview_want();
		send_line(c, "OK");
	} else if (!strcmp(line, "FRAME")) {
		if (!shm || shm->seq == 0) {
			send_line(c, "ERR no frame yet");
			return;
		}
		const CamdSlot *s = &shm->slot[shm->latest];
		send_line(c, "FRAME %s %llu %u %u %u %llu", CAMD_SHM_PATH, (unsigned long long)s->seq, s->width, s->height, s->stride,
		    (unsigned long long)s->ts_ns);
	} else if (!strcmp(line, "CAPTURE")) {
		cmd_capture(c, args);
	} else if (!strcmp(line, "EXPOSURE")) {
		unsigned lines;
		double g;
		if (!strcmp(args, "auto")) {
			ae.manual = false;
			send_line(c, "OK");
		} else if (sscanf(args, "%u,%lf", &lines, &g) == 2 && lines >= 12 && g >= 1 && g <= 64) {
			ae.manual = true;
			ae.t_us = lines * ae.line_us;
			ae.gain = g;
			if (powered) exp_apply(1e9);
			send_line(c, "OK");
		} else {
			send_line(c, "ERR EXPOSURE auto | lines,gain");
		}
	} else if (!strcmp(line, "EV")) {
		double v = atof(args);
		ae.ev = (float)(v < -3 ? -3 : v > 3 ? 3 : v);
		ae.skip = 0;
		send_line(c, "OK");
	} else if (!strcmp(line, "METER")) {
		double x, y;
		if (sscanf(args, "%lf,%lf", &x, &y) == 2 && x >= 0 && x <= 1 && y >= 0 && y <= 1) {
			portrait_to_raw(x, y, &ae.mx, &ae.my);
			ae.skip = 0;
			send_line(c, "OK");
		} else {
			ae.mx = ae.my = -1;
			send_line(c, "OK");
		}
	} else if (!strcmp(line, "WB")) {
		float r, g, b;
		if (sscanf(args, "%f,%f,%f", &r, &g, &b) == 3 && r > 0 && g > 0 && b > 0) {
			ae.wb_manual = true;
			ae.wb[0] = r / g;
			ae.wb[1] = 1;
			ae.wb[2] = b / g;
		} else {
			ae.wb_manual = false;
		}
		send_line(c, "OK");
	} else if (!strcmp(line, "SET")) {
		char k[32];
		int v;
		if (sscanf(args, "%31s %d", k, &v) != 2) {
			send_line(c, "ERR SET key value");
			return;
		}
		if (!strcmp(k, "dng")) conf.dng = v != 0;
		else if (!strcmp(k, "rotate") && (v == 0 || v == 90 || v == 180 || v == 270)) conf.rotate = v;
		else if (!strcmp(k, "quality") && v >= 50 && v <= 100) conf.quality = v;
		else if (!strcmp(k, "preview_mode") && (v == 0 || v == 1 || v == 3 || v == 4)) conf.preview_mode = v;
		else {
			send_line(c, "ERR unknown key or value");
			return;
		}
		conf_save();
		send_line(c, "OK");
	} else if (line[0] != 0) {
		send_line(c, "ERR unknown command");
	}
}

static void
client_gone(struct client *c)
{
	if (c->gone) return;
	c->gone = true;
	if (cur_still && cur_still->who == c) cur_still->who = NULL;
	dispatch_source_cancel(c->rs);
	update_preview_want();
}

static void
client_read(struct client *c)
{
	if (c->gone) return;
	ssize_t r = read(c->fd, c->buf + c->n, sizeof c->buf - 1 - c->n);
	if (r <= 0) {
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
		client_gone(c);
		return;
	}
	c->n += (size_t)r;
	c->buf[c->n] = 0;
	char *start = c->buf, *nl;
	while (!c->gone && (nl = strchr(start, '\n')) != NULL) {
		*nl = 0;
		handle_line(c, start);
		start = nl + 1;
	}
	c->n = strlen(start);
	memmove(c->buf, start, c->n + 1);
	if (c->n >= sizeof c->buf - 1) c->n = 0;
}

static void
accept_client(int ls)
{
	int fd = accept(ls, NULL, NULL);
	if (fd < 0) return;
	uid_t uid = (uid_t)-1;
	gid_t gid;
	if (getpeereid(fd, &uid, &gid) != 0 || (uid != 0 && uid != MOBILE_UID)) {
		LOG("control socket: peer uid %d refused", (int)uid);
		close(fd);
		return;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	struct client *c = calloc(1, sizeof *c);
	if (!c) {
		close(fd);
		return;
	}
	c->fd = fd;
	c->next = clients;
	clients = c;
	c->rs = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, q);
	dispatch_source_set_event_handler(c->rs, ^{ client_read(c); });
	dispatch_source_set_cancel_handler(c->rs, ^{
		close(c->fd);
		for (struct client **pp = &clients; *pp; pp = &(*pp)->next)
			if (*pp == c) {
				*pp = c->next;
				break;
			}
		free(c);
	});
	dispatch_resume(c->rs);
	last_activity = now_s();
}

static int
start_socket(void)
{
	unlink(CAMD_SOCK_PATH);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ls < 0) return -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", CAMD_SOCK_PATH);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
		LOG("control socket %s: %s", CAMD_SOCK_PATH, strerror(errno));
		close(ls);
		return -1;
	}
	chmod(CAMD_SOCK_PATH, 0666);
	fcntl(ls, F_SETFL, fcntl(ls, F_GETFL) | O_NONBLOCK);
	dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)ls, 0, q);
	dispatch_source_set_event_handler(s, ^{ accept_client(ls); });
	dispatch_resume(s);
	LOG("control socket %s ready", CAMD_SOCK_PATH);
	return 0;
}

/* ================================================================== main */
int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	if (argc > 1 && !strcmp(argv[1], "--no-wait")) {
		/* by hand: skip the boot wait */
	} else {
		/* notifications reach jobs started early in the boot only later (as vibd, fpd): wait until it is 2 min old */
		struct timeval bt;
		size_t bl = sizeof bt;
		int bm[2] = { CTL_KERN, KERN_BOOTTIME };
		if (sysctl(bm, 2, &bt, &bl, NULL, 0) == 0) {
			time_t age = time(NULL) - bt.tv_sec;
			if (age >= 0 && age < 120) {
				LOG("boot is %ld s old: waiting", (long)age);
				sleep((unsigned)(120 - age));
			}
		}
	}
	camk_detect();
	conf_load();
	pv_mode = default_preview_mode();
	LOG("camd started (pid %d): kernel %s, exposure %s, preview mode %d, rotate %d", getpid(),
	    camk.ring ? "0276 ring" : camk.present ? "single frames (0275)" : "WITHOUT kern.sweet_cam",
	    camk.kexp ? "kern.sweet_cam_exp" : camk.i2c ? "sensor registers over kern.sweet_cam_i2c" : "none", pv_mode, conf.rotate);
	q = dispatch_queue_create("com.sapphire.camd", DISPATCH_QUEUE_SERIAL);
	dispatch_sync(q, ^{
		if (shm_open_file() != 0) LOG("no shared memory: the preview will not work");
		if (notify_register_check(CAMD_NOTIFY_FRAME, &notify_tok) != NOTIFY_STATUS_OK) notify_tok = -1;
		find_files_dir();
		LOG("photos go to %s", files_dir);
		if (start_socket() != 0) {
			LOG("no control socket: exiting (launchd restarts camd)");
			exit(1);
		}
		last_activity = now_s();
		/* power off after IDLE_OFF_S without preview or still */
		dispatch_source_t idle = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
		dispatch_source_set_timer(idle, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
		dispatch_source_set_event_handler(idle, ^{
			if (powered && !pv_want && !cap_busy && now_s() - last_activity > IDLE_OFF_S) power_off("idle");
		});
		dispatch_resume(idle);
	});
	dispatch_main();
}
