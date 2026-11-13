/*
 * sns_wire.h: the SNS_CLIENT wire helpers shared with snsprobe.c (QMI over /dev/redwood-qmiN, nanopb-style
 * protobuf encoded and decoded by hand), copied from snsprobe.c for sensord.
 */
#pragma clang diagnostic ignored "-Wunused-function"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>

struct sweet_qmi_hdr { uint32_t node, port; };
struct sweet_qmi_lookup { uint32_t svc, inst, index, node, port, found_inst; };
#define SWEET_QMI_LOOKUP        _IOWR('q', 1, struct sweet_qmi_lookup)

#define SVC_SNS_CLIENT          0x190
#define QMI_SNS_CLIENT_REQ      0x20
#define QMI_SNS_REPORT_IND      0x21
#define QMI_SNS_JUMBO_IND       0x22

/* SEE message ids */
#define MSG_DISABLE             10      /* SNS_CLIENT_MSGID_SNS_CLIENT_DISABLE_REQ */
#define MSG_ATTR_REQ            1       /* SNS_STD_MSGID_SNS_STD_ATTR_REQ */
#define MSG_ATTR_EVENT          128
#define MSG_ERROR_EVENT         129
#define MSG_SUID_REQ            512
#define MSG_SUID_EVENT          768
#define MSG_SENSOR_CONFIG       513     /* sns_std_sensor_config { float sample_rate = 1 } */
#define MSG_ON_CHANGE_CONFIG    514
#define MSG_PROX_EVENT          1024    /* sns_proximity_event */
#define MSG_SENSOR_EVENT        1025    /* sns_std_sensor_event { repeated float data = 1; status = 2 } */
#define MSG_PHYS_CONFIG_EVENT   1026

#define SUID_LOOKUP_LOW         0xababababababababull
#define SUID_LOOKUP_HIGH        0xababababababababull

struct suid { uint64_t low, high; };
struct msg { uint8_t b[0x4000]; uint32_t n; };

static int verbose;
static int fd = -1;
static uint32_t node = 5, port = 0xd;
static uint16_t txn;
static volatile sig_atomic_t stop;

/* ---------------- small helpers */

static double
now_s(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void
hexdump(const char *what, const uint8_t *p, uint32_t n)
{
	fprintf(stderr, "%s (%u):", what, n);
	for (uint32_t i = 0; i < n && i < 512; i++) {
		fprintf(stderr, " %02x", p[i]);
	}
	fprintf(stderr, n > 512 ? " ...\n" : "\n");
}

static void
on_alarm(int sig)
{
	(void)sig;
	static const char m[] = "snsprobe: overall timeout, giving up\n";
	write(2, m, sizeof(m) - 1);
	_exit(3);                       /* the kernel closes the port: QRTR DEL_CLIENT */
}

static void
on_int(int sig)
{
	(void)sig;
	stop = 1;
}

/* ---------------- protobuf, by hand */

struct pbw { uint8_t b[1000]; uint32_t n; bool err; };

static void
w_byte(struct pbw *w, uint8_t v)
{
	if (w->n >= sizeof(w->b)) {
		w->err = true;
		return;
	}
	w->b[w->n++] = v;
}

static void
w_varint(struct pbw *w, uint64_t v)
{
	do {
		uint8_t c = v & 0x7f;
		v >>= 7;
		w_byte(w, c | (v ? 0x80 : 0));
	} while (v);
}

static void w_key(struct pbw *w, uint32_t num, uint32_t wt) { w_varint(w, ((uint64_t)num << 3) | wt); }

static void
w_fixed64(struct pbw *w, uint32_t num, uint64_t v)
{
	w_key(w, num, 1);
	for (int i = 0; i < 8; i++) {
		w_byte(w, (uint8_t)(v >> (8 * i)));
	}
}

static void
w_fixed32(struct pbw *w, uint32_t num, uint32_t v)
{
	w_key(w, num, 5);
	for (int i = 0; i < 4; i++) {
		w_byte(w, (uint8_t)(v >> (8 * i)));
	}
}

static void
w_float(struct pbw *w, uint32_t num, float f)
{
	uint32_t v;
	memcpy(&v, &f, 4);
	w_fixed32(w, num, v);
}

static void
w_uint(struct pbw *w, uint32_t num, uint64_t v)
{
	w_key(w, num, 0);
	w_varint(w, v);
}

static void
w_bytes(struct pbw *w, uint32_t num, const void *d, uint32_t n)
{
	w_key(w, num, 2);
	w_varint(w, n);
	for (uint32_t i = 0; i < n; i++) {
		w_byte(w, ((const uint8_t *)d)[i]);
	}
}

static void
w_suid(struct pbw *w, uint32_t num, struct suid s)
{
	struct pbw in = { .n = 0 };
	w_fixed64(&in, 1, s.low);
	w_fixed64(&in, 2, s.high);
	w_bytes(w, num, in.b, in.n);
}

struct pb { const uint8_t *p, *end; };
struct pbf { uint32_t num, wt; uint64_t v; const uint8_t *d; uint32_t n; };

static bool
pb_varint(struct pb *b, uint64_t *v)
{
	*v = 0;
	for (int s = 0; s < 64; s += 7) {
		if (b->p >= b->end) {
			return false;
		}
		uint8_t c = *b->p++;
		*v |= (uint64_t)(c & 0x7f) << s;
		if (!(c & 0x80)) {
			return true;
		}
	}
	return false;
}

/* 1: a field, 0: end, -1: malformed */
static int
pb_next(struct pb *b, struct pbf *f)
{
	uint64_t key, len;
	if (b->p >= b->end) {
		return 0;
	}
	if (!pb_varint(b, &key)) {
		return -1;
	}
	f->num = (uint32_t)(key >> 3);
	f->wt = (uint32_t)(key & 7);
	f->d = NULL;
	f->n = 0;
	f->v = 0;
	switch (f->wt) {
	case 0:
		return pb_varint(b, &f->v) ? 1 : -1;
	case 1:
		if (b->end - b->p < 8) {
			return -1;
		}
		memcpy(&f->v, b->p, 8);
		b->p += 8;
		return 1;
	case 2:
		if (!pb_varint(b, &len) || (uint64_t)(b->end - b->p) < len) {
			return -1;
		}
		f->d = b->p;
		f->n = (uint32_t)len;
		b->p += len;
		return 1;
	case 5:
		if (b->end - b->p < 4) {
			return -1;
		}
		{
			uint32_t v32;
			memcpy(&v32, b->p, 4);
			f->v = v32;
		}
		b->p += 4;
		return 1;
	default:
		return -1;
	}
}

static float
f_float(const struct pbf *f)
{
	uint32_t v = (uint32_t)f->v;
	float x;
	memcpy(&x, &v, 4);
	return x;
}

static struct suid
pb_suid(const uint8_t *d, uint32_t n)
{
	struct suid s = { 0, 0 };
	struct pb b = { d, d + n };
	struct pbf f;
	while (pb_next(&b, &f) == 1) {
		if (f.num == 1 && f.wt == 1) {
			s.low = f.v;
		} else if (f.num == 2 && f.wt == 1) {
			s.high = f.v;
		}
	}
	return s;
}

static bool suid_eq(struct suid a, struct suid b) { return a.low == b.low && a.high == b.high; }

static const char *
suid_str(struct suid s)
{
	static char buf[4][40];
	static int k;
	char *p = buf[k++ & 3];
	snprintf(p, 40, "%016llx%016llx", (unsigned long long)s.high, (unsigned long long)s.low);
	return p;
}

/* ---------------- QMI over /dev/redwood-qmiN */

static void
m_init(struct msg *m, uint16_t id)
{
	m->b[0] = 0;                    /* request */
	m->b[1] = m->b[2] = 0;
	memcpy(m->b + 3, &id, 2);
	m->b[5] = m->b[6] = 0;
	m->n = 7;
}

static void
m_tlv(struct msg *m, uint8_t type, const void *v, uint16_t len)
{
	m->b[m->n] = type;
	memcpy(m->b + m->n + 1, &len, 2);
	memcpy(m->b + m->n + 3, v, len);
	m->n += 3 + len;
	uint16_t total = (uint16_t)(m->n - 7);
	memcpy(m->b + 5, &total, 2);
}

static const uint8_t *
tlv(const struct msg *m, uint8_t type, uint16_t *len)
{
	uint32_t o = 7;
	while (o + 3 <= m->n) {
		uint16_t l;
		memcpy(&l, m->b + o + 1, 2);
		if (o + 3 + l > m->n) {
			break;
		}
		if (m->b[o] == type) {
			*len = l;
			return m->b + o + 3;
		}
		o += 3 + l;
	}
	return NULL;
}

static uint16_t m_id(const struct msg *m) { uint16_t v; memcpy(&v, m->b + 3, 2); return v; }
static uint16_t m_txn(const struct msg *m) { uint16_t v; memcpy(&v, m->b + 1, 2); return v; }

static int
dev_open(void)
{
	char path[32];
	for (int i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/redwood-qmi%d", i);
		fd = open(path, O_RDWR);
		if (fd >= 0) {
			break;
		}
		if (errno != EBUSY) {
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			return -1;
		}
	}
	if (fd < 0) {
		fprintf(stderr, "all /dev/redwood-qmi nodes busy\n");
		return -1;
	}
	txn = (uint16_t)((getpid() & 0xff) << 8);
	return 0;
}

/* next message, 1; timeout or error, 0 */
static int
m_recv(struct msg *m, int ms)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
		return 0;
	}
	static uint8_t buf[sizeof(struct sweet_qmi_hdr) + sizeof(m->b)];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n < (ssize_t)sizeof(struct sweet_qmi_hdr) + 7) {
		return 0;
	}
	m->n = (uint32_t)(n - (ssize_t)sizeof(struct sweet_qmi_hdr));
	memcpy(m->b, buf + sizeof(struct sweet_qmi_hdr), m->n);
	if (verbose) {
		hexdump("<-", m->b, m->n);
	}
	return 1;
}

