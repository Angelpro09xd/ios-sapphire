/*
 * snsprobe: talk to the ADSP's sensor core (Qualcomm SEE) from the shell, through /dev/redwood-qmiN
 * (kernel 0220: one QRTR client port per open, QMI messages in and out).
 *
 * The ADSP announces the SEE client service SNS_CLIENT (QMI service 0x190) at boot; the kernel log
 * shows it as "adsp: t=.. ms: service 190:1 node 5 port d". The kernel's lookup ioctl only knows the
 * modem's services, so the default address is node 5, port 0xd (-n / -p override it).
 *
 *   snsprobe [-v] [-n NODE] [-p PORT] info [TYPE...]   SUID lookup + attributes of every sensor found
 *   snsprobe [-v] [-n NODE] [-p PORT] stream TYPE RATE SECS
 *                                                       RATE Hz (1..50), 0 = on-change; SECS 1..60
 *
 * QMI SNS_CLIENT: request 0x20 (TLV 0x01 payload = u16 length + bytes, TLV 0x10 use_jumbo_report),
 * response 0x20 (TLV 0x02 result, 0x10 client id, 0x11 SEE result), indications 0x21 report /
 * 0x22 jumbo report (TLV 0x01 client id, TLV 0x02 payload). The payloads are protobuf
 * (sns_client_request_msg / sns_client_event_msg), encoded and decoded here by hand.
 *
 * Every wait has a timeout and an alarm() ends the process if anything still hangs; closing the
 * device makes the kernel send QRTR DEL_CLIENT, so the ADSP drops whatever this client asked for.
 */
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

/* ---------------- SEE events */

enum { MODE_LOOKUP, MODE_ATTR, MODE_STREAM };
static int mode;

/* lookup results */
#define MAX_TYPES 16
#define MAX_SUIDS 8
static struct {
	const char *type;
	bool answered;
	int n;
	struct suid s[MAX_SUIDS];
} types[MAX_TYPES];
static int ntypes;

static struct suid attr_target;
static bool attr_done;
static uint64_t ts0;
static unsigned stream_events;

static const char *
attr_name(int id)
{
	static const char *n[] = { "NAME", "VENDOR", "TYPE", "AVAILABLE", "VERSION", "API", "RATES",
		"RESOLUTIONS", "FIFO_SIZE", "ACTIVE_CURRENT", "SLEEP_CURRENT", "RANGES", "OP_MODES", "DRI",
		"STREAM_SYNC", "EVENT_SIZE", "STREAM_TYPE", "DYNAMIC", "HW_ID", "RIGID_BODY", "PLACEMENT",
		"PHYSICAL_SENSOR", "PHYSICAL_SENSOR_TESTS", "SELECTED_RESOLUTION", "SELECTED_RANGE",
		"ADDITIONAL_LOW_LATENCY_RATES", "PASSIVE_REQUEST", "TRANSPORT_MTU_SIZE", "HLOS_INCOMPATIBLE" };
	return id >= 0 && id < (int)(sizeof(n) / sizeof(n[0])) ? n[id] : NULL;
}

/* sns_std_attr_value { repeated data values = 1 }, data: oneof subtype=1 str=2 flt=3 sint=4 (zigzag) boolean=5 */
static void
print_attr_value(const uint8_t *d, uint32_t n, int depth)
{
	struct pb b = { d, d + n };
	struct pbf f;
	bool first = true;
	while (pb_next(&b, &f) == 1) {
		if (f.num != 1 || f.wt != 2) {
			continue;
		}
		struct pb v = { f.d, f.d + f.n };
		struct pbf g;
		printf("%s", first ? "" : ", ");
		first = false;
		while (pb_next(&v, &g) == 1) {
			if (g.num == 1 && g.wt == 2 && depth < 4) {
				printf("{");
				print_attr_value(g.d, g.n, depth + 1);
				printf("}");
			} else if (g.num == 2 && g.wt == 2) {
				printf("\"%.*s\"", (int)g.n, (const char *)g.d);
			} else if (g.num == 3 && g.wt == 5) {
				printf("%g", f_float(&g));
			} else if (g.num == 4 && g.wt == 0) {
				int64_t s = (int64_t)(g.v >> 1) ^ -(int64_t)(g.v & 1);
				printf("%lld", (long long)s);
			} else if (g.num == 5 && g.wt == 0) {
				printf("%s", g.v ? "true" : "false");
			}
		}
	}
}

static void
handle_attr_event(struct suid s, const uint8_t *d, uint32_t n)
{
	if (mode != MODE_ATTR || !suid_eq(s, attr_target)) {
		return;
	}
	struct pb b = { d, d + n };
	struct pbf f;
	while (pb_next(&b, &f) == 1) {
		if (f.num != 1 || f.wt != 2) {
			continue;
		}
		struct pb a = { f.d, f.d + f.n };
		struct pbf g;
		int id = -1;
		const uint8_t *val = NULL;
		uint32_t vn = 0;
		while (pb_next(&a, &g) == 1) {
			if (g.num == 1 && g.wt == 0) {
				id = (int)(int32_t)g.v;
			} else if (g.num == 2 && g.wt == 2) {
				val = g.d;
				vn = g.n;
			}
		}
		const char *nm = attr_name(id);
		if (nm != NULL) {
			printf("    %-22s ", nm);
		} else {
			printf("    attr %-17d ", id);
		}
		if (val != NULL) {
			print_attr_value(val, vn, 0);
		}
		printf("\n");
	}
	attr_done = true;
}

static void
handle_suid_event(const uint8_t *d, uint32_t n)
{
	struct pb b = { d, d + n };
	struct pbf f;
	char type[64] = "";
	struct suid found[MAX_SUIDS];
	int nf = 0;
	while (pb_next(&b, &f) == 1) {
		if (f.num == 1 && f.wt == 2) {
			snprintf(type, sizeof(type), "%.*s", (int)f.n, (const char *)f.d);
		} else if (f.num == 2 && f.wt == 2 && nf < MAX_SUIDS) {
			found[nf++] = pb_suid(f.d, f.n);
		}
	}
	for (int i = 0; i < ntypes; i++) {
		if (strcmp(types[i].type, type) == 0) {
			types[i].answered = true;
			for (int k = 0; k < nf; k++) {
				bool dup = false;
				for (int j = 0; j < types[i].n; j++) {
					dup |= suid_eq(types[i].s[j], found[k]);
				}
				if (!dup && types[i].n < MAX_SUIDS) {
					types[i].s[types[i].n++] = found[k];
				}
			}
			return;
		}
	}
	if (verbose) {
		fprintf(stderr, "suid event for unasked type '%s' (%d)\n", type, nf);
	}
}

static void
handle_stream_event(uint32_t msg_id, uint64_t ts, const uint8_t *d, uint32_t n)
{
	static const char *st[] = { "unreliable", "low", "medium", "high" };
	if (ts0 == 0) {
		ts0 = ts;
	}
	double ms = (double)(ts - ts0) / 19200.0;       /* QTimer, 19.2 MHz */
	struct pb b = { d, d + n };
	struct pbf f;

	if (msg_id == MSG_SENSOR_EVENT) {
		float v[16];
		int nv = 0;
		uint64_t status = 0;
		while (pb_next(&b, &f) == 1) {
			if (f.num == 1 && f.wt == 5 && nv < 16) {
				v[nv++] = f_float(&f);
			} else if (f.num == 1 && f.wt == 2) {      /* packed floats */
				for (uint32_t o = 0; o + 4 <= f.n && nv < 16; o += 4) {
					memcpy(&v[nv++], f.d + o, 4);
				}
			} else if (f.num == 2 && f.wt == 0) {
				status = f.v;
			}
		}
		printf("%9.1f ms  data", ms);
		for (int i = 0; i < nv; i++) {
			printf(" %10.4f", v[i]);
		}
		if (nv == 3) {
			printf("  |v| %.4f", sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]));
		}
		printf("  status %s\n", status < 4 ? st[status] : "?");
		stream_events++;
	} else if (msg_id == MSG_PROX_EVENT) {
		uint64_t type = 0, status = 0;
		int64_t raw = 0;
		while (pb_next(&b, &f) == 1) {
			if (f.num == 1 && f.wt == 0) {
				type = f.v;
			} else if (f.num == 2 && f.wt == 0) {
				raw = (int64_t)(int32_t)f.v;
			} else if (f.num == 3 && f.wt == 0) {
				status = f.v;
			}
		}
		printf("%9.1f ms  proximity %s  raw_adc %lld  status %s\n", ms, type == 1 ? "NEAR" : type == 0 ? "FAR" : "?",
		    (long long)raw, status < 4 ? st[status] : "?");
		stream_events++;
	} else if (msg_id == MSG_PHYS_CONFIG_EVENT) {
		printf("%9.1f ms  physical config:", ms);
		while (pb_next(&b, &f) == 1) {
			if (f.wt == 5) {
				printf(" f%u=%g", f.num, f_float(&f));
			} else if (f.wt == 0) {
				printf(" f%u=%llu", f.num, (unsigned long long)f.v);
			}
		}
		printf("\n");
	} else {
		printf("%9.1f ms  event msg_id %u, %u bytes\n", ms, msg_id, n);
	}
}

/* sns_client_event_msg { suid = 1; repeated event events = 2 { fixed32 msg_id = 1; fixed64 timestamp = 2; bytes payload = 3 } } */
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
			uint64_t ts = 0;
			const uint8_t *pl = (const uint8_t *)"";
			uint32_t pn = 0;
			while (pb_next(&e, &g) == 1) {
				if (g.num == 1) {
					msg_id = (uint32_t)g.v;
				} else if (g.num == 2) {
					ts = g.v;
				} else if (g.num == 3 && g.wt == 2) {
					pl = g.d;
					pn = g.n;
				}
			}
			if (verbose) {
				fprintf(stderr, "event suid %s msg_id %u ts %llu, %u bytes\n", suid_str(s), msg_id,
				    (unsigned long long)ts, pn);
			}
			if (msg_id == MSG_SUID_EVENT) {
				handle_suid_event(pl, pn);
			} else if (msg_id == MSG_ATTR_EVENT) {
				handle_attr_event(s, pl, pn);
			} else if (msg_id == MSG_ERROR_EVENT) {
				struct pb x = { pl, pl + pn };
				struct pbf h;
				uint64_t err = 0;
				while (pb_next(&x, &h) == 1) {
					if (h.num == 1 && h.wt == 0) {
						err = h.v;
					}
				}
				printf("error event from %s: sns_std_error %llu\n", suid_str(s), (unsigned long long)err);
				if (mode == MODE_ATTR && suid_eq(s, attr_target)) {
					attr_done = true;
				}
			} else if (mode == MODE_STREAM) {
				handle_stream_event(msg_id, ts, pl, pn);
			}
		}
	}
}

static void
handle_indication(const struct msg *m)
{
	uint16_t id = m_id(m), l;
	if (id != QMI_SNS_REPORT_IND && id != QMI_SNS_JUMBO_IND) {
		if (verbose) {
			fprintf(stderr, "indication 0x%x ignored\n", id);
		}
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

/* drain messages for ms milliseconds (or until *done) */
static void
pump(int ms, bool *done)
{
	struct msg m;
	double end = now_s() + ms / 1000.0;
	while (!stop && (done == NULL || !*done)) {
		int left = (int)((end - now_s()) * 1000);
		if (left <= 0) {
			break;
		}
		if (m_recv(&m, left > 200 ? 200 : left) && m.b[0] == 4) {
			handle_indication(&m);
		}
	}
}

/* one SEE request; 0 when the service accepted it */
static int
see_request(struct suid s, uint32_t msg_id, const void *payload, uint32_t pn)
{
	struct pbw req = { .n = 0 }, susp = { .n = 0 }, rq = { .n = 0 };
	w_uint(&susp, 1, 1);            /* client_proc_type: SNS_STD_PROCESSOR_APSS */
	w_uint(&susp, 2, 1);            /* delivery_type: SNS_CLIENT_DELIVERY_NO_WAKEUP */
	w_bytes(&rq, 2, payload, pn);   /* sns_std_request.payload */
	w_suid(&req, 1, s);
	w_fixed32(&req, 2, msg_id);
	w_bytes(&req, 3, susp.b, susp.n);
	w_bytes(&req, 4, rq.b, rq.n);
	if (req.err || susp.err || rq.err) {
		fprintf(stderr, "request too big\n");
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
	if (verbose) {
		hexdump("->", m.b, m.n);
	}
	if (write(fd, buf, sizeof(h) + m.n) < 0) {
		fprintf(stderr, "write: %s\n", strerror(errno));
		return -1;
	}
	double end = now_s() + 2.0;
	while (!stop) {
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
		const uint8_t *cid = tlv(&r, 0x10, &l);
		uint64_t client = 0;
		if (cid != NULL && l >= 8) {
			memcpy(&client, cid, 8);
		}
		const uint8_t *sr = tlv(&r, 0x11, &l);
		uint32_t see = 0;
		if (sr != NULL && l >= 4) {
			memcpy(&see, sr, 4);
		}
		if (verbose) {
			fprintf(stderr, "response: qmi %u/%u, client %llu, see result %u\n", qr, qe,
			    (unsigned long long)client, see);
		}
		if (qr != 0) {
			fprintf(stderr, "msg_id %u: QMI error %u\n", msg_id, qe);
			return -1;
		}
		if (see != 0) {
			fprintf(stderr, "msg_id %u: SEE result %u\n", msg_id, see);
			return -1;
		}
		return 0;
	}
	fprintf(stderr, "msg_id %u: no response from %u:%x within 2 s\n", msg_id, node, port);
	return -2;
}

static int
suid_lookup(const char *type, bool default_only)
{
	struct pbw p = { .n = 0 };
	w_bytes(&p, 1, type, (uint32_t)strlen(type));
	w_uint(&p, 2, 0);               /* register_updates */
	w_uint(&p, 3, default_only ? 1 : 0);
	struct suid lk = { SUID_LOOKUP_LOW, SUID_LOOKUP_HIGH };
	return see_request(lk, MSG_SUID_REQ, p.b, p.n);
}

static void
add_type(const char *t)
{
	if (ntypes < MAX_TYPES) {
		types[ntypes++].type = t;
	}
}

static int
cmd_info(int argc, char **argv)
{
	static const char *def[] = { "accel", "gyro", "mag", "proximity", "ambient_light", "registry",
		"pickup", "device_orient", "motion_detect", "tilt" };
	if (argc == 0) {
		for (unsigned i = 0; i < sizeof(def) / sizeof(def[0]); i++) {
			add_type(def[i]);
		}
	} else {
		for (int i = 0; i < argc; i++) {
			add_type(argv[i]);
		}
	}
	alarm(30 + 4 * ntypes * 3);

	mode = MODE_LOOKUP;
	for (int i = 0; i < ntypes && !stop; i++) {
		if (suid_lookup(types[i].type, false) == -2 && i == 0) {
			printf("SNS_CLIENT at %u:%x does not answer: check the boot log line 'service 190:1 node 5 port ..'\n",
			    node, port);
			return 1;
		}
		pump(150, NULL);        /* keep the rate low */
	}
	pump(2000, NULL);

	for (int i = 0; i < ntypes; i++) {
		printf("%-14s %s", types[i].type, !types[i].answered ? "no answer" :
		    types[i].n == 0 ? "none (no sensor of this type)" : "");
		for (int k = 0; k < types[i].n; k++) {
			printf("%s%s", k ? ", " : "", suid_str(types[i].s[k]));
		}
		printf("\n");
	}
	mode = MODE_ATTR;
	for (int i = 0; i < ntypes && !stop; i++) {
		for (int k = 0; k < types[i].n && !stop; k++) {
			printf("\n%s %s:\n", types[i].type, suid_str(types[i].s[k]));
			attr_target = types[i].s[k];
			attr_done = false;
			if (see_request(attr_target, MSG_ATTR_REQ, NULL, 0) == 0) {
				pump(2500, &attr_done);
			}
			if (!attr_done) {
				printf("    (no attributes within 2.5 s)\n");
			}
			pump(100, NULL);
		}
	}
	return 0;
}

static int
cmd_stream(const char *type, double rate, int secs)
{
	if (rate < 0 || rate > 50 || secs < 1 || secs > 60) {
		fprintf(stderr, "stream: RATE 0..50 Hz (0 = on-change), SECS 1..60\n");
		return 2;
	}
	alarm(secs + 20);
	add_type(type);
	mode = MODE_LOOKUP;
	if (suid_lookup(type, true) != 0) {
		return 1;
	}
	pump(2000, &types[0].answered);
	if (types[0].n == 0) {
		printf("%s: %s\n", type, types[0].answered ? "no sensor of this type" : "no SUID event within 2 s");
		return 1;
	}
	struct suid s = types[0].s[0];
	printf("%s: SUID %s, %s for %d s\n", type, suid_str(s), rate > 0 ? "streaming" : "on-change", secs);

	mode = MODE_STREAM;
	int r;
	if (rate > 0) {
		struct pbw p = { .n = 0 };
		w_float(&p, 1, (float)rate);
		r = see_request(s, MSG_SENSOR_CONFIG, p.b, p.n);
	} else {
		r = see_request(s, MSG_ON_CHANGE_CONFIG, NULL, 0);
	}
	if (r != 0) {
		return 1;
	}
	pump(secs * 1000, NULL);
	stop = 0;                       /* still say goodbye after ^C */
	(void)see_request(s, MSG_DISABLE, NULL, 0);
	pump(300, NULL);
	printf("%u samples\n", stream_events);
	return 0;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: snsprobe [-v] [-n NODE] [-p PORT] info [TYPE...]\n"
	    "       snsprobe [-v] [-n NODE] [-p PORT] stream TYPE RATE SECS   (RATE Hz 1..50, 0 = on-change)\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	int c;
	bool addr_given = false;
	while ((c = getopt(argc, argv, "vn:p:")) != -1) {
		switch (c) {
		case 'v':
			verbose++;
			break;
		case 'n':
			node = (uint32_t)strtoul(optarg, NULL, 0);
			addr_given = true;
			break;
		case 'p':
			port = (uint32_t)strtoul(optarg, NULL, 0);
			addr_given = true;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGALRM, on_alarm);
	signal(SIGINT, on_int);
	signal(SIGTERM, on_int);
	alarm(30);
	if (dev_open() != 0) {
		return 1;
	}
	if (!addr_given) {
		/* a kernel that records the ADSP's services knows it; otherwise the default 5:0xd */
		struct sweet_qmi_lookup l = { SVC_SNS_CLIENT, ~0u, 0, 0, 0, 0 };
		if (ioctl(fd, SWEET_QMI_LOOKUP, &l) == 0) {
			node = l.node;
			port = l.port;
		}
	}
	fprintf(stderr, "SNS_CLIENT at node %u port 0x%x\n", node, port);

	int r;
	if (argc == 0 || strcmp(argv[0], "info") == 0) {
		r = cmd_info(argc > 0 ? argc - 1 : 0, argv + 1);
	} else if (strcmp(argv[0], "stream") == 0 && argc == 4) {
		r = cmd_stream(argv[1], atof(argv[2]), atoi(argv[3]));
	} else {
		usage();
		r = 2;
	}
	close(fd);
	return r;
}
