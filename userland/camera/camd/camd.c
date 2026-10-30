/*
 * camd: the main camera (S5KHM6 through kern.sweet_cam) for apps. The only process that runs
 * the camera ops; apps talk to it over /var/run/camd.sock and read the live preview from shared
 * memory (protocol and layout: camd_proto.h).
 *
 *   threads   two serial queues: ctlq serves the socket, the replies and the watchdog and never
 *             runs a camera op; camq runs every kernel op, the preview and the stills. A stuck or
 *             slow op never keeps clients from being answered (STATUS says what camq is doing).
 *   preview   kernel 0276: op 30 ring (mode 3 1280x720 by default, frame length set for <= 30 fps),
 *             the newest frame through kern.sweet_cam_sel -1; older kernels: the sensor streams
 *             mode 1 and op 23 grabs one frame at a time. Each frame -> BGRX (preview.c), upright,
 *             into the shm; at most 15 frames/s processed (4/s while a still develops). The ring is
 *             polled every 10 ms; "running 0", a fault or no new frame for 1 s restart it.
 *   watchdog  no frame for 2 s while a client wants the preview: op 9 and everything again.
 *   AE / AWB  metered mean green (centre-weighted or a spot) to ~16% with highlight protection,
 *             50 Hz anti-banding, through kern.sweet_cam_exp (0276) or the sensor registers over
 *             kern.sweet_cam_i2c; gray-world white balance with clamps. Both feed the stills.
 *   stills    108 MP: op 32 (0276) or stream mode 0 + op 23; 12 MP: one mode 1 frame. The raw
 *             goes to /var/tmp, a forked child develops it with the pipeline library (develop.c,
 *             all cores) into Files' "On My iPhone"/Camara/IMG_YYYYMMDD_HHMMSS.jpg (+ .dng only
 *             when asked), Exif with the orientation; a thumbnail from the preview in Camara/.thumbs/.
 *   power     ops 1, 2 on demand; op 9 after 30 s without preview or capture.
 * Every wait is bounded and sleeps; nothing polls without a delay.
 * Log: /var/tmp/camd.log (256 KB, then .1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
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
#define PV_FPS     15.0     /* preview frames processed per second at most */
#define PV_FPS_DEV 4.0      /* ... while a still is being developed */
#define WD_NOFRAME 2.0      /* watchdog: seconds without a frame */

/* ================================================================== log */
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

static void
logf_(const char *fmt, ...)
{
	char msg[1200], line[1300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	pthread_mutex_lock(&log_mu);
	struct stat st;
	if (stat(LOG_PATH, &st) == 0 && st.st_size > 256 * 1024) rename(LOG_PATH, LOG_PATH ".1");
	int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd >= 0) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		struct tm tm;
		localtime_r(&tv.tv_sec, &tm);
		int n = snprintf(line, sizeof line, "%02d-%02d %02d:%02d:%02d.%03d %s\n", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		    tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), msg);
		if (n > 0) (void)!write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
		close(fd);
	}
	pthread_mutex_unlock(&log_mu);
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
static pthread_mutex_t conf_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int dng, rotate, quality, preview_mode; /* preview_mode 0: per kernel (3 with the ring, else 1) */
	double cal[5][5];                         /* still brightness / preview brightness, [still][preview] */
} conf = { 0, 90, 92, 0, { { 0 } } };

static void
conf_load(void)
{
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 5; j++) conf.cal[i][j] = 1.0;
	FILE *f = fopen(CONF_PATH, "r");
	if (!f) return;
	char k[64], line[160];
	double v;
	int a, b;
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

/* with conf_mu held */
static void
conf_save_locked(void)
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

/* ================================================================== shared state (atomics) */
static dispatch_queue_t ctlq, camq;
static atomic_bool g_pv_want;             /* some client wants the preview (set on ctlq) */
static atomic_int g_pv_req_mode;          /* the sensor mode it asked for */
static atomic_bool g_cap_busy;            /* a still is being taken or developed */
static atomic_bool g_developing;          /* the develop child runs */
static atomic_bool g_still_taking;        /* camq is taking a still's raw (preview paused on purpose) */
static _Atomic double g_last_frame;       /* now_s() of the last preview frame */
static _Atomic double g_pv_since;         /* the preview was (re)requested or reset at */
static _Atomic double g_op_since;         /* camq inside a kernel call since (0: not) */
static atomic_int g_op_num;
static _Atomic double g_last_activity;
static atomic_bool g_wd_pending;
static atomic_bool g_powered;

/* status snapshot, written by camq */
static pthread_mutex_t snap_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	int mode;
	double exp_us, gain, luma;
	float wb[3];
	float ev;
	bool manual;
	uint64_t seq;
	char last_error[160];
} snap;

/* ================================================================== clients (ctlq only) */
struct client {
	int id, fd;
	dispatch_source_t rs;
	char buf[1024];
	size_t n;
	bool gone, wants_preview;
	int mode;
	double want_at;
	struct client *next;
};
static struct client *clients;
static int next_client_id = 1;

static struct client *
client_by_id(int id)
{
	for (struct client *c = clients; c; c = c->next)
		if (c->id == id && !c->gone) return c;
	return NULL;
}

/* on ctlq: never blocks for long (a client that does not read loses the line) */
static void
send_line(struct client *c, const char *s)
{
	if (c == NULL || c->gone) return;
	size_t n = strlen(s);
	char b[1300];
	if (n > sizeof b - 2) n = sizeof b - 2;
	memcpy(b, s, n);
	b[n++] = '\n';
	size_t off = 0;
	for (int tries = 0; off < n && tries < 25;) {
		ssize_t k = write(c->fd, b + off, n - off);
		if (k > 0) off += (size_t)k;
		else if (k < 0 && (errno == EAGAIN || errno == EINTR)) {
			usleep(2000);
			tries++;
		} else break;
	}
	if (off < n) LOG("client %d: reply dropped (not reading?)", c->id);
}

static void
reply(struct client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void
reply(struct client *c, const char *fmt, ...)
{
	char b[1300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	send_line(c, b);
}

/* from any queue: a line to client `id` (sent on ctlq) */
static void
client_send(int id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void
client_send(int id, const char *fmt, ...)
{
	if (id <= 0) return;
	char b[1300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	char *line = strdup(b);
	if (!line) return;
	dispatch_async(ctlq, ^{
		send_line(client_by_id(id), line);
		free(line);
	});
}

/* ================================================================== camq state */
static bool powered;
static int stream_cur = -1;      /* frames path: the mode streaming with WM7 set up */
static bool ring_on;
static int ring_mode = -1;
static int pv_mode = 1;          /* mode the preview runs in (camq) */
static bool pv_running;          /* camq's view: a preview loop is active */
static unsigned pv_gen;
static uint64_t pv_seq, ring_last_seq;
static double ring_seq_at;       /* when ring_last_seq last advanced */
static int pv_errors, pv_torn;
static uint8_t *frame;
static size_t frame_cap;
static CamdShm *shm;
static int notify_tok = -1;

static struct {
	bool manual;
	double E;
	double t_us, gain;
	unsigned lines, gain_x1000, fll;
	double line_us;
	unsigned def_max_lines, def_fll;
	int mode;
	float ev;
	float mx, my;
	int skip;
	double gplain, gmean, E_at_stats;
	bool have_stats, wb_manual;
	float wb[3];
} ae = { .E = 8000, .gain = 1, .mx = -1, .my = -1, .wb = { 1.9f, 1.0f, 1.6f } };

static int
default_preview_mode(void)
{
	int pm = conf.preview_mode;
	if (pm == 1 || pm == 4) return pm;
	return camk.ring ? 3 : 1;
}

/* ================================================================== camera ops (camq) */
static int
op(int o, int arg, const char *what)
{
	double t0 = now_s();
	atomic_store(&g_op_num, o);
	atomic_store(&g_op_since, t0);
	int r = camk_op(o, arg);
	atomic_store(&g_op_since, 0.0);
	double dt = now_s() - t0;
	if (r != 0) {
		LOG("op %d (%s, arg %#x) failed: %s (%.0f ms); kernel log: %s", o, what, arg < 0 ? 0 : arg, strerror(r), dt * 1000,
		    camk_log);
		pthread_mutex_lock(&snap_mu);
		snprintf(snap.last_error, sizeof snap.last_error, "op %d %s: %s", o, what, strerror(r));
		pthread_mutex_unlock(&snap_mu);
	} else if (dt > 0.5) {
		LOG("op %d (%s) took %.0f ms", o, what, dt * 1000);
	}
	return r;
}

/* a frame read, timed for the watchdog's view of camq */
static int
read_frame(uint8_t *dst, size_t len)
{
	atomic_store(&g_op_num, 100);
	atomic_store(&g_op_since, now_s());
	int r = camk_read(dst, len);
	atomic_store(&g_op_since, 0.0);
	return r;
}

static void
set_powered(bool p)
{
	powered = p;
	atomic_store(&g_powered, p);
}

static int
power_on(void)
{
	if (powered) return 0;
	int r = op(1, -1, "camss on");
	r = r ? r : op(2, -1, "main sensor power");
	if (r == 0) {
		set_powered(true);
		LOG("camera powered");
	}
	return r;
}

static void
forget_hw(void)
{
	set_powered(false);
	ring_on = false;
	ring_mode = -1;
	stream_cur = -1;
}

static void
power_off(const char *why)
{
	if (!powered) return;
	if (ring_on) op(31, -1, "preview off");
	op(9, -1, "all off");
	forget_hw();
	LOG("camera off (%s)", why);
}

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

/* ================================================================== exposure (camq) */
static void
snap_update(void)
{
	pthread_mutex_lock(&snap_mu);
	snap.mode = ae.mode;
	snap.exp_us = ae.t_us;
	snap.gain = ae.gain;
	snap.luma = ae.gmean;
	memcpy(snap.wb, ae.wb, sizeof snap.wb);
	snap.ev = ae.ev;
	snap.manual = ae.manual;
	snap.seq = pv_seq;
	pthread_mutex_unlock(&snap_mu);
}

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

/* E -> time and gain for the current mode (50 Hz anti-banding above 10 ms) and to the sensor.
 * preview: frame length for at most 30 fps (the 120 fps mode 3 would only cost bus and CPU). */
static void
exp_apply(double t_max, bool preview)
{
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
	unsigned lines = (unsigned)(t / ae.line_us + 0.5);
	if (lines < 12) lines = 12;
	unsigned fll = 0;
	if (camk.kexp) {
		unsigned fll30 = (unsigned)(33333.0 / ae.line_us);
		if (preview && fll30 > ae.def_fll) fll = fll30;
		if (lines + 16 > (fll ? fll : ae.def_fll)) fll = lines + 16;
	} else if (lines > ae.def_max_lines) {
		lines = ae.def_max_lines;
	}
	unsigned gx = (unsigned)(g * 1000 + 0.5);
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
	if (st->gp98 * ratio > 0.95) {
		double hr = 0.95 / st->gp98;
		ratio = hr > ratio * 0.5 ? (hr < ratio ? hr : ratio) : ratio * 0.5;
	}
	ratio = ratio < 0.125 ? 0.125 : ratio > 8 ? 8 : ratio;
	if (fabs(log2(ratio)) < 0.1) return;
	double E = Eapplied * pow(ratio, 0.65);
	double Emax = T_MAX_PREVIEW * GAIN_MAX, Emin = 12 * ae.line_us;
	ae.E = E < Emin ? Emin : E > Emax ? Emax : E;
	exp_apply(T_MAX_PREVIEW, true);
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

/* ================================================================== preview (camq) */
#define RENDER_CHUNKS 4
typedef struct {
	const uint8_t *raw;
	PvRaw r;
	PvParams p;
	uint8_t *dst;
	long dst_stride;
	int rows;
	PvStats st[RENDER_CHUNKS];
} RenderJob;

static bool
render_frame(const uint8_t *raw, int mode, uint64_t ts_ns, PvStats *out)
{
	if (!shm) return false;
	const ModeInfo *mi = &MODES[mode];
	static RenderJob j;
	memset(&j, 0, sizeof j);
	j.raw = raw;
	j.r = (PvRaw){ mi->w, mi->h, (long)mi->w * 5 / 4, mi->cell, BLACK, WHITE };
	memcpy(j.p.wb, ae.wb, sizeof j.p.wb);
	j.p.gain = 1;
	j.p.rotate = conf.rotate;
	j.p.max_out = mode == 3 ? CAMD_SHM_MAXDIM : 1000;
	j.p.meter_x = ae.mx;
	j.p.meter_y = ae.my;
	int B, S, ow, oh;
	pv_geometry(&j.r, &j.p, &B, &S, &ow, &oh);
	if (ow > CAMD_SHM_MAXDIM || oh > CAMD_SHM_MAXDIM) return false;
	unsigned slot = (shm->latest + 1) % CAMD_SHM_SLOTS;
	j.dst = shm_slot(slot);
	j.dst_stride = ((long)ow * 4 + 63) & ~63L;
	j.rows = (j.r.h - B) / S + 1;
	dispatch_apply(RENDER_CHUNKS, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t k) {
		int a = (int)((long)j.rows * (long)k / RENDER_CHUNKS), b = (int)((long)j.rows * (long)(k + 1) / RENDER_CHUNKS);
		pv_render_rows(j.raw, &j.r, &j.p, a, b, j.dst, j.dst_stride, &j.st[k]);
	});
	PvStats st;
	memset(&st, 0, sizeof st);
	for (int k = 0; k < RENDER_CHUNKS; k++) pv_stats_merge(&st, &j.st[k]);
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

static void pv_schedule(double delay_s);

static int
preview_start_hw(int mode)
{
	int r = power_on();
	if (r) return r;
	if (camk.ring) {
		r = op(30, mode << 4, "preview ring");
		if (r == EAGAIN || r == ENXIO || r == ENODEV) {
			/* powered off under us (op 9 from elsewhere): power up again once */
			LOG("op 30: %s, powering up again", strerror(r));
			forget_hw();
			r = power_on();
			r = r ? r : op(30, mode << 4, "preview ring");
		}
		ring_on = r == 0;
		ring_mode = r == 0 ? mode : -1;
		ring_last_seq = 0;
		ring_seq_at = now_s();
	} else {
		r = stream_mode(mode);
		if (r == EAGAIN) {
			forget_hw();
			r = power_on();
			r = r ? r : stream_mode(mode);
		}
	}
	if (r == 0) {
		exp_refresh_mode(mode);
		exp_apply(T_MAX_PREVIEW, true);
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

/* everything off and forgotten: the next step powers and starts again */
static void
full_reset(const char *why)
{
	LOG("pipeline reset: %s", why);
	op(9, -1, "all off (reset)");
	forget_hw();
	pv_errors = 0;
	pv_torn = 0;
	atomic_store(&g_pv_since, now_s());
}

static void
pv_fail(const char *what, int r)
{
	pv_errors++;
	if (pv_errors == 1 || pv_errors % 10 == 0) LOG("preview: %s: %s (%d in a row)", what, strerror(r), pv_errors);
	if (pv_errors >= 5) {
		full_reset("preview errors");
		pv_schedule(1.0);
	} else {
		pv_schedule(0.2 * pv_errors);
	}
}

/* one preview frame (camq). Every path ends in exactly one pv_schedule with a delay, or stops. */
static void
pv_step(unsigned gen)
{
	if (gen != pv_gen || !pv_running) return;
	if (atomic_load(&g_still_taking)) {
		pv_schedule(0.1);
		return;
	}
	double t0 = now_s();
	int mode = pv_mode, r = 0;
	size_t len = 0;
	uint64_t ts = 0;
	if (camk.ring) {
		if (!ring_on || ring_mode != mode) {
			r = preview_start_hw(mode);
			if (r) return pv_fail("start", r);
		}
		CamLatest l;
		r = camk_latest(&l);
		if (r) return pv_fail("kern.sweet_cam_latest", r);
		if (!l.running || l.fault) {
			LOG("preview ring not running (running %d seq %llu fault %#x ovf %llu): restarting", l.running,
			    (unsigned long long)l.seq, l.fault, (unsigned long long)l.ovf);
			ring_on = false;
			return pv_fail("ring stopped", EIO);
		}
		if (l.seq == ring_last_seq) {
			if (t0 - ring_seq_at > 1.0) {
				LOG("preview ring: no new frame for %.1f s (seq %llu): restarting", t0 - ring_seq_at,
				    (unsigned long long)l.seq);
				op(31, -1, "preview off (stalled)");
				ring_on = false;
				return pv_fail("ring stalled", ETIMEDOUT);
			}
			pv_schedule(0.010);
			return;
		}
		len = l.bytes;
		if (!frame_buf(len)) return pv_fail("frame buffer", ENOMEM);
		r = camk_select(-1);
		r = r ? r : read_frame(frame, len);
		if (r) return pv_fail("frame read", r);
		CamLatest l2;
		bool torn = camk_latest(&l2) == 0 && l2.seq >= l.seq + 3;
		ring_last_seq = l.seq;
		ring_seq_at = t0;
		ts = l.t_ns;
		if (torn && ++pv_torn < 3) {
			/* the buffer may have been rewritten while being read: skip it (a few times at most) */
			pv_schedule(0.010);
			return;
		}
		pv_torn = 0;
	} else {
		r = stream_cur == mode ? 0 : preview_start_hw(mode);
		r = r ? r : op(23, -1, "frame");
		if (r == 0) {
			long fl = camk_frame_len();
			len = fl > 0 ? (size_t)fl : 0;
			r = len ? 0 : EIO;
		}
		r = r ? r : (frame_buf(len) ? 0 : ENOMEM);
		r = r ? r : read_frame(frame, len);
		if (r) return pv_fail("single frame", r);
		ts = (uint64_t)(now_s() * 1e9);
	}
	const ModeInfo *mi = &MODES[mode];
	if (len < (size_t)mi->w * mi->h * 5 / 4) {
		LOG("preview frame of %zu bytes, %dx%d needs %d", len, mi->w, mi->h, mi->w * mi->h * 5 / 4);
		if (camk.ring) ring_on = false;
		return pv_fail("short frame", EIO);
	}
	pv_errors = 0;
	PvStats st;
	if (render_frame(frame, mode, ts, &st)) {
		ae_update(&st);
		atomic_store(&g_last_frame, now_s());
		snap_update();
	}
	atomic_store(&g_last_activity, now_s());
	double dt = now_s() - t0, period = 1.0 / (atomic_load(&g_developing) ? PV_FPS_DEV : PV_FPS);
	pv_schedule(dt < period ? period - dt : 0.002);
}

static void
pv_schedule(double delay_s)
{
	unsigned gen = pv_gen;
	if (delay_s < 0.001) delay_s = 0.001;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay_s * 1e9)), camq, ^{ pv_step(gen); });
}

/* camq: follow what the clients want */
static void
pv_reconcile(void)
{
	bool want = atomic_load(&g_pv_want);
	int mode = atomic_load(&g_pv_req_mode);
	if (want) {
		if (!pv_running || mode != pv_mode) {
			pv_mode = mode;
			pv_running = true;
			pv_gen++;
			pv_errors = 0;
			pv_schedule(0);
		}
	} else if (pv_running) {
		pv_running = false;
		pv_gen++;
		if (!atomic_load(&g_still_taking)) preview_stop_hw();
		LOG("preview stopped");
	}
	atomic_store(&g_last_activity, now_s());
}

/* ================================================================== files */
static char files_dir[1024];

static bool
find_files_dir(void)
{
	const char *base = "/var/mobile/Containers/Shared/AppGroup";
	DIR *d = opendir(base);
	bool found = false;
	if (d) {
		struct dirent *e;
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
	}
	if (!found) snprintf(files_dir, sizeof files_dir, "/var/mobile/Media/Camara");
	char thumbs[1100];
	mkdir(files_dir, 0755);
	chown(files_dir, MOBILE_UID, MOBILE_UID);
	snprintf(thumbs, sizeof thumbs, "%s/.thumbs", files_dir);
	mkdir(thumbs, 0755);
	chown(thumbs, MOBILE_UID, MOBILE_UID);
	return found;
}

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
	if (!rgb) {
		fclose(f);
		return;
	}
	JpegEnc *e = jpeg_new(w, h, 85);
	g_jpeg_app1 = NULL;
	jpeg_write_header(e, f);
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

/* ================================================================== stills (camq) */
typedef struct {
	int who;             /* client id */
	int mp, mode, pv_mode;
	bool dng;
	char base[1100];
	char jpeg[1200], dngp[1200], thumb[1200];
	double t0, t_dev;
	double E_still, E_preview, gplain, cal_g;
	pid_t pid;
	int pfd;
	char pbuf[512];
	size_t pn;
	int last_pct;
	dispatch_source_t rs, ps;
	uint8_t app1[1024];
	size_t app1_len;
	float wb[3];
	float expect_g;
	struct tm tm;
} Still;

static void
dump_progress(size_t done, size_t total, void *ctx)
{
	Still *s = ctx;
	int p = (int)(done * 18 / (total ? total : 1)) + 1;
	if (p != s->last_pct && s->mp == 108) client_send(s->who, "PROGRESS %d", p);
	s->last_pct = p;
	atomic_store(&g_op_since, now_s()); /* reading: alive */
}

static void still_finish(Still *s, int status);

static void
still_child_line(Still *s, char *line)
{
	int p;
	double g;
	if (sscanf(line, "P %d", &p) == 1) {
		int base = s->mp == 108 ? 20 : 10, pct = base + p * (99 - base) / 100;
		if (pct != s->last_pct) client_send(s->who, "PROGRESS %d", pct);
		s->last_pct = pct;
	} else if (sscanf(line, "CAL %lf", &g) == 1) {
		s->cal_g = g;
	} else if (line[0] == 'T') {
		LOG("develop: %s", line + 1);
	}
}

static int
ncores(void)
{
	int n = 0;
	size_t l = sizeof n;
	if (sysctlbyname("hw.ncpu", &n, &l, NULL, 0) != 0 || n < 1) n = (int)sysconf(_SC_NPROCESSORS_ONLN);
	return n < 1 ? 4 : n > 16 ? 16 : n;
}

static void
still_develop(Still *s)
{
	int pfd[2];
	if (pipe(pfd) != 0) {
		client_send(s->who, "ERR pipe: %s", strerror(errno));
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
	j.expect_g = s->expect_g;
	j.jpeg = s->jpeg;
	j.dng = s->dng ? s->dngp : NULL;
	j.quality = conf.quality;
	j.chunk = 32;
	j.threads = ncores();
	j.sharpen = s->mp == 12 ? 0.3f : 0;
	j.app1 = s->app1_len ? s->app1 : NULL;
	j.app1_len = s->app1_len;
	j.sensor = "S5KHM6";
	s->t_dev = now_s();
	atomic_store(&g_developing, true);
	pid_t pid = fork();
	if (pid == 0) {
		/* the child: no dispatch, only the library, at the foreground app's priority on every core */
		close(pfd[0]);
		j.progress_fd = pfd[1];
		signal(SIGPIPE, SIG_IGN);
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
		int r = develop_run(&j);
		_exit(r == 0 ? 0 : 1);
	}
	close(pfd[1]);
	if (pid < 0) {
		close(pfd[0]);
		client_send(s->who, "ERR fork: %s", strerror(errno));
		still_finish(s, -1);
		return;
	}
	s->pid = pid;
	s->pfd = pfd[0];
	fcntl(s->pfd, F_SETFL, fcntl(s->pfd, F_GETFL) | O_NONBLOCK);
	s->rs = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)s->pfd, 0, camq);
	dispatch_source_set_event_handler(s->rs, ^{
		ssize_t k = read(s->pfd, s->pbuf + s->pn, sizeof s->pbuf - 1 - s->pn);
		if (k <= 0) {
			if (k < 0 && (errno == EAGAIN || errno == EINTR)) return;
			dispatch_source_cancel(s->rs);  /* EOF: no more handler calls */
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
	s->ps = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, camq);
	dispatch_source_set_event_handler(s->ps, ^{
		int st = 0;
		waitpid(s->pid, &st, 0);
		dispatch_source_cancel(s->ps);
		dispatch_release(s->ps);
		s->ps = NULL;
		int status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
		/* let the last progress lines in first */
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), camq, ^{ still_finish(s, status); });
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
	atomic_store(&g_developing, false);
	unlink(RAW_PATH);
	double dt = now_s() - s->t0;
	if (status == 0) {
		chown(s->jpeg, MOBILE_UID, MOBILE_UID);
		chmod(s->jpeg, 0644);
		if (s->dng) {
			chown(s->dngp, MOBILE_UID, MOBILE_UID);
			chmod(s->dngp, 0644);
		}
		if (s->cal_g > 0.005 && s->cal_g < 0.8 && s->gplain > 0.005 && s->E_preview > 0) {
			double r = s->cal_g / (s->gplain * s->E_still / s->E_preview);
			r = r < 0.05 ? 0.05 : r > 20 ? 20 : r;
			pthread_mutex_lock(&conf_mu);
			double *c = &conf.cal[s->mode][s->pv_mode];
			*c = sqrt(*c * r);
			conf_save_locked();
			double cv = *c;
			pthread_mutex_unlock(&conf_mu);
			LOG("still mean green %.4f, preview %.4f (exposure %.0f vs %.0f): ratio %.3f, calibration now %.3f", s->cal_g,
			    s->gplain, s->E_still, s->E_preview, r, cv);
		}
		LOG("still %d MP done in %.1f s (develop %.1f s): %s%s", s->mp, dt, now_s() - s->t_dev, s->jpeg, s->dng ? " + DNG" : "");
		if (s->dng) client_send(s->who, "DNG %s", s->dngp);
		client_send(s->who, "DONE %s", s->jpeg);
		notify_post(CAMD_NOTIFY_PHOTO);
	} else {
		LOG("still %d MP failed (status %d) after %.1f s", s->mp, status, dt);
		unlink(s->jpeg);
		if (s->dng) unlink(s->dngp);
		unlink(s->thumb);
		if (status >= 0) client_send(s->who, "ERR develop failed (%d)", status);
	}
	atomic_store(&g_cap_busy, false);
	atomic_store(&g_last_activity, now_s());
	free(s);
}

/* the raw into RAW_PATH (camq, the preview paused); 0 or an errno. Every wait is bounded. */
static int
still_take(Still *s)
{
	atomic_store(&g_still_taking, true);
	int r = power_on();
	size_t len = 0;
	const ModeInfo *mi = &MODES[s->mode];
	s->E_preview = ae.E_at_stats > 0 ? ae.E_at_stats : ae.t_us * ae.gain;
	s->gplain = ae.have_stats ? ae.gplain : 0;
	double cal = conf.cal[s->mode][s->pv_mode];
	if (r == 0 && camk.ring) {
		if (s->mode == 0) {
			r = op(32, 0, "108 MP still");
			ring_on = false;
			ring_mode = -1;
			r = r ? r : camk_select(100);
		} else {
			if (!ring_on || ring_mode != 1) r = op(30, 1 << 4, "preview ring mode 1");
			ring_on = r == 0;
			ring_mode = r == 0 ? 1 : -1;
			CamLatest l0, l;
			r = r ? r : camk_latest(&l0);
			double t0 = now_s();
			while (r == 0) {
				r = camk_latest(&l);
				if (r == 0 && (!l.running || l.fault)) r = EIO;
				else if (r == 0 && l.seq >= l0.seq + 2) break;
				else if (now_s() - t0 > 2) r = ETIMEDOUT;
				else usleep(10000);
			}
			r = r ? r : camk_select(-1);
		}
		s->E_still = ae.t_us * ae.gain;
	} else if (r == 0) {
		r = stream_mode(s->mode);
		if (r == 0) {
			exp_refresh_mode(s->mode);
			double E = (ae.manual ? ae.t_us * ae.gain : ae.E) / cal;
			bool man = ae.manual;
			ae.manual = false;
			double keepE = ae.E;
			ae.E = E;
			exp_apply(66000, false);
			ae.E = keepE;
			ae.manual = man;
			s->E_still = ae.t_us * ae.gain;
			usleep((useconds_t)(2.2e6 / mi->fps));
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
		client_send(s->who, "SHUTTER");
		int fd = open(RAW_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) r = errno;
		else {
			double t0 = now_s();
			s->last_pct = -1;
			atomic_store(&g_op_num, 101);
			atomic_store(&g_op_since, t0);
			r = camk_read_to_fd(fd, len, dump_progress, s);
			atomic_store(&g_op_since, 0.0);
			close(fd);
			LOG("still raw %zu bytes read in %.2f s", len, now_s() - t0);
		}
	}
	s->expect_g = (float)(s->gplain > 0 ? s->gplain * (s->E_still / (s->E_preview > 0 ? s->E_preview : s->E_still)) * cal : 0);
	if (r != 0) {
		/* whatever state the sensor is in now: start clean */
		full_reset("still failed");
	}
	/* back to the preview (restarted by its next step), or stop streaming */
	if (camk.ring) {
		ring_on = false;
		ring_mode = -1;
	} else if (!pv_running && stream_cur >= 0) {
		op(13, -1, "stop");
		stream_cur = -1;
	}
	atomic_store(&g_still_taking, false);
	atomic_store(&g_pv_since, now_s());
	if (pv_running) {
		pv_gen++;
		pv_schedule(0);
	}
	return r;
}

static void
still_begin(int who, int mp, bool dng)
{
	Still *s = calloc(1, sizeof *s);
	if (!s) {
		client_send(who, "ERR no memory");
		atomic_store(&g_cap_busy, false);
		return;
	}
	s->who = who;
	s->mp = mp;
	s->mode = mp == 108 ? 0 : 1;
	s->pv_mode = pv_running ? pv_mode : default_preview_mode();
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
	LOG("still %d MP%s -> %s (preview mode %d, exposure %.0f us x %.2f)", mp, dng ? " + DNG" : "", s->jpeg, s->pv_mode, ae.t_us,
	    ae.gain);
	write_thumb(s->thumb);
	int r = still_take(s);
	if (r != 0) {
		client_send(who, "ERR capture failed: %s", strerror(r));
		still_finish(s, -1);
		return;
	}
	client_send(who, "PROGRESS %d", mp == 108 ? 20 : 10);
	still_develop(s);
}

/* ================================================================== commands (ctlq) */
static const char *
op_name(int o)
{
	switch (o) {
	case 100: return "preview frame read";
	case 101: return "still read";
	default: return "op";
	}
}

static void
cmd_status(struct client *c)
{
	pthread_mutex_lock(&snap_mu);
	typeof(snap) sn = snap;
	pthread_mutex_unlock(&snap_mu);
	double now = now_s(), since = atomic_load(&g_op_since), lf = atomic_load(&g_last_frame);
	char busy[96] = "idle";
	if (since > 0) snprintf(busy, sizeof busy, "%s_%d_for_%.1fs", op_name(atomic_load(&g_op_num)), atomic_load(&g_op_num), now - since);
	bool cap = atomic_load(&g_cap_busy), pv = atomic_load(&g_pv_want);
	reply(c, "STATUS state=%s kernel=%s exp=%s powered=%d preview=%d mode=%d seq=%llu last_frame=%.1fs camq=%s exp_us=%.0f "
	    "gain=%.2f ae=%s ev=%.1f luma=%.3f wb=%.2f,%.2f,%.2f busy=%d developing=%d dng=%d rotate=%d quality=%d "
	    "preview_mode=%d last_error=\"%s\" dir=%s",
	    cap ? "capture" : pv ? "preview" : atomic_load(&g_powered) ? "on" : "off",
	    camk.ring ? "ring" : camk.present ? "frames" : "none", camk.kexp ? "sysctl" : camk.i2c ? "i2c" : "none",
	    atomic_load(&g_powered), pv, sn.mode, (unsigned long long)sn.seq, lf > 0 ? now - lf : -1.0, busy, sn.exp_us, sn.gain,
	    sn.manual ? "manual" : "auto", sn.ev, sn.luma, sn.wb[0], sn.wb[1], sn.wb[2], cap, atomic_load(&g_developing), conf.dng,
	    conf.rotate, conf.quality, default_preview_mode(), sn.last_error, files_dir);
}

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

/* ctlq: recompute what the clients want, hand it to camq */
static void
update_preview_want(void)
{
	bool want = false;
	int mode = default_preview_mode();
	double latest = -1;
	for (struct client *c = clients; c; c = c->next)
		if (!c->gone && c->wants_preview) {
			want = true;
			if (c->want_at > latest) {
				latest = c->want_at;
				mode = c->mode;
			}
		}
	bool was = atomic_exchange(&g_pv_want, want);
	int old = atomic_exchange(&g_pv_req_mode, mode);
	if (want && (!was || old != mode)) atomic_store(&g_pv_since, now_s());
	if (was != want || old != mode) dispatch_async(camq, ^{ pv_reconcile(); });
	atomic_store(&g_last_activity, now_s());
}

static void
handle_line(struct client *c, char *line)
{
	size_t l = strlen(line);
	while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
	char *args = strchr(line, ' ');
	if (args) *args++ = 0;
	else args = line + l;
	atomic_store(&g_last_activity, now_s());
	if (!strcmp(line, "HELLO")) {
		reply(c, "OK camd 1 kernel=%s exp=%s", camk.ring ? "ring" : camk.present ? "frames" : "none",
		    camk.kexp ? "sysctl" : camk.i2c ? "i2c" : "none");
	} else if (!strcmp(line, "STATUS")) {
		cmd_status(c);
	} else if (!strcmp(line, "START")) {
		if (!camk.present) {
			reply(c, "ERR no camera driver in this kernel");
			return;
		}
		int m = atoi(args);
		if (m != 1 && m != 3 && m != 4) m = default_preview_mode();
		if (m == 3 && !camk.ring) m = 1;
		c->wants_preview = true;
		c->mode = m;
		c->want_at = now_s();
		update_preview_want();
		PvRaw r = { MODES[m].w, MODES[m].h, MODES[m].w * 5 / 4, MODES[m].cell, BLACK, WHITE };
		PvParams p = { .rotate = conf.rotate, .max_out = m == 3 ? CAMD_SHM_MAXDIM : 1000 };
		int ow, oh;
		pv_geometry(&r, &p, NULL, NULL, &ow, &oh);
		reply(c, "OK preview %d %d %s", ow, oh, CAMD_SHM_PATH);
	} else if (!strcmp(line, "STOP")) {
		c->wants_preview = false;
		update_preview_want();
		reply(c, "OK");
	} else if (!strcmp(line, "FRAME")) {
		if (!shm || shm->seq == 0) {
			reply(c, "ERR no frame yet");
			return;
		}
		const CamdSlot *s = &shm->slot[shm->latest];
		reply(c, "FRAME %s %llu %u %u %u %llu", CAMD_SHM_PATH, (unsigned long long)s->seq, s->width, s->height, s->stride,
		    (unsigned long long)s->ts_ns);
	} else if (!strcmp(line, "CAPTURE")) {
		if (!camk.present) {
			reply(c, "ERR no camera driver in this kernel");
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
		bool expected = false;
		if (!atomic_compare_exchange_strong(&g_cap_busy, &expected, true)) {
			reply(c, "ERR busy");
			return;
		}
		reply(c, "OK capture %d", mp);
		int id = c->id;
		dispatch_async(camq, ^{ still_begin(id, mp, dng); });
	} else if (!strcmp(line, "EXPOSURE")) {
		unsigned lines;
		double g;
		if (!strcmp(args, "auto")) {
			dispatch_async(camq, ^{ ae.manual = false; });
			reply(c, "OK");
		} else if (sscanf(args, "%u,%lf", &lines, &g) == 2 && lines >= 12 && g >= 1 && g <= 64) {
			dispatch_async(camq, ^{
				ae.manual = true;
				ae.t_us = lines * ae.line_us;
				ae.gain = g;
				if (powered) exp_apply(1e9, pv_running);
			});
			reply(c, "OK");
		} else {
			reply(c, "ERR EXPOSURE auto | lines,gain");
		}
	} else if (!strcmp(line, "EV")) {
		double v = atof(args);
		float ev = (float)(v < -3 ? -3 : v > 3 ? 3 : v);
		dispatch_async(camq, ^{
			ae.ev = ev;
			ae.skip = 0;
		});
		reply(c, "OK");
	} else if (!strcmp(line, "METER")) {
		double x, y;
		float mx = -1, my = -1;
		if (sscanf(args, "%lf,%lf", &x, &y) == 2 && x >= 0 && x <= 1 && y >= 0 && y <= 1) portrait_to_raw(x, y, &mx, &my);
		dispatch_async(camq, ^{
			ae.mx = mx;
			ae.my = my;
			ae.skip = 0;
		});
		reply(c, "OK");
	} else if (!strcmp(line, "WB")) {
		float r, g, b;
		bool man = sscanf(args, "%f,%f,%f", &r, &g, &b) == 3 && r > 0 && g > 0 && b > 0;
		dispatch_async(camq, ^{
			ae.wb_manual = man;
			if (man) {
				ae.wb[0] = r / g;
				ae.wb[1] = 1;
				ae.wb[2] = b / g;
			}
		});
		reply(c, "OK");
	} else if (!strcmp(line, "SET")) {
		char k[32];
		int v;
		if (sscanf(args, "%31s %d", k, &v) != 2) {
			reply(c, "ERR SET key value");
			return;
		}
		pthread_mutex_lock(&conf_mu);
		bool ok = true;
		if (!strcmp(k, "dng")) conf.dng = v != 0;
		else if (!strcmp(k, "rotate") && (v == 0 || v == 90 || v == 180 || v == 270)) conf.rotate = v;
		else if (!strcmp(k, "quality") && v >= 50 && v <= 100) conf.quality = v;
		else if (!strcmp(k, "preview_mode") && (v == 0 || v == 1 || v == 3 || v == 4)) conf.preview_mode = v;
		else ok = false;
		if (ok) conf_save_locked();
		pthread_mutex_unlock(&conf_mu);
		reply(c, ok ? "OK" : "ERR unknown key or value");
	} else if (line[0] != 0) {
		reply(c, "ERR unknown command");
	}
}

static void
client_gone(struct client *c)
{
	if (c->gone) return;
	c->gone = true;
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
	for (int k = 0; k < 16; k++) { /* drain the backlog, bounded */
		int fd = accept(ls, NULL, NULL);
		if (fd < 0) return;
		uid_t uid = (uid_t)-1;
		gid_t gid;
		if (getpeereid(fd, &uid, &gid) != 0 || (uid != 0 && uid != MOBILE_UID)) {
			LOG("control socket: peer uid %d refused", (int)uid);
			close(fd);
			continue;
		}
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		struct client *c = calloc(1, sizeof *c);
		if (!c) {
			close(fd);
			continue;
		}
		c->id = next_client_id++;
		c->fd = fd;
		c->next = clients;
		clients = c;
		c->rs = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, ctlq);
		dispatch_source_set_event_handler(c->rs, ^{ client_read(c); });
		dispatch_source_set_cancel_handler(c->rs, ^{
			close(c->fd);
			for (struct client **pp = &clients; *pp; pp = &(*pp)->next)
				if (*pp == c) {
					*pp = c->next;
					break;
				}
			dispatch_release(c->rs);
			free(c);
		});
		dispatch_resume(c->rs);
	}
}

static int
start_socket(void)
{
	unlink(CAMD_SOCK_PATH);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ls < 0) return -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", CAMD_SOCK_PATH);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 32) != 0) {
		LOG("control socket %s: %s", CAMD_SOCK_PATH, strerror(errno));
		close(ls);
		return -1;
	}
	chmod(CAMD_SOCK_PATH, 0666);
	fcntl(ls, F_SETFL, fcntl(ls, F_GETFL) | O_NONBLOCK);
	fcntl(ls, F_SETFD, FD_CLOEXEC);
	dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)ls, 0, ctlq);
	dispatch_source_set_event_handler(s, ^{ accept_client(ls); });
	dispatch_resume(s);
	LOG("control socket %s ready", CAMD_SOCK_PATH);
	return 0;
}

/* ================================================================== watchdog (ctlq) */
static void
watchdog_tick(void)
{
	double now = now_s();
	double since = atomic_load(&g_op_since);
	static double stuck_logged;
	if (since > 0 && now - since > 15 && since != stuck_logged) {
		stuck_logged = since;
		LOG("watchdog: camq inside %s %d for %.0f s (in the kernel); clients are still served", op_name(atomic_load(&g_op_num)),
		    atomic_load(&g_op_num), now - since);
	}
	if (!atomic_load(&g_pv_want) || atomic_load(&g_still_taking) || since > 0 || atomic_load(&g_wd_pending)) return;
	double lf = atomic_load(&g_last_frame), ps = atomic_load(&g_pv_since);
	double ref = lf > ps ? lf : ps;
	if (now - ref < WD_NOFRAME + (lf > ps ? 0 : 2)) return; /* a cold start gets 2 s more */
	atomic_store(&g_wd_pending, true);
	atomic_store(&g_pv_since, now);
	double gap = now - ref;
	dispatch_async(camq, ^{
		atomic_store(&g_wd_pending, false);
		if (!pv_running || atomic_load(&g_still_taking)) return;
		char why[80];
		snprintf(why, sizeof why, "watchdog: no preview frame for %.1f s", gap);
		full_reset(why);
		pv_gen++;
		pv_schedule(0.05);
	});
}

/* ================================================================== main */
int
main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	if (!(argc > 1 && !strcmp(argv[1], "--no-wait"))) {
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
	atomic_store(&g_pv_req_mode, default_preview_mode());
	LOG("camd started (pid %d): kernel %s, exposure %s, preview mode %d, rotate %d, %d cores", getpid(),
	    camk.ring ? "0276 ring" : camk.present ? "single frames (0275)" : "WITHOUT kern.sweet_cam",
	    camk.kexp ? "kern.sweet_cam_exp" : camk.i2c ? "sensor registers over kern.sweet_cam_i2c" : "none",
	    default_preview_mode(), conf.rotate, ncores());
	ctlq = dispatch_queue_create("com.sapphire.camd.ctl", DISPATCH_QUEUE_SERIAL);
	camq = dispatch_queue_create("com.sapphire.camd.cam",
	    dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
	if (shm_open_file() != 0) LOG("no shared memory: the preview will not work");
	if (notify_register_check(CAMD_NOTIFY_FRAME, &notify_tok) != NOTIFY_STATUS_OK) notify_tok = -1;
	find_files_dir();
	LOG("photos go to %s", files_dir);
	atomic_store(&g_last_activity, now_s());
	__block int sock_ok = 0;
	dispatch_sync(ctlq, ^{ sock_ok = start_socket() == 0; });
	if (!sock_ok) {
		LOG("no control socket: exiting (launchd restarts camd)");
		exit(1);
	}
	dispatch_source_t wd = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, ctlq);
	dispatch_source_set_timer(wd, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), 500 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(wd, ^{ watchdog_tick(); });
	dispatch_resume(wd);
	/* power off after IDLE_OFF_S without preview or still */
	dispatch_source_t idle = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, camq);
	dispatch_source_set_timer(idle, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(idle, ^{
		if (powered && !pv_running && !atomic_load(&g_cap_busy) && now_s() - atomic_load(&g_last_activity) > IDLE_OFF_S)
			power_off("idle");
	});
	dispatch_resume(idle);
	dispatch_main();
}
