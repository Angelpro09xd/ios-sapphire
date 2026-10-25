/*
 * qmitool: the modem's identity, SIM, network and SMS from the command line, over
 * /dev/redwood-qmiN (kernel 0220: one QRTR client port per node, QMI messages in and out).
 * Message layouts follow libqmi's data/qmi-service-{dms,uim,nas,wms}.json.
 *
 *   qmitool info                 manufacturer, model, firmware, IMEI, operating mode, number
 *   qmitool online | lowpower    DMS Set Operating Mode
 *   qmitool sim                  card / application / PIN state, ICCID, IMSI
 *   qmitool pin CODE             verify PIN1
 *   qmitool net                  registration, operator, radio, signal
 *   qmitool sms-send NUMBER TEXT one SMS (GSM 7-bit if it fits the basic alphabet, else UCS-2)
 *   qmitool sms-list             the messages stored on the SIM and in the modem
 *   qmitool sms-watch            print SMS as they arrive
 *   qmitool raw SVC MSG [HEX]    any request, TLVs given as hex bytes; prints the answer
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

struct sweet_qmi_hdr { uint32_t node, port; };
struct sweet_qmi_lookup { uint32_t svc, inst, index, node, port, found_inst; };
#define SWEET_QMI_LOOKUP        _IOWR('q', 1, struct sweet_qmi_lookup)

enum { SVC_DMS = 0x02, SVC_NAS = 0x03, SVC_WMS = 0x05, SVC_UIM = 0x0b };

struct cli { int fd; uint32_t node, port; uint16_t txn; };
struct msg { uint8_t b[0x4000]; uint32_t n; };

static int verbose;

static void
hexdump(const char *what, const uint8_t *p, uint32_t n)
{
	fprintf(stderr, "%s (%u):", what, n);
	for (uint32_t i = 0; i < n; i++) {
		fprintf(stderr, " %02x", p[i]);
	}
	fprintf(stderr, "\n");
}

static int
cli_open(struct cli *c, uint32_t svc)
{
	char path[32];
	for (int i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/redwood-qmi%d", i);
		c->fd = open(path, O_RDWR);
		if (c->fd >= 0) {
			break;
		}
		if (errno != EBUSY) {
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			return -1;
		}
	}
	if (c->fd < 0) {
		fprintf(stderr, "all /dev/redwood-qmi nodes busy\n");
		return -1;
	}
	struct sweet_qmi_lookup l = { svc, ~0u, 0, 0, 0, 0 };
	if (ioctl(c->fd, SWEET_QMI_LOOKUP, &l) != 0) {
		fprintf(stderr, "QMI service 0x%x: not announced by the modem\n", svc);
		close(c->fd);
		return -1;
	}
	c->node = l.node;
	c->port = l.port;
	c->txn = (uint16_t)(getpid() & 0xff) << 8;
	if (verbose) {
		fprintf(stderr, "service 0x%x inst 0x%x at %u:%x\n", svc, l.found_inst, l.node, l.port);
	}
	return 0;
}

static void
m_init(struct msg *m, uint16_t txn, uint16_t id)
{
	m->b[0] = 0;                    /* request */
	memcpy(m->b + 1, &txn, 2);
	memcpy(m->b + 3, &id, 2);
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

static void m_u8(struct msg *m, uint8_t t, uint8_t v) { m_tlv(m, t, &v, 1); }

/* a TLV of a received message (b = whole QMI message) */
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

/* QMI result TLV 0x02: 0 ok, else the QMI error (-1 if missing) */
static int
m_error(const struct msg *m)
{
	uint16_t l;
	const uint8_t *r = tlv(m, 0x02, &l);
	if (r == NULL || l < 4) {
		return -1;
	}
	uint16_t res, err;
	memcpy(&res, r, 2);
	memcpy(&err, r + 2, 2);
	return res == 0 ? 0 : err;
}

/* the next message from the service; 0 on timeout */
static int
m_recv(struct cli *c, struct msg *m, int ms)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(c->fd, &fds);
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	if (select(c->fd + 1, &fds, NULL, NULL, ms < 0 ? NULL : &tv) <= 0) {
		return 0;
	}
	uint8_t buf[sizeof(struct sweet_qmi_hdr) + sizeof(m->b)];
	ssize_t n = read(c->fd, buf, sizeof(buf));
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

static void (*on_indication)(struct cli *c, const struct msg *m);

/* send a request and wait for its response (indications meanwhile go to on_indication) */
static int
request(struct cli *c, struct msg *req, struct msg *resp)
{
	uint16_t txn = ++c->txn;
	memcpy(req->b + 1, &txn, 2);
	uint8_t buf[sizeof(struct sweet_qmi_hdr) + sizeof(req->b)];
	struct sweet_qmi_hdr h = { c->node, c->port };
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), req->b, req->n);
	if (verbose) {
		hexdump("->", req->b, req->n);
	}
	if (write(c->fd, buf, sizeof(h) + req->n) < 0) {
		fprintf(stderr, "write: %s\n", strerror(errno));
		return -1;
	}
	for (;;) {
		if (!m_recv(c, resp, 10000)) {
			fprintf(stderr, "message 0x%04x: no answer in 10 s\n", m_id(req));
			return -1;
		}
		uint16_t rt;
		memcpy(&rt, resp->b + 1, 2);
		if (resp->b[0] == 2 && rt == txn) {
			return m_error(resp);
		}
		if (resp->b[0] == 4 && on_indication != NULL) {
			on_indication(c, resp);
		}
	}
}

static int
simple(struct cli *c, uint16_t id, struct msg *resp)
{
	struct msg q;
	m_init(&q, 0, id);
	return request(c, &q, resp);
}

static void
print_str(const char *label, const struct msg *m, uint8_t type)
{
	uint16_t l;
	const uint8_t *v = tlv(m, type, &l);
	if (v != NULL) {
		printf("%-14s %.*s\n", label, (int)l, (const char *)v);
	}
}

/* ---- DMS ---- */

static int
cmd_info(void)
{
	struct cli c;
	struct msg r;
	if (cli_open(&c, SVC_DMS) != 0) {
		return 1;
	}
	if (simple(&c, 0x0021, &r) == 0) print_str("manufacturer", &r, 0x01);
	if (simple(&c, 0x0022, &r) == 0) print_str("model", &r, 0x01);
	if (simple(&c, 0x0023, &r) == 0) print_str("firmware", &r, 0x01);
	int e = simple(&c, 0x0025, &r);
	if (e == 0) {
		print_str("IMEI", &r, 0x11);
		print_str("IMEI SV", &r, 0x13);
		print_str("ESN", &r, 0x10);
		print_str("MEID", &r, 0x12);
	} else {
		printf("IDs: QMI error %d\n", e);
	}
	e = simple(&c, 0x0024, &r);
	if (e == 0) print_str("number", &r, 0x01);
	else printf("%-14s (QMI error %d)\n", "number", e);
	if (simple(&c, 0x002d, &r) == 0) {
		uint16_t l;
		const uint8_t *v = tlv(&r, 0x01, &l);
		static const char *modes[] = { "online", "low power", "factory test", "offline", "resetting",
			"shutting down", "persistent low power", "mode-only low power" };
		if (v != NULL) {
			printf("%-14s %s\n", "mode", v[0] < 8 ? modes[v[0]] : "?");
		}
	}
	close(c.fd);
	return 0;
}

static int
cmd_mode(uint8_t mode)
{
	struct cli c;
	struct msg q, r;
	if (cli_open(&c, SVC_DMS) != 0) {
		return 1;
	}
	m_init(&q, 0, 0x002e);
	m_u8(&q, 0x01, mode);
	int e = request(&c, &q, &r);
	printf("set operating mode %u: %s (%d)\n", mode, e == 0 ? "ok" : "QMI error", e);
	close(c.fd);
	return e != 0;
}

/* ---- UIM ---- */

static const char *
app_state(uint8_t s)
{
	static const char *n[] = { "unknown", "detected", "PIN1 or UPIN needed", "PUK1 or PUK needed",
		"personalization check", "PIN1 blocked", "illegal", "ready" };
	return s < 8 ? n[s] : "?";
}

static const char *
pin_state(uint8_t s)
{
	static const char *n[] = { "not initialized", "enabled, not verified", "enabled, verified",
		"disabled", "blocked", "permanently blocked" };
	return s < 6 ? n[s] : "?";
}

/* BCD with swapped nibbles (ICCID, IMSI): digits until an F nibble */
static void
bcd(char *out, const uint8_t *p, int n, int skip_first_nibble)
{
	int k = 0;
	for (int i = 0; i < n; i++) {
		uint8_t lo = p[i] & 0xf, hi = p[i] >> 4;
		if (!(i == 0 && skip_first_nibble) && lo <= 9) out[k++] = (char)('0' + lo);
		if (hi <= 9) out[k++] = (char)('0' + hi);
		else if (hi == 0xf) break;
	}
	out[k] = 0;
}

static int
uim_read(struct cli *c, uint8_t session, uint16_t file, const uint8_t *path, uint8_t plen, uint8_t *out, uint16_t *olen)
{
	struct msg q, r;
	m_init(&q, 0, 0x0020);
	uint8_t s[2] = { session, 0 };
	m_tlv(&q, 0x01, s, 2);
	uint8_t f[3 + 8];
	memcpy(f, &file, 2);
	f[2] = plen;
	memcpy(f + 3, path, plen);
	m_tlv(&q, 0x02, f, (uint16_t)(3 + plen));
	uint8_t ri[4] = { 0, 0, 0, 0 };          /* offset 0, length 0 = all */
	m_tlv(&q, 0x03, ri, 4);
	int e = request(c, &q, &r);
	if (e != 0) {
		return e;
	}
	uint16_t l;
	const uint8_t *v = tlv(&r, 0x11, &l);
	if (v == NULL || l < 2) {
		return -1;
	}
	uint16_t n;
	memcpy(&n, v, 2);
	if (n > *olen || n + 2 > l) {
		return -1;
	}
	memcpy(out, v + 2, n);
	*olen = n;
	return 0;
}

static int
cmd_sim(void)
{
	struct cli c;
	struct msg r;
	if (cli_open(&c, SVC_UIM) != 0) {
		return 1;
	}
	int e = simple(&c, 0x002f, &r);
	uint16_t l;
	const uint8_t *v = e == 0 ? tlv(&r, 0x10, &l) : NULL;
	if (v == NULL || l < 9) {
		printf("card status: QMI error %d\n", e);
		close(c.fd);
		return 1;
	}
	static const char *card[] = { "absent", "present", "error" };
	static const char *apptype[] = { "unknown", "SIM", "USIM", "RUIM", "CSIM", "ISIM" };
	const uint8_t *p = v + 8, *end = v + l;
	uint8_t ncards = *p++;
	for (int i = 0; i < ncards && p + 6 <= end; i++) {
		printf("card %d: %s, UPIN %s (%u tries), error %u\n", i, p[0] < 3 ? card[p[0]] : "?",
		    pin_state(p[1]), p[2], p[4]);
		uint8_t napps = p[5];
		p += 6;
		for (int a = 0; a < napps && p + 7 <= end; a++) {
			uint8_t type = p[0], st = p[1], aidlen = p[6];
			const uint8_t *q = p + 7 + aidlen;
			if (q + 7 > end) break;
			printf("  app %d: %s, %s; PIN1 %s (%u tries, PUK %u)\n", a, type < 6 ? apptype[type] : "?",
			    app_state(st), pin_state(q[1]), q[2], q[3]);
			p = q + 7;
		}
	}
	uint8_t buf[64];
	uint16_t n = sizeof(buf);
	static const uint8_t mf[] = { 0x00, 0x3f }, adf[] = { 0x00, 0x3f, 0xff, 0x7f };
	char s[40];
	if ((e = uim_read(&c, 6 /* card slot 1 */, 0x2fe2, mf, 2, buf, &n)) == 0) {
		bcd(s, buf, n, 0);
		printf("ICCID          %s\n", s);
	} else {
		printf("ICCID          (QMI error %d)\n", e);
	}
	n = sizeof(buf);
	if ((e = uim_read(&c, 0 /* primary GW */, 0x6f07, adf, 4, buf, &n)) == 0 && n >= 2) {
		bcd(s, buf + 1, buf[0], 1);
		printf("IMSI           %s\n", s);
	} else {
		printf("IMSI           (QMI error %d)\n", e);
	}
	close(c.fd);
	return 0;
}

static int
cmd_pin(const char *code)
{
	struct cli c;
	struct msg q, r;
	if (cli_open(&c, SVC_UIM) != 0) {
		return 1;
	}
	m_init(&q, 0, 0x0026);
	uint8_t s[2] = { 0, 0 };
	m_tlv(&q, 0x01, s, 2);
	uint8_t p[2 + 16];
	size_t n = strlen(code);
	if (n > 16) n = 16;
	p[0] = 1;                               /* PIN1 */
	p[1] = (uint8_t)n;
	memcpy(p + 2, code, n);
	m_tlv(&q, 0x02, p, (uint16_t)(2 + n));
	int e = request(&c, &q, &r);
	uint16_t l;
	const uint8_t *t = tlv(&r, 0x10, &l);
	printf("verify PIN1: %s (%d)", e == 0 ? "ok" : "refused", e);
	if (t != NULL && l >= 2) printf(", %u tries left", t[0]);
	printf("\n");
	close(c.fd);
	return e != 0;
}

/* ---- NAS ---- */

static int
cmd_net(void)
{
	struct cli c;
	struct msg r;
	if (cli_open(&c, SVC_NAS) != 0) {
		return 1;
	}
	int e = simple(&c, 0x0024, &r);
	uint16_t l;
	const uint8_t *v;
	if (e == 0 && (v = tlv(&r, 0x01, &l)) != NULL && l >= 5) {
		static const char *reg[] = { "not registered", "registered", "searching", "denied", "unknown" };
		static const char *rif[] = { "none", "cdma1x", "evdo", "amps", "gsm", "umts", "lte", "?", "td-scdma",
			"?", "?", "?", "nr5g" };
		printf("registration   %s (CS %s, PS %s)\n", v[0] < 5 ? reg[v[0]] : "?",
		    v[1] == 1 ? "attached" : "detached", v[2] == 1 ? "attached" : "detached");
		printf("radio         ");
		for (int i = 0; i < v[4] && 5 + i < l; i++) {
			printf(" %s", v[5 + i] < 13 ? rif[v[5 + i]] : "?");
		}
		printf("\n");
		if ((v = tlv(&r, 0x12, &l)) != NULL && l >= 5) {
			uint16_t mcc, mnc;
			memcpy(&mcc, v, 2);
			memcpy(&mnc, v + 2, 2);
			printf("operator       %03u-%02u %.*s\n", mcc, mnc, (int)v[4], (const char *)v + 5);
		}
	} else {
		printf("serving system: QMI error %d\n", e);
	}
	if (simple(&c, 0x004f, &r) == 0) {
		if ((v = tlv(&r, 0x14, &l)) != NULL && l >= 6) {
			int16_t rsrp, snr;
			memcpy(&rsrp, v + 2, 2);
			memcpy(&snr, v + 4, 2);
			printf("LTE signal     RSSI %d dBm, RSRQ %d dB, RSRP %d dBm, SNR %.1f dB\n", (int8_t)v[0],
			    (int8_t)v[1], rsrp, snr / 10.0);
		}
		if ((v = tlv(&r, 0x13, &l)) != NULL && l >= 4) {
			int16_t ecio;
			memcpy(&ecio, v + 1, 2);
			printf("WCDMA signal   RSSI %d dBm, ECIO %.1f dB\n", (int8_t)v[0], ecio / -10.0);
		}
		if ((v = tlv(&r, 0x12, &l)) != NULL && l >= 1) {
			printf("GSM signal     RSSI %d dBm\n", (int8_t)v[0]);
		}
	}
	close(c.fd);
	return 0;
}

/* ---- WMS / SMS PDUs (3GPP TS 23.040, 23.038) ---- */

static const uint16_t gsm7[128] = {
	'@', 0xa3, '$', 0xa5, 0xe8, 0xe9, 0xf9, 0xec, 0xf2, 0xc7, '\n', 0xd8, 0xf8, '\r', 0xc5, 0xe5,
	0x394, '_', 0x3a6, 0x393, 0x39b, 0x3a9, 0x3a0, 0x3a8, 0x3a3, 0x398, 0x39e, 0x1b, 0xc6, 0xe6, 0xdf, 0xc9,
	' ', '!', '"', '#', 0xa4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
	0xa1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xc4, 0xd6, 0xd1, 0xdc, 0xa7,
	0xbf, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xe4, 0xf6, 0xf1, 0xfc, 0xe0,
};

static void
put_utf8(uint32_t u)
{
	if (u < 0x80) putchar((int)u);
	else if (u < 0x800) { putchar(0xc0 | (int)(u >> 6)); putchar(0x80 | (int)(u & 0x3f)); }
	else { putchar(0xe0 | (int)(u >> 12)); putchar(0x80 | (int)((u >> 6) & 0x3f)); putchar(0x80 | (int)(u & 0x3f)); }
}

/* UTF-8 to code points (BMP); returns the count */
static int
utf8_decode(const char *s, uint16_t *out, int max)
{
	int n = 0;
	const uint8_t *p = (const uint8_t *)s;
	while (*p && n < max) {
		uint32_t u;
		if (*p < 0x80) u = *p++;
		else if ((*p & 0xe0) == 0xc0 && p[1]) { u = ((p[0] & 0x1fu) << 6) | (p[1] & 0x3fu); p += 2; }
		else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) { u = ((p[0] & 0xfu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu); p += 3; }
		else { u = '?'; p++; while ((*p & 0xc0) == 0x80) p++; }
		out[n++] = (uint16_t)u;
	}
	return n;
}

static int
gsm7_code(uint16_t u)
{
	for (int i = 0; i < 128; i++) {
		if (gsm7[i] == u && i != 0x1b) return i;
	}
	return -1;
}

/* address field: length in digits, type, swapped BCD */
static int
put_address(uint8_t *p, const char *num)
{
	int intl = num[0] == '+';
	if (intl) num++;
	int nd = 0;
	p[1] = intl ? 0x91 : 0x81;
	for (const char *s = num; *s; s++) {
		if (*s < '0' || *s > '9') continue;
		int k = 2 + nd / 2;
		if (nd % 2 == 0) p[k] = 0xf0 | (uint8_t)(*s - '0');
		else p[k] = (uint8_t)((p[k] & 0x0f) | ((*s - '0') << 4));
		nd++;
	}
	p[0] = (uint8_t)nd;
	return 2 + (nd + 1) / 2;
}

static int
cmd_sms_send(const char *num, const char *text)
{
	uint16_t u[200];
	int n = utf8_decode(text, u, 200), seven = 1;
	for (int i = 0; i < n; i++) {
		if (gsm7_code(u[i]) < 0) seven = 0;
	}
	if ((seven && n > 160) || (!seven && n > 70)) {
		fprintf(stderr, "too long for one SMS (%d characters, at most %d)\n", n, seven ? 160 : 70);
		return 1;
	}
	uint8_t pdu[200];
	int k = 0;
	pdu[k++] = 0x00;                        /* SMSC: the SIM's */
	pdu[k++] = 0x11;                        /* SMS-SUBMIT, relative validity period */
	pdu[k++] = 0x00;                        /* message reference: the modem's */
	k += put_address(pdu + k, num);
	pdu[k++] = 0x00;                        /* PID */
	pdu[k++] = seven ? 0x00 : 0x08;         /* DCS: GSM 7-bit / UCS-2 */
	pdu[k++] = 0xaa;                        /* valid 4 days */
	if (seven) {
		pdu[k++] = (uint8_t)n;
		uint32_t acc = 0;
		int bits = 0;
		for (int i = 0; i < n; i++) {
			acc |= (uint32_t)gsm7_code(u[i]) << bits;
			bits += 7;
			while (bits >= 8) {
				pdu[k++] = (uint8_t)acc;
				acc >>= 8;
				bits -= 8;
			}
		}
		if (bits > 0) pdu[k++] = (uint8_t)acc;
	} else {
		pdu[k++] = (uint8_t)(2 * n);
		for (int i = 0; i < n; i++) {
			pdu[k++] = (uint8_t)(u[i] >> 8);
			pdu[k++] = (uint8_t)u[i];
		}
	}
	struct cli c;
	struct msg q, r;
	if (cli_open(&c, SVC_WMS) != 0) {
		return 1;
	}
	m_init(&q, 0, 0x0020);
	uint8_t raw[3 + sizeof(pdu)];
	raw[0] = 6;                             /* GSM/WCDMA point-to-point */
	uint16_t len = (uint16_t)k;
	memcpy(raw + 1, &len, 2);
	memcpy(raw + 3, pdu, (size_t)k);
	m_tlv(&q, 0x01, raw, (uint16_t)(3 + k));
	int e = request(&c, &q, &r);
	uint16_t l;
	const uint8_t *v = tlv(&r, 0x01, &l);
	if (e == 0) {
		uint16_t id = 0;
		if (v != NULL && l >= 2) memcpy(&id, v, 2);
		printf("sent (message reference %u)\n", id);
	} else {
		v = tlv(&r, 0x10, &l);
		printf("not sent: QMI error %d", e);
		if (v != NULL && l >= 2) {
			uint16_t cause;
			memcpy(&cause, v, 2);
			printf(", network cause %u", cause);
		}
		printf("\n");
	}
	close(c.fd);
	return e != 0;
}

static void
print_address(const uint8_t *p)
{
	int nd = p[0];
	if ((p[1] & 0x70) == 0x50) {            /* alphanumeric sender: GSM 7-bit */
		int chars = nd * 4 / 7;
		uint32_t acc = 0;
		int bits = 0, j = 2;
		for (int i = 0; i < chars; i++) {
			while (bits < 7) { acc |= (uint32_t)p[j++] << bits; bits += 8; }
			put_utf8(gsm7[acc & 0x7f]);
			acc >>= 7;
			bits -= 7;
		}
		return;
	}
	if (p[1] == 0x91) putchar('+');
	for (int i = 0; i < nd; i++) {
		uint8_t b = p[2 + i / 2];
		putchar('0' + ((i % 2) ? b >> 4 : b & 0xf));
	}
}

/* an SMS-DELIVER (with its SMSC prefix): sender, time, text */
static void
print_deliver(const uint8_t *p, int n)
{
	int k = 1 + p[0];                       /* past the SMSC */
	if (k >= n) return;
	uint8_t first = p[k++];
	if ((first & 3) != 0) {
		printf("(not an SMS-DELIVER: first octet %02x)\n", first);
		return;
	}
	printf("from ");
	print_address(p + k);
	k += 2 + (p[k] + 1) / 2;
	uint8_t dcs = p[k + 1];
	k += 2;
	const uint8_t *t = p + k;
	printf("  20%x%x-%x%x-%x%x %x%x:%x%x\n", t[0] & 0xf, t[0] >> 4, t[1] & 0xf, t[1] >> 4, t[2] & 0xf, t[2] >> 4,
	    t[3] & 0xf, t[3] >> 4, t[4] & 0xf, t[4] >> 4);
	k += 7;
	int udl = p[k++];
	int udh = (first & 0x40) ? p[k] + 1 : 0;
	int alphabet = (dcs & 0xc0) == 0 ? (dcs >> 2) & 3 : (dcs & 0xf0) == 0xf0 ? (dcs >> 2) & 1 : 0;
	printf("  ");
	if (alphabet == 0) {
		int skip = udh ? (udh * 8 + 6) / 7 : 0;
		uint32_t acc = 0;
		int bits = 0, j = k, esc = 0;
		for (int i = 0; i < udl && j <= n; i++) {
			while (bits < 7 && j < n) { acc |= (uint32_t)p[j++] << bits; bits += 8; }
			uint8_t ch = acc & 0x7f;
			acc >>= 7;
			bits -= 7;
			if (i < skip) continue;
			if (esc) {
				static const char ext[] = { 0x0a, '\f', 0x14, '^', 0x28, '{', 0x29, '}', 0x2f, '\\', 0x3c, '[',
					0x3d, '~', 0x3e, ']', 0x40, '|', 0 };
				int found = 0;
				for (int e = 0; ext[e]; e += 2) if (ext[e] == ch) { putchar(ext[e + 1]); found = 1; }
				if (ch == 0x65) { put_utf8(0x20ac); found = 1; }
				if (!found) putchar('?');
				esc = 0;
			} else if (ch == 0x1b) {
				esc = 1;
			} else {
				put_utf8(gsm7[ch]);
			}
		}
	} else if (alphabet == 2) {
		for (int i = k + udh; i + 1 < k + udl && i + 1 < n; i += 2) {
			put_utf8((uint32_t)(p[i] << 8 | p[i + 1]));
		}
	} else {
		printf("(8-bit data, %d bytes)", udl);
	}
	printf("\n");
}

static int
wms_read(struct cli *c, uint8_t storage, uint32_t index)
{
	struct msg q, r;
	m_init(&q, 0, 0x0022);
	uint8_t s[5] = { storage };
	memcpy(s + 1, &index, 4);
	m_tlv(&q, 0x01, s, 5);
	m_u8(&q, 0x10, 1);                      /* message mode GW */
	int e = request(c, &q, &r);
	uint16_t l;
	const uint8_t *v = tlv(&r, 0x01, &l);
	if (e != 0 || v == NULL || l < 4) {
		printf("[%s %u] read: QMI error %d\n", storage ? "modem" : "SIM", index, e);
		return e;
	}
	uint16_t n;
	memcpy(&n, v + 2, 2);
	static const char *tags[] = { "read", "unread", "sent", "unsent" };
	printf("[%s %u, %s] ", storage ? "modem" : "SIM", index, v[0] < 4 ? tags[v[0]] : "?");
	if (v[0] >= 2) {
		printf("(outgoing, %u bytes)\n", n);
	} else {
		print_deliver(v + 4, n < l - 4 ? n : l - 4);
	}
	return 0;
}

static int
cmd_sms_list(void)
{
	struct cli c;
	if (cli_open(&c, SVC_WMS) != 0) {
		return 1;
	}
	for (uint8_t storage = 0; storage < 2; storage++) {
		struct msg q, r;
		m_init(&q, 0, 0x0031);
		m_u8(&q, 0x01, storage);
		m_u8(&q, 0x11, 1);                      /* message mode GW */
		int e = request(&c, &q, &r);
		uint16_t l;
		const uint8_t *v = tlv(&r, 0x01, &l);
		if (e != 0 || v == NULL || l < 4) {
			printf("%s: list: QMI error %d\n", storage ? "modem" : "SIM", e);
			continue;
		}
		uint32_t count;
		memcpy(&count, v, 4);
		printf("%s: %u messages\n", storage ? "modem" : "SIM", count);
		for (uint32_t i = 0; i < count && 4 + i * 5 + 5 <= l; i++) {
			uint32_t idx;
			memcpy(&idx, v + 4 + i * 5, 4);
			wms_read(&c, storage, idx);
		}
	}
	close(c.fd);
	return 0;
}

static void
wms_indication(struct cli *c, const struct msg *m)
{
	if (m_id(m) != 0x0001) {
		return;
	}
	uint16_t l;
	const uint8_t *v = tlv(m, 0x10, &l);  /* MT message stored */
	if (v != NULL && l >= 5) {
		uint32_t idx;
		memcpy(&idx, v + 1, 4);
		printf("new SMS:\n");
		void (*saved)(struct cli *, const struct msg *) = on_indication;
		on_indication = NULL;
		wms_read(c, v[0], idx);
		on_indication = saved;
	}
	v = tlv(m, 0x11, &l);                   /* transfer-route MT message: ack u8, txn u32, format u8, data */
	if (v != NULL && l >= 8) {
		uint16_t n;
		memcpy(&n, v + 6, 2);
		printf("new SMS (not stored):\n  ");
		print_deliver(v + 8, n < l - 8 ? n : l - 8);
	}
	fflush(stdout);
}

static int
cmd_sms_watch(void)
{
	struct cli c;
	struct msg q, r;
	if (cli_open(&c, SVC_WMS) != 0) {
		return 1;
	}
	m_init(&q, 0, 0x0001);
	m_u8(&q, 0x10, 1);                      /* new MT message indicator */
	int e = request(&c, &q, &r);
	printf("event report: %s (%d); waiting for SMS (Ctrl-C to stop)\n", e == 0 ? "on" : "QMI error", e);
	fflush(stdout);
	on_indication = wms_indication;
	for (;;) {
		if (m_recv(&c, &r, -1) && r.b[0] == 4) {
			wms_indication(&c, &r);
		}
	}
}

/* ---- raw ---- */

static int
cmd_raw(int argc, char **argv)
{
	struct cli c;
	struct msg q, r;
	if (cli_open(&c, (uint32_t)strtoul(argv[0], NULL, 0)) != 0) {
		return 1;
	}
	m_init(&q, 0, (uint16_t)strtoul(argv[1], NULL, 0));
	for (int i = 2; i < argc; i++) {
		for (const char *s = argv[i]; s[0] && s[1]; s += 2) {
			char h[3] = { s[0], s[1], 0 };
			q.b[q.n++] = (uint8_t)strtoul(h, NULL, 16);
		}
	}
	uint16_t total = (uint16_t)(q.n - 7);
	memcpy(q.b + 5, &total, 2);
	int e = request(&c, &q, &r);
	printf("result %d\n", e);
	hexdump("response", r.b, r.n);
	close(c.fd);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "-v") == 0) {
		verbose = 1;
		argv++;
		argc--;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: qmitool [-v] info | online | lowpower | sim | pin CODE | net |\n"
		    "               sms-send NUMBER TEXT | sms-list | sms-watch | raw SVC MSG [HEX...]\n");
		return 2;
	}
	const char *cmd = argv[1];
	if (!strcmp(cmd, "info")) return cmd_info();
	if (!strcmp(cmd, "online")) return cmd_mode(0);
	if (!strcmp(cmd, "lowpower")) return cmd_mode(1);
	if (!strcmp(cmd, "sim")) return cmd_sim();
	if (!strcmp(cmd, "pin") && argc > 2) return cmd_pin(argv[2]);
	if (!strcmp(cmd, "net")) return cmd_net();
	if (!strcmp(cmd, "sms-send") && argc > 3) return cmd_sms_send(argv[2], argv[3]);
	if (!strcmp(cmd, "sms-list")) return cmd_sms_list();
	if (!strcmp(cmd, "sms-watch")) return cmd_sms_watch();
	if (!strcmp(cmd, "raw") && argc > 3) return cmd_raw(argc - 2, argv + 2);
	fprintf(stderr, "unknown command %s\n", cmd);
	return 2;
}
