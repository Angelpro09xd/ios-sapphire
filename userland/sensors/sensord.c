/*
 * sensord: the motion and environment sensors for iOS, from the ADSP's sensor core (Qualcomm SEE).
 *
 * Start (launchd job com.sapphire.sensord, kept alive): waits until the boot is 120 s old (notifications reach
 * jobs started early only later), loads the files the ADSP's sensors PD reads (/var/root/sensors/files, laid
 * out by full path: vendor/etc/sensors, mnt/vendor/persist/sensors, sys/devices/soc0; RAM only, as sensload
 * does) unless the kernel's store has them already, starts FastRPC (kern.sweet_frpc = 1, retried until the
 * state is 2) and waits for the sensors to appear (SUID lookups on SNS_CLIENT, QMI 0x190, ADSP node 5).
 *
 * Then, on one QMI client (/dev/redwood-qmiN):
 *   proximity      on-change, always: HID proximity service (backboardd: calls), darwin notification
 *                  com.sapphire.sensor.prox (notify state 1 near / 0 far, posted on every change; fpd ignores
 *                  the fingerprint sensor and SpringBoard does not light the screen for notifications while near)
 *   accelerometer  25 Hz while the screen is on: HID accelerometer (16.16 g, iOS axes: x right, y up, z out of
 *                  the screen, gravity reads -1 g on the axis pointing up -- Android's m/s^2 negated / 9.80665)
 *                  and the device orientation computed from it (HID orientation, CM usages 0xff0c/0x65..0x6a,
 *                  after 0.4 s stable)
 *   gyro, mag      25 Hz while the screen is on and their HID service has a client (rad/s, uT)
 *   ambient light  2 Hz while the screen is on: HID ALS (lux, the event's first value), at most every 0.5 s unless it
 *                  changes by more than 10 %
 *   pickup         on-change while the screen is off: com.sapphire.sensor.pickup (SpringBoard's
 *                  SapphireFingerprint wakes the lock screen with the "raiseToShow" pref, if the proximity is far)
 *   motion_detect  on-change while the screen is off: com.sapphire.sensor.motion
 * Everything goes to the kernel through kern.sweet_sensor ({kind, v[4]} records, SweetHID.cpp's services).
 *
 * Log: /var/root/sensors/sensord.log (256 KB, then sensord.log.1).
 */
#include <stdarg.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <notify.h>
#include "sns_wire.h"

#define DIR_BASE        "/var/root/sensors"
#define FILES_DIR       DIR_BASE "/files"
#define LOG_PATH        DIR_BASE "/sensord.log"
#define LOG_OLD         DIR_BASE "/sensord.log.1"
#define LOG_MAX         (256 * 1024)
#define BOOT_SETTLE_S   120

#define MSG_PROX_EVENT2 769                     /* sns_proximity_event (SEE's proximity sensor) */
#define G               9.80665f

#define N_BLANKED       "com.apple.springboard.hasBlankedScreen"
#define N_PROX          "com.sapphire.sensor.prox"
#define N_PICKUP        "com.sapphire.sensor.pickup"
#define N_MOTION        "com.sapphire.sensor.motion"

/* kern.sweet_sensor kinds (SweetHID.cpp) */
enum { K_ACCEL, K_GYRO, K_COMPASS, K_PROX, K_ALS, K_ORIENT };

/* the sensors used */
enum { S_ACCEL, S_GYRO, S_MAG, S_PROX, S_ALS, S_PICKUP, S_MOTION, S_N };
static struct sensor {
	const char *type;
	struct suid suid;
	bool have;
	int on;                                 /* 0 off, 1 streaming (rate), 2 on-change */
	float rate;
	unsigned events;
} sn[S_N] = {
	{ .type = "accel" }, { .type = "gyro" }, { .type = "mag" }, { .type = "proximity" }, { .type = "ambient_light" },
	{ .type = "pickup" }, { .type = "motion_detect" },
};

static FILE *logf_;

static void
LOG(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	struct stat st;
	if (logf_ != NULL && fstat(fileno(logf_), &st) == 0 && st.st_size > LOG_MAX) {
		fclose(logf_);
		rename(LOG_PATH, LOG_OLD);
		logf_ = NULL;
	}
	if (logf_ == NULL) {
		logf_ = fopen(LOG_PATH, "a");
	}
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char ts[32];
	strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);
	if (logf_ != NULL) {
		fprintf(logf_, "%s %s\n", ts, line);
		fflush(logf_);
	}
	fprintf(stderr, "%s %s\n", ts, line);
}

static time_t
boot_age(void)
{
	struct timeval bt;
	size_t l = sizeof bt;
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };
	if (sysctl(mib, 2, &bt, &l, NULL, 0) != 0) {
		return BOOT_SETTLE_S;
	}
	return time(NULL) - bt.tv_sec;
}

/* ---------------- the sensors PD's files and FastRPC */

static int loaded, load_failed;
static size_t rootlen;

static void
load_file(const char *full)
{
	FILE *f = fopen(full, "rb");
	if (f == NULL) {
		load_failed++;
		return;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	const char *path = full + rootlen;
	size_t pl = strlen(path) + 1;
	char *buf = n >= 0 ? malloc(pl + (size_t)n) : NULL;
	if (buf != NULL) {
		memcpy(buf, path, pl);
		if (fread(buf + pl, 1, (size_t)n, f) == (size_t)n &&
		    sysctlbyname("kern.sweet_frpc_file", NULL, NULL, buf, pl + (size_t)n) == 0) {
			loaded++;
		} else {
			load_failed++;
		}
		free(buf);
	}
	fclose(f);
}

static void
load_tree(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	if (d == NULL) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char p[1024];
		struct stat st;
		if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) {
			continue;
		}
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		if (lstat(p, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			load_tree(p);
		} else if (S_ISREG(st.st_mode)) {
			load_file(p);
		}
	}
	closedir(d);
}

static int
frpc_state(void)
{
	int v = 0;
	size_t l = sizeof v;
	return sysctlbyname("kern.sweet_frpc", &v, &l, NULL, 0) == 0 ? v : -100;
}

static unsigned
store_files(void)
{
	size_t l = 4096;
	char *b = malloc(256 * 1024);
	unsigned n = 0;
	l = 256 * 1024;
	if (b != NULL && sysctlbyname("kern.sweet_frpc_files", b, &l, NULL, 0) == 0) {
		n = (unsigned)strtoul(b, NULL, 10);
	}
	free(b);
	return n;
}

/* the sensors PD up: 0, else -1 (FastRPC stopped at a step: needs a reboot) */
static int
bring_up_pd(void)
{
	int st = frpc_state();
	if (st == 2) {
		LOG("FastRPC already listening (state 2)");
		return 0;
	}
	unsigned have = store_files();
	if (have == 0) {
		rootlen = strlen(FILES_DIR);
		load_tree(FILES_DIR);
		LOG("files for the sensors PD: %d loaded from %s, %d failed", loaded, FILES_DIR, load_failed);
	} else {
		LOG("files for the sensors PD: %u already in the kernel's store", have);
	}
	if (st < 0 && st != -100) {
		LOG("FastRPC stopped earlier (state %d): sensors need a reboot", st);
		return -1;
	}
	for (int i = 0; i < 150; i++) {
		st = frpc_state();
		if (st == 2) {
			LOG("FastRPC listening after %d s", 2 * i);
			return 0;
		}
		if (st < 0) {
			LOG("FastRPC stopped (state %d)", st);
			return -1;
		}
		if (st == 0) {
			int one = 1;
			if (sysctlbyname("kern.sweet_frpc", NULL, NULL, &one, sizeof one) != 0 && errno != EAGAIN) {
				LOG("kern.sweet_frpc=1: %s", strerror(errno));
			}
		}
		sleep(2);
	}
	LOG("FastRPC never reached state 2 (now %d)", frpc_state());
	return -1;
}

/* ---------------- SEE requests */

static int see_request(struct suid s, uint32_t msg_id, const void *payload, uint32_t pn);
static void handle_indication(const struct msg *m);

static int
see_request(struct suid s, uint32_t msg_id, const void *payload, uint32_t pn)
{
	struct pbw req = { .n = 0 }, susp = { .n = 0 }, rq = { .n = 0 };
	w_uint(&susp, 1, 1);            /* APSS */
	w_uint(&susp, 2, 1);            /* NO_WAKEUP */
	w_bytes(&rq, 2, payload, pn);
	w_suid(&req, 1, s);
	w_fixed32(&req, 2, msg_id);
	w_bytes(&req, 3, susp.b, susp.n);
	w_bytes(&req, 4, rq.b, rq.n);
	if (req.err || susp.err || rq.err) {
		return -1;
	}
	static struct msg m, r;
	uint8_t v[2 + sizeof(req.b)];
	uint16_t len = (uint16_t)req.n;
	memcpy(v, &len, 2);
	memcpy(v + 2, req.b, req.n);
	m_init(&m, QMI_SNS_CLIENT_REQ);
	m_tlv(&m, 0x01, v, (uint16_t)(2 + req.n));
	uint8_t jumbo = 1;
	m_tlv(&m, 0x10, &jumbo, 1);
	uint16_t t = ++txn;
	memcpy(m.b + 1, &t, 2);
	uint8_t buf[sizeof(struct sweet_qmi_hdr) + sizeof(m.b)];
	struct sweet_qmi_hdr h = { node, port };
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), m.b, m.n);
	if (write(fd, buf, sizeof(h) + m.n) < 0) {
		LOG("write: %s", strerror(errno));
		return -1;
	}
	double end = now_s() + 2.0;
	for (;;) {
		int left = (int)((end - now_s()) * 1000);
		if (left <= 0) {
			break;
		}
		if (!m_recv(&r, left)) {
			continue;
		}
		if (r.b[0] == 4) {
			handle_indication(&r);
			continue;
		}
		if (r.b[0] != 2 || m_txn(&r) != t) {
			continue;
		}
		uint16_t l;
		const uint8_t *res = tlv(&r, 0x02, &l);
		uint16_t qr = 1, qe = 0xffff;
		if (res != NULL && l >= 4) {
			memcpy(&qr, res, 2);
			memcpy(&qe, res + 2, 2);
		}
		const uint8_t *sr = tlv(&r, 0x11, &l);
		uint32_t see = 0;
		if (sr != NULL && l >= 4) {
			memcpy(&see, sr, 4);
		}
		if (qr != 0 || see != 0) {
			LOG("msg %u to %016llx%016llx: QMI %u/%u, SEE %u", msg_id, (unsigned long long)s.high,
			    (unsigned long long)s.low, qr, qe, see);
			return -1;
		}
		return 0;
	}
	LOG("msg %u: no response within 2 s", msg_id);
	return -2;
}

/* ---------------- events */

static bool screen_on, have_orient_cand;
static int prox_near = -1, orient = -1, orient_cand = -1;
static double orient_since, als_last_t, pickup_last, motion_last;
static float als_last = -1;
static int tok_blank = -1, tok_prox = -1;
static unsigned hid_errors;

static void
push(int kind, int32_t a, int32_t b, int32_t c, int32_t d)
{
	int32_t rec[5] = { kind, a, b, c, d };
	if (sysctlbyname("kern.sweet_sensor", NULL, NULL, rec, sizeof rec) != 0 && hid_errors++ < 5) {
		LOG("kern.sweet_sensor (kind %d): %s", kind, strerror(errno));
	}
}

static int32_t fx(float v) { return (int32_t)lrintf(v * 65536.0f); }

/* iOS's CM usages: 0x65 portrait, 0x66 upside down, 0x67 landscape left, 0x68 landscape right, 0x69 face up, 0x6a face down */
static void
orientation_from(float x, float y, float z)
{
	float g = sqrtf(x * x + y * y + z * z);
	int o = -1;
	if (g < 0.5f || g > 1.6f) {
		return;                         /* shaken: no decision */
	}
	if (fabsf(z) > 0.85f * g) {
		o = z < 0 ? 0x69 : 0x6a;
	} else if (fabsf(y) > fabsf(x) + 0.2f) {
		o = y < 0 ? 0x65 : 0x66;
	} else if (fabsf(x) > fabsf(y) + 0.2f) {
		o = x < 0 ? 0x67 : 0x68;
	}
	if (o < 0) {
		return;
	}
	double t = now_s();
	if (!have_orient_cand || o != orient_cand) {
		have_orient_cand = true;
		orient_cand = o;
		orient_since = t;
		return;
	}
	if (o != orient && t - orient_since >= 0.4) {
		static const char *names[] = { "portrait", "upside down", "landscape left", "landscape right", "face up", "face down" };
		LOG("orientation: %s", names[o - 0x65]);
		orient = o;
		push(K_ORIENT, o, 0, 0, 0);
	}
}

static void
set_prox(int near, int raw)
{
	if (near == prox_near) {
		return;
	}
	prox_near = near;
	LOG("proximity: %s (raw %d)", near ? "NEAR" : "far", raw);
	push(K_PROX, near, raw, 0, 0);
	if (tok_prox >= 0) {
		notify_set_state(tok_prox, (uint64_t)near);
	}
	notify_post(N_PROX);
}

static void
handle_event(int si, uint32_t msg_id, const uint8_t *d, uint32_t n)
{
	struct pb b = { d, d + n };
	struct pbf f;
	float v[8] = { 0 };
	int nv = 0;

	sn[si].events++;
	if (msg_id == MSG_SENSOR_EVENT) {
		while (pb_next(&b, &f) == 1) {
			if (f.num == 1 && f.wt == 5 && nv < 8) {
				v[nv++] = f_float(&f);
			} else if (f.num == 1 && f.wt == 2) {
				for (uint32_t o = 0; o + 4 <= f.n && nv < 8; o += 4) {
					memcpy(&v[nv++], f.d + o, 4);
				}
			}
		}
	}
	switch (si) {
	case S_ACCEL:
		if (nv >= 3) {
			float x = -v[0] / G, y = -v[1] / G, z = -v[2] / G;
			push(K_ACCEL, fx(x), fx(y), fx(z), 0);
			orientation_from(x, y, z);
		}
		break;
	case S_GYRO:
		if (nv >= 3) {
			push(K_GYRO, fx(v[0]), fx(v[1]), fx(v[2]), 0);
		}
		break;
	case S_MAG:
		if (nv >= 3) {
			push(K_COMPASS, fx(v[0]), fx(v[1]), fx(v[2]), 0);
		}
		break;
	case S_PROX:
		if (msg_id == MSG_PROX_EVENT2 || msg_id == MSG_PROX_EVENT) {
			uint64_t type = 0, raw = 0;
			while (pb_next(&b, &f) == 1) {
				if (f.num == 1 && f.wt == 0) {
					type = f.v;
				} else if (f.num == 2 && f.wt == 0) {
					raw = f.v;
				}
			}
			set_prox(type == 1, (int)(int32_t)raw);
		} else if (msg_id == MSG_SENSOR_EVENT && nv >= 1) {
			set_prox(v[0] < 0.5f || (nv >= 2 && v[0] == 1.0f && v[1] > 0), nv >= 2 ? (int)v[1] : 0);
		}
		break;
	case S_ALS:
		if (nv >= 1) {
			double t = now_s();
			float lux = v[0] < 0 ? 0 : v[0];
			if (als_last < 0 || fabsf(lux - als_last) > 0.1f * (als_last + 1) || t - als_last_t > 5) {
				if (t - als_last_t >= 0.5) {
					push(K_ALS, (int32_t)lrintf(lux), 0, 0, 0);
					als_last = lux;
					als_last_t = t;
				}
			}
		}
		break;
	case S_PICKUP:
		if (now_s() - pickup_last > 1.0) {
			pickup_last = now_s();
			LOG("pickup (screen %s, proximity %s)", screen_on ? "on" : "off", prox_near == 1 ? "near" : "far");
			notify_post(N_PICKUP);
		}
		break;
	case S_MOTION:
		if (now_s() - motion_last > 1.0) {
			motion_last = now_s();
			notify_post(N_MOTION);
		}
		break;
	}
}

/* sns_client_event_msg { suid = 1; repeated event events = 2 { msg_id = 1; timestamp = 2; payload = 3 } } */
static struct suid lookup_suid = { SUID_LOOKUP_LOW, SUID_LOOKUP_HIGH };

static void
handle_suid_event(const uint8_t *d, uint32_t n)
{
	struct pb b = { d, d + n };
	struct pbf f;
	char type[64] = "";
	struct suid first = { 0, 0 };
	int nf = 0;
	while (pb_next(&b, &f) == 1) {
		if (f.num == 1 && f.wt == 2) {
			snprintf(type, sizeof(type), "%.*s", (int)f.n, (const char *)f.d);
		} else if (f.num == 2 && f.wt == 2 && nf++ == 0) {
			first = pb_suid(f.d, f.n);
		}
	}
	for (int i = 0; i < S_N; i++) {
		if (strcmp(sn[i].type, type) == 0 && nf > 0 && !sn[i].have) {
			sn[i].suid = first;
			sn[i].have = true;
			LOG("%s: SUID %s (%d found)", type, suid_str(first), nf);
		}
	}
}

static void
handle_event_msg(const uint8_t *d, uint32_t n)
{
	struct pb b = { d, d + n };
	struct pbf f;
	struct suid s = { 0, 0 };
	while (pb_next(&b, &f) == 1) {
		if (f.num == 1 && f.wt == 2) {
			s = pb_suid(f.d, f.n);
		} else if (f.num == 2 && f.wt == 2) {
			struct pb e = { f.d, f.d + f.n };
			struct pbf g;
			uint32_t msg_id = 0;
			const uint8_t *pl = (const uint8_t *)"";
			uint32_t pn = 0;
			while (pb_next(&e, &g) == 1) {
				if (g.num == 1) {
					msg_id = (uint32_t)g.v;
				} else if (g.num == 3 && g.wt == 2) {
					pl = g.d;
					pn = g.n;
				}
			}
			if (msg_id == MSG_SUID_EVENT && suid_eq(s, lookup_suid)) {
				handle_suid_event(pl, pn);
				continue;
			}
			for (int i = 0; i < S_N; i++) {
				if (sn[i].have && suid_eq(s, sn[i].suid)) {
					if (msg_id == MSG_ERROR_EVENT) {
						LOG("%s: error event", sn[i].type);
					} else if (msg_id != MSG_PHYS_CONFIG_EVENT && msg_id != MSG_ATTR_EVENT) {
						handle_event(i, msg_id, pl, pn);
					}
					break;
				}
			}
		}
	}
}

static void
handle_indication(const struct msg *m)
{
	uint16_t id = m_id(m), l;
	if (id != QMI_SNS_REPORT_IND && id != QMI_SNS_JUMBO_IND) {
		return;
	}
	const uint8_t *p = tlv(m, 0x02, &l);
	if (p == NULL || l < 2) {
		return;
	}
	uint16_t pl;
	memcpy(&pl, p, 2);
	if (pl > l - 2) {
		pl = l - 2;
	}
	handle_event_msg(p + 2, pl);
}

static void
pump(int ms)
{
	static struct msg m;
	double end = now_s() + ms / 1000.0;
	for (;;) {
		int left = (int)((end - now_s()) * 1000);
		if (left <= 0) {
			return;
		}
		if (m_recv(&m, left > 100 ? 100 : left) && m.b[0] == 4) {
			handle_indication(&m);
		}
	}
}

/* ---------------- streams */

/* want: 0 off, 1 streaming at rate, 2 on-change (falls back to 2 Hz streaming if refused) */
static void
set_stream(int i, int want, float rate)
{
	struct sensor *s = &sn[i];
	if (!s->have || (s->on == want && (want != 1 || s->rate == rate))) {
		return;
	}
	if (want == 0) {
		see_request(s->suid, MSG_DISABLE, NULL, 0);
		s->on = 0;
		LOG("%s: off (%u events)", s->type, s->events);
		return;
	}
	int r;
	if (want == 2) {
		r = see_request(s->suid, MSG_ON_CHANGE_CONFIG, NULL, 0);
		if (r == -1) {
			struct pbw p = { .n = 0 };
			w_float(&p, 1, 2.0f);
			r = see_request(s->suid, MSG_SENSOR_CONFIG, p.b, p.n);
			LOG("%s: on-change refused, 2 Hz instead: %d", s->type, r);
		}
	} else {
		struct pbw p = { .n = 0 };
		w_float(&p, 1, rate);
		r = see_request(s->suid, MSG_SENSOR_CONFIG, p.b, p.n);
	}
	if (r == 0) {
		s->on = want;
		s->rate = rate;
		LOG("%s: %s", s->type, want == 2 ? "on-change" : "streaming");
		if (i == S_ACCEL) {
			have_orient_cand = false;
			orient = -1;                    /* republish the orientation once it is stable */
		}
	}
}

/* clients of the HID gyro / compass services (kern.sweet_sensor: "name present clients interval samples") */
static void
hid_clients(unsigned *gyro, unsigned *compass)
{
	char b[512];
	size_t l = sizeof(b) - 1;
	*gyro = *compass = 0;
	if (sysctlbyname("kern.sweet_sensor", b, &l, NULL, 0) != 0) {
		return;
	}
	b[l] = 0;
	char name[32];
	unsigned present, clients, iv, cnt;
	for (char *p = b; p != NULL && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
		if (sscanf(p, "%31s %u %u %u %u", name, &present, &clients, &iv, &cnt) == 5) {
			if (strcmp(name, "gyro") == 0) {
				*gyro = clients;
			} else if (strcmp(name, "compass") == 0) {
				*compass = clients;
			}
		}
	}
}

static void
apply(void)
{
	uint64_t blanked = 0;
	if (tok_blank >= 0) {
		notify_get_state(tok_blank, &blanked);
	}
	bool on = blanked == 0;
	if (on != screen_on) {
		screen_on = on;
		LOG("screen %s", on ? "on" : "off");
	}
	unsigned gc, cc;
	hid_clients(&gc, &cc);
	set_stream(S_PROX, 2, 0);
	set_stream(S_ACCEL, on ? 1 : 0, 25.0f);
	set_stream(S_GYRO, on && gc > 0 ? 1 : 0, 25.0f);
	set_stream(S_MAG, on && cc > 0 ? 1 : 0, 25.0f);
	set_stream(S_ALS, on ? 1 : 0, 2.0f);
	set_stream(S_PICKUP, on ? 0 : 2, 0);
	set_stream(S_MOTION, on ? 0 : 2, 0);
}

int
main(void)
{
	mkdir(DIR_BASE, 0755);
	setvbuf(stderr, NULL, _IOLBF, 0);
	time_t age = boot_age();
	if (age < BOOT_SETTLE_S) {
		LOG("boot is %ld s old: waiting %ld s", (long)age, (long)(BOOT_SETTLE_S - age));
		sleep((unsigned)(BOOT_SETTLE_S - age));
	}
	LOG("sensord starting");
	if (bring_up_pd() != 0) {
		for (;;) {
			sleep(3600);                    /* nothing to do until a reboot; staying up keeps launchd quiet */
		}
	}
	if (dev_open() != 0) {
		LOG("no /dev/redwood-qmi node");
		return 1;
	}
	struct sweet_qmi_lookup lk = { SVC_SNS_CLIENT, ~0u, 0, 0, 0, 0 };
	if (ioctl(fd, SWEET_QMI_LOOKUP, &lk) == 0) {
		node = lk.node;
		port = lk.port;
	}
	LOG("SNS_CLIENT at node %u port 0x%x", node, port);
	notify_register_check(N_BLANKED, &tok_blank);
	notify_register_check(N_PROX, &tok_prox);
	notify_set_state(tok_prox, 0);

	/* the sensors appear once the PD has read its registry */
	for (int tries = 0; tries < 60 && !sn[S_ACCEL].have; tries++) {
		for (int i = 0; i < S_N; i++) {
			if (!sn[i].have) {
				struct pbw p = { .n = 0 };
				w_bytes(&p, 1, sn[i].type, (uint32_t)strlen(sn[i].type));
				w_uint(&p, 2, 0);
				w_uint(&p, 3, 1);
				see_request(lookup_suid, MSG_SUID_REQ, p.b, p.n);
				pump(100);
			}
		}
		pump(2000);
	}
	for (int i = 0; i < S_N; i++) {
		if (!sn[i].have) {
			LOG("%s: not found", sn[i].type);
		}
	}
	if (!sn[S_ACCEL].have && !sn[S_PROX].have) {
		LOG("no sensors: exiting (launchd restarts in 30 s)");
		return 1;
	}
	double next_apply = 0;
	for (;;) {
		if (now_s() >= next_apply) {
			apply();
			next_apply = now_s() + 1.0;
		}
		pump(200);
		/* pickup and motion detect are one-shot on some sensors: re-armed after each event */
		for (int i = S_PICKUP; i <= S_MOTION; i++) {
			if (sn[i].on == 2 && sn[i].events > 0 && now_s() - (i == S_PICKUP ? pickup_last : motion_last) > 1.5) {
				sn[i].on = 0;
				sn[i].events = 0;
			}
		}
	}
}
