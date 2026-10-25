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
 *   qmitool datacall [APN]       IPv4 data call over the IPA (kernel 0225+ with ipa=1): WDA data
 *                                format QMAP on the embedded endpoint, WDS mux 1, start network;
 *                                prints the address and stays (the call ends with this client)
 *   qmitool ping DST [SRC]       ICMP echo through the call (kern.sweet_ipa_tx / sweet_ipa_rx)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <time.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>

/* installed next to qmitool (/redwood/bin) or, by hand, in /var/root */
#define PDPNET (access("/redwood/bin/pdpnet", X_OK) == 0 ? "/redwood/bin/pdpnet" : "/var/root/pdpnet")

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
	uint16_t gw_primary;
	memcpy(&gw_primary, v, 2);
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
			if (gw_primary == 0xffff && (type == 1 || type == 2) && st == 1) {
				/*
				 * Nobody chose this application for the GW primary session (Android's RIL does
				 * at boot): UIM Change Provisioning Session, activate, slot i+1 + its AID.
				 */
				struct msg cq, cr;
				m_init(&cq, 0, 0x0038);
				uint8_t sc[2] = { 0, 1 };
				m_tlv(&cq, 0x01, sc, 2);
				uint8_t ai[2 + 32];
				ai[0] = (uint8_t)(i + 1);
				ai[1] = aidlen > 32 ? 32 : aidlen;
				memcpy(ai + 2, p + 7, ai[1]);
				m_tlv(&cq, 0x10, ai, (uint16_t)(2 + ai[1]));
				int pe = request(&c, &cq, &cr);
				printf("  -> activated as the primary session: %s (%d); run qmitool sim again\n",
				    pe == 0 ? "ok" : "QMI error", pe);
				gw_primary = 0;
			}
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
		static const char *rif[] = { "none", "cdma1x", "evdo", "amps", "gsm", "umts", "?", "?", "lte",
			"td-scdma", "?", "?", "nr5g" };     /* QmiNasRadioInterface */
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

/*
 * an SMS-DELIVER: sender, time, text. Stored messages may start with the SMSC address,
 * transfer-route ones do not (the SMSC comes in its own TLV): an SMSC field is a length of
 * 1..12 octets followed by a type of number (0x8x/0x9x), never a TPDU's first octet + OA length.
 */
static void
print_deliver(const uint8_t *p, int n)
{
	int k = (n > 2 && p[0] >= 1 && p[0] <= 12 && (p[1] & 0x80)) ? 1 + p[0] : 0;
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
		if (v[0] == 0) {
			/* "ack required": WMS Send Ack {transaction, protocol WCDMA, success}, else the network sends it again */
			struct msg q, r;
			m_init(&q, 0, 0x0037);
			uint8_t a[6];
			memcpy(a, v + 1, 4);
			a[4] = 1;
			a[5] = 1;
			m_tlv(&q, 0x01, a, 6);
			void (*saved)(struct cli *, const struct msg *) = on_indication;
			on_indication = NULL;
			int e = request(c, &q, &r);
			on_indication = saved;
			printf("  (acknowledged: %s)\n", e == 0 ? "ok" : "QMI error");
		}
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

/* ---- data call (libqmi qmi-service-wda.json / qmi-service-wds.json) */

static void m_u32(struct msg *m, uint8_t t, uint32_t v) { m_tlv(m, t, &v, 4); }

static uint32_t
tlv_u32(const struct msg *m, uint8_t type)
{
	uint16_t l;
	const uint8_t *v = tlv(m, type, &l);
	uint32_t x = 0;
	if (v != NULL && l >= 4) {
		memcpy(&x, v, 4);
	}
	return x;
}

static void
print_ip(const char *label, uint32_t a)
{
	printf("%-14s %u.%u.%u.%u\n", label, a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

/*
 * pdp_ip0 (kernel 0232): the call's address on the interface (point to point to the gateway) and,
 * when there is none yet, the default route through it, as configd would do for a cellular service.
 */
static void
sin_set(struct sockaddr_in *a, uint32_t ip)
{
	memset(a, 0, sizeof(*a));
	a->sin_len = sizeof(*a);
	a->sin_family = AF_INET;
	a->sin_addr.s_addr = htonl(ip);
}

/* <net/route.h> is not in the iOS SDK: the routing socket's message, as xnu has it */
struct rt_metrics {
	uint32_t rmx_locks, rmx_mtu, rmx_hopcount;
	int32_t rmx_expire;
	uint32_t rmx_recvpipe, rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent, rmx_state, rmx_filler[3];
};
struct rt_msghdr {
	u_short rtm_msglen;
	u_char rtm_version, rtm_type;
	u_short rtm_index;
	int rtm_flags, rtm_addrs;
	pid_t rtm_pid;
	int rtm_seq, rtm_errno, rtm_use;
	uint32_t rtm_inits;
	struct rt_metrics rtm_rmx;
};
#define RTM_VERSION     5
#define RTM_ADD         0x1
#define RTF_UP          0x1
#define RTF_GATEWAY     0x2
#define RTF_STATIC      0x800
#define RTA_DST         0x1
#define RTA_GATEWAY     0x2
#define RTA_NETMASK     0x4

struct ifaliasreq4 {
	char ifra_name[IFNAMSIZ];
	struct sockaddr_in ifra_addr, ifra_broadaddr, ifra_mask;
};

static int
pdp_config(uint32_t ip, uint32_t gw, uint32_t mtu)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("pdp_ip0: socket");
		return -1;
	}
	struct ifaliasreq4 ifra;
	memset(&ifra, 0, sizeof(ifra));
	strlcpy(ifra.ifra_name, "pdp_ip0", sizeof(ifra.ifra_name));
	sin_set(&ifra.ifra_addr, ip);
	sin_set(&ifra.ifra_broadaddr, gw != 0 ? gw : ip);
	sin_set(&ifra.ifra_mask, 0xffffffffu);
	if (ioctl(s, SIOCAIFADDR, &ifra) < 0) {
		perror("pdp_ip0: SIOCAIFADDR");
		close(s);
		return -1;
	}
	if (mtu >= 576 && mtu <= 1500) {
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		strlcpy(ifr.ifr_name, "pdp_ip0", sizeof(ifr.ifr_name));
		ifr.ifr_mtu = (int)mtu;
		(void)ioctl(s, SIOCSIFMTU, &ifr);
	}
	close(s);
	printf("pdp_ip0        %u.%u.%u.%u -> %u.%u.%u.%u\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
	    gw >> 24, (gw >> 16) & 255, (gw >> 8) & 255, gw & 255);

	int r = socket(PF_ROUTE, SOCK_RAW, 0);
	if (r < 0) {
		perror("route socket");
		return -1;
	}
	struct {
		struct rt_msghdr h;
		struct sockaddr_in dst, gw, mask;
	} m;
	memset(&m, 0, sizeof(m));
	m.h.rtm_msglen = sizeof(m);
	m.h.rtm_version = RTM_VERSION;
	m.h.rtm_type = RTM_ADD;
	m.h.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
	m.h.rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
	m.h.rtm_pid = getpid();
	m.h.rtm_seq = 1;
	sin_set(&m.dst, 0);
	sin_set(&m.gw, gw != 0 ? gw : ip);
	sin_set(&m.mask, 0);
	if (write(r, &m, sizeof(m)) < 0) {
		printf("default route  %s\n", errno == EEXIST ? "already there (left alone)" : strerror(errno));
	} else {
		printf("default route  via pdp_ip0\n");
	}
	close(r);
	return 0;
}

static int
cmd_pdp_up(void)
{
	unsigned a[4], g[4];
	FILE *f = fopen("/var/tmp/datacall.ip", "r");
	if (f == NULL) {
		printf("no data call (/var/tmp/datacall.ip)\n");
		return 1;
	}
	int n = fscanf(f, "%u.%u.%u.%u %u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3], &g[0], &g[1], &g[2], &g[3]);
	fclose(f);
	if (n < 4) {
		return 1;
	}
	uint32_t ip = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
	uint32_t gw = n == 8 ? (g[0] << 24 | g[1] << 16 | g[2] << 8 | g[3]) : 0;
	return pdp_config(ip, gw, 0) ? 1 : 0;
}

/* a plain TCP connection through pdp_ip0 (IP_BOUND_IF): the kernel's own stack over the data call */
static int
cmd_http(const char *ip, const char *host, const char *path)
{
	struct sockaddr_in a;
	if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
		return 1;
	}
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(80);
	memset(a.sin_zero, 0, sizeof(a.sin_zero));
	int s = socket(AF_INET, SOCK_STREAM, 0);
	unsigned idx = if_nametoindex("pdp_ip0");
	if (s < 0 || idx == 0) {
		printf("no socket or no pdp_ip0 (%u)\n", idx);
		return 1;
	}
	if (getenv("ANY_IF") == NULL && setsockopt(s, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx)) < 0) {
		perror("IP_BOUND_IF");
	}
	struct timeval tv = { 15, 0 };
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct timeval t0, t1;
	gettimeofday(&t0, NULL);
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		perror("connect");
		close(s);
		return 1;
	}
	gettimeofday(&t1, NULL);
	printf("connected in %ld ms\n", (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000));
	char req[512];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: qmitool\r\n\r\n", path, host);
	if (write(s, req, (size_t)n) != n) {
		perror("write");
	}
	char buf[4096];
	long total = 0;
	ssize_t r;
	while ((r = read(s, buf, sizeof(buf))) > 0) {
		if (total < 600) {
			fwrite(buf, 1, (size_t)(r < 600 - total ? r : 600 - total), stdout);
		}
		total += r;
	}
	gettimeofday(&t1, NULL);
	printf("\n-- %ld bytes in %ld ms%s\n", total, (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000),
	    r < 0 ? " (read error)" : "");
	close(s);
	return 0;
}

static int
cmd_datacall(const char *apn)
{
	struct cli wda, wds;
	struct msg q, r;
	uint32_t ep[2] = { 4, 1 };              /* embedded endpoint (the IPA), interface 1 */
	int e;

	struct cli dpm;
	if (cli_open(&dpm, 0x2f) != 0 || cli_open(&wda, 0x1a) != 0 || cli_open(&wds, 0x01) != 0) {
		return 1;
	}
	/*
	 * DPM Open Port first (netmgr's qmi_dpm_enabled on IPA SoCs): the embedded hardware data port,
	 * IPA consumer pipe 1 (AP -> modem) then producer pipe 9 (modem -> AP), as rmnet_ipa's RMNET_IOCTL_GET_EP_PAIR gives netmgr. Without it WDA and WDS answer
	 * INVALID_OPERATION. The client stays open with the call.
	 */
	struct { uint8_t n; uint32_t type, iface, rx, tx; } __attribute__((packed)) hw = { 1, 4, 1, 1, 9 };  /* consumer 1 (AP->IPA), producer 9 */
	if (getenv("DPM_RX") != NULL) {
		hw.rx = (uint32_t)strtoul(getenv("DPM_RX"), NULL, 0);
	}
	if (getenv("DPM_TX") != NULL) {
		hw.tx = (uint32_t)strtoul(getenv("DPM_TX"), NULL, 0);
	}
	m_init(&q, 0, 0x0020);
	m_tlv(&q, 0x11, &hw, sizeof(hw));
	e = request(&dpm, &q, &r);
	printf("dpm port       %s (%d)\n", e ? "refused" : "open", e);
	m_init(&q, 0, 0x0020);                  /* WDA Set Data Format */
	m_u32(&q, 0x11, 2);                     /* link layer: raw IP */
	m_u32(&q, 0x12, 5);                     /* uplink aggregation: QMAP */
	m_u32(&q, 0x13, 5);                     /* downlink aggregation: QMAP */
	m_u32(&q, 0x15, 32);
	m_u32(&q, 0x16, 32768);
	m_tlv(&q, 0x17, ep, sizeof(ep));
	e = request(&wda, &q, &r);
	printf("data format    %s (link %u, ul %u, dl %u)\n", e ? "refused" : "set", tlv_u32(&r, 0x11), tlv_u32(&r, 0x12),
	    tlv_u32(&r, 0x13));
	if (e) {
		hexdump("answer", r.b, r.n);
	}
	m_init(&q, 0, 0x00a2);                  /* WDS Bind Mux Data Port */
	m_tlv(&q, 0x10, ep, sizeof(ep));
	m_u8(&q, 0x11, 1);                      /* mux id 1 */
	if (getenv("MUX_TETHERED") != NULL) {
		m_u32(&q, 0x13, 1);             /* client type: tethered */
	}
	e = request(&wds, &q, &r);
	printf("mux 1          %s (%d)\n", e ? "refused" : "bound", e);
	m_init(&q, 0, 0x004d);                  /* WDS Set IP Family */
	m_u8(&q, 0x01, 4);
	(void)request(&wds, &q, &r);
	m_init(&q, 0, 0x0020);                  /* WDS Start Network */
	if (apn != NULL) {
		m_tlv(&q, 0x14, apn, (uint16_t)strlen(apn));
	}
	m_u8(&q, 0x19, 4);
	e = request(&wds, &q, &r);
	if (e) {
		printf("start network  failed: QMI error %d, call end reason %u, verbose %u/%u\n", e,
		    tlv_u32(&r, 0x10) & 0xffff, tlv_u32(&r, 0x11) & 0xffff, tlv_u32(&r, 0x11) >> 16);
		hexdump("answer", r.b, r.n);
		return 1;
	}
	printf("start network  ok, handle %08x\n", tlv_u32(&r, 0x01));
	m_init(&q, 0, 0x002d);                  /* WDS Get Runtime Settings */
	m_u32(&q, 0x10, (1u << 4) | (1u << 8) | (1u << 9) | (1u << 13));
	if (request(&wds, &q, &r) == 0) {
		uint32_t ip = tlv_u32(&r, 0x1e);
		print_ip("address", ip);
		print_ip("gateway", tlv_u32(&r, 0x20));
		print_ip("netmask", tlv_u32(&r, 0x21));
		print_ip("dns", tlv_u32(&r, 0x15));
		print_ip("dns 2", tlv_u32(&r, 0x16));
		printf("%-14s %u\n", "mtu", tlv_u32(&r, 0x29));
		uint32_t gw = tlv_u32(&r, 0x20);
		FILE *f = fopen("/var/tmp/datacall.ip", "w");
		if (f != NULL) {
			uint32_t d1 = tlv_u32(&r, 0x15), d2 = tlv_u32(&r, 0x16);
			fprintf(f, "%u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u %u.%u.%u.%u\n", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255,
			    ip & 255, gw >> 24, (gw >> 16) & 255, (gw >> 8) & 255, gw & 255, d1 >> 24, (d1 >> 16) & 255,
			    (d1 >> 8) & 255, d1 & 255, d2 >> 24, (d2 >> 16) & 255, (d2 >> 8) & 255, d2 & 255);
			fclose(f);
		}
		if (getenv("NO_PDP") == NULL && pdp_config(ip, gw, tlv_u32(&r, 0x29)) == 0 &&
		    access(PDPNET, X_OK) == 0) {
			fflush(stdout);
			/* pdpnet watch: the call's IPv4 and DNS for configd while the cellular data switch is on */
			pid_t pid = fork();
			if (pid == 0) {
				setsid();
				int fd = open("/var/tmp/pdpnet.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
				if (fd >= 0) {
					dup2(fd, 1);
					dup2(fd, 2);
				}
				execl(PDPNET, "pdpnet", "watch", (char *)NULL);
				_exit(127);
			}
		}
	}
	fflush(stdout);
	for (;;) {                              /* keep the WDS client (and the call) alive */
		sleep(10);
		m_init(&q, 0, 0x0024);          /* WDS Get Packet Statistics */
		m_u32(&q, 0x01, 0x3ff);
		if (request(&wds, &q, &r) == 0) {
			printf("stats: tx ok %u err %u ovf %u, rx ok %u err %u ovf %u, tx dropped %u rx dropped %u\n",
			    tlv_u32(&r, 0x10), tlv_u32(&r, 0x12), tlv_u32(&r, 0x14), tlv_u32(&r, 0x11), tlv_u32(&r, 0x13),
			    tlv_u32(&r, 0x15), tlv_u32(&r, 0x1b), tlv_u32(&r, 0x1c));
			fflush(stdout);
		}
	}
}

/* 1 when NAS says registered with PS attached, 0 otherwise, -1 when NAS does not answer */
static int
ps_attached(void)
{
	struct cli c;
	struct msg r;
	if (cli_open(&c, SVC_NAS) != 0) {
		return -1;
	}
	int e = simple(&c, 0x0024, &r);
	uint16_t l;
	const uint8_t *v;
	int ok = e != 0 ? -1 : ((v = tlv(&r, 0x01, &l)) != NULL && l >= 5 && v[0] == 1 && v[2] == 1);
	close(c.fd);
	return ok;
}

/*
 * qmitool autodata [APN]: the data call at boot, without anyone at the console (started by
 * ccstart next to CommCenter). Waits for the modem's registration with packet service (the PIN is
 * entered on the lock screen), takes the modem online when it sits in low power after boot, then
 * runs datacall; when the call fails or ends, starts over. One instance (flock).
 */
static int
cmd_autodata(const char *apn)
{
	if (geteuid() == 0) {
		/* setuid root from ccstart (CommCenter's user): root for the address, the route, pdpnet */
		(void)setgid(0);
		(void)setuid(0);
	}
	int lk = open("/var/tmp/autodata.lock", O_RDWR | O_CREAT, 0644);
	if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB) < 0) {
		printf("autodata: already running\n");
		return 0;
	}
	if (apn == NULL) {
		FILE *f = fopen("/var/root/apn", "r");
		static char b[64];
		if (f != NULL && fscanf(f, "%63s", b) == 1) {
			apn = b;
		}
		if (f != NULL) {
			fclose(f);
		}
	}
	if (apn == NULL) {
		apn = "internet";
	}
	time_t last_online = 0;
	for (;;) {
		int a = ps_attached();
		if (a != 1) {
			time_t now = time(NULL);
			if (a == 0 && now - last_online >= 30) {
				printf("autodata: not attached, modem online\n");
				fflush(stdout);
				(void)cmd_mode(0);
				last_online = now;
			}
			sleep(3);
			continue;
		}
		printf("autodata: attached, data call on APN %s\n", apn);
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			_exit(cmd_datacall(apn));
		}
		int st = 0;
		if (pid > 0) {
			(void)waitpid(pid, &st, 0);
		}
		printf("autodata: data call ended (status %d), again in 10 s\n", st);
		fflush(stdout);
		sleep(10);
	}
}

static uint16_t
ip_sum(const uint8_t *p, int n)
{
	uint32_t s = 0;
	for (int i = 0; i + 1 < n; i += 2) {
		s += (uint32_t)(p[i] << 8 | p[i + 1]);
	}
	if (n & 1) {
		s += (uint32_t)p[n - 1] << 8;
	}
	while (s >> 16) {
		s = (s & 0xffff) + (s >> 16);
	}
	return (uint16_t)~s;
}

struct rxrec { uint32_t seq; uint8_t mux, pad; uint16_t len; uint8_t data[248]; };

static int
cmd_ping(const char *dst_s, const char *src_s)
{
	char srcbuf[32] = "";
	if (src_s == NULL) {
		FILE *f = fopen("/var/tmp/datacall.ip", "r");
		if (f == NULL || fgets(srcbuf, sizeof(srcbuf), f) == NULL) {
			fprintf(stderr, "no source address: run qmitool datacall first, or give one\n");
			return 1;
		}
		fclose(f);
		srcbuf[strcspn(srcbuf, "\n")] = 0;
		src_s = srcbuf;
	}
	struct in_addr src, dst;
	if (inet_aton(src_s, &src) == 0 || inet_aton(dst_s, &dst) == 0) {
		fprintf(stderr, "bad address\n");
		return 2;
	}
	uint32_t last = 0;
	struct rxrec rx[8];
	size_t rl = sizeof(rx);
	if (sysctlbyname("kern.sweet_ipa_rx", rx, &rl, NULL, 0) == 0) {
		for (unsigned i = 0; i < 8; i++) {
			last = rx[i].seq > last ? rx[i].seq : last;
		}
	}
	int got = 0;
	int burst = getenv("PING_BURST") ? atoi(getenv("PING_BURST")) : 0;
	if (burst > 0) {
		/* a burst of echo requests back to back (the modem's receive channel stays awake), then
		 * whatever comes back within 5 s */
		uint8_t pkt[1 + 84];
		for (int k = 0; k < burst; k++) {
			memset(pkt, 0, sizeof(pkt));
			uint8_t *ip = pkt + 1, *ic = ip + 20;
			pkt[0] = 1;
			ip[0] = 0x45; ip[3] = 84; ip[4] = 0x13; ip[5] = (uint8_t)k; ip[8] = 64; ip[9] = 1;
			memcpy(ip + 12, &src, 4);
			memcpy(ip + 16, &dst, 4);
			uint16_t c = ip_sum(ip, 20);
			ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
			ic[0] = 8; ic[4] = 0x53; ic[5] = 0x57; ic[7] = (uint8_t)k;
			c = ip_sum(ic, 64);
			ic[2] = (uint8_t)(c >> 8); ic[3] = (uint8_t)c;
			for (int t = 0; t < 100 && sysctlbyname("kern.sweet_ipa_tx", NULL, NULL, pkt, sizeof(pkt)) != 0; t++) {
				usleep(2000);
			}
		}
		for (int w = 0; w < 500; w++) {
			usleep(10000);
			rl = sizeof(rx);
			if (sysctlbyname("kern.sweet_ipa_rx", rx, &rl, NULL, 0) != 0) continue;
			for (unsigned i = 0; i < 8; i++) {
				if (rx[i].seq <= last) continue;
				const uint8_t *r = rx[i].data;
				printf("from the modem: %u bytes, proto %u, %u.%u.%u.%u -> %u.%u.%u.%u, icmp type %u\n", rx[i].len, r[9],
				    r[12], r[13], r[14], r[15], r[16], r[17], r[18], r[19], r[20]);
				last = rx[i].seq;
				got++;
			}
		}
		printf("%d of %d answered\n", got, burst);
		return got ? 0 : 1;
	}
	for (uint16_t seq = 1; seq <= 4; seq++) {
		uint8_t pkt[1 + 84];
		uint8_t *ip = pkt + 1, *ic = ip + 20;
		memset(pkt, 0, sizeof(pkt));
		pkt[0] = 1;                     /* mux id */
		ip[0] = 0x45;
		ip[2] = 0;
		ip[3] = 84;
		ip[4] = 0x12;
		ip[5] = (uint8_t)seq;
		ip[8] = 64;
		ip[9] = 1;
		memcpy(ip + 12, &src, 4);
		memcpy(ip + 16, &dst, 4);
		uint16_t c = ip_sum(ip, 20);
		ip[10] = (uint8_t)(c >> 8);
		ip[11] = (uint8_t)c;
		ic[0] = 8;
		ic[4] = 0x53;
		ic[5] = 0x57;
		ic[6] = 0;
		ic[7] = (uint8_t)seq;
		for (int i = 8; i < 64; i++) {
			ic[i] = (uint8_t)i;
		}
		c = ip_sum(ic, 64);
		ic[2] = (uint8_t)(c >> 8);
		ic[3] = (uint8_t)c;
		struct timeval t0, t1;
		gettimeofday(&t0, NULL);
		if (sysctlbyname("kern.sweet_ipa_tx", NULL, NULL, pkt, sizeof(pkt)) != 0) {
			fprintf(stderr, "kern.sweet_ipa_tx: %s\n", strerror(errno));
			return 1;
		}
		int ok = 0;
		for (int w = 0; w < 300 && !ok; w++) {
			usleep(10000);
			rl = sizeof(rx);
			if (sysctlbyname("kern.sweet_ipa_rx", rx, &rl, NULL, 0) != 0) {
				continue;
			}
			for (unsigned i = 0; i < 8; i++) {
				if (rx[i].seq <= last) {
					continue;
				}
				const uint8_t *r = rx[i].data;
				if (rx[i].len >= 28 && r[9] == 1 && r[20] == 0 && memcmp(r + 12, &dst, 4) == 0 && r[27] == seq) {
					gettimeofday(&t1, NULL);
					printf("%u bytes from %s: icmp_seq=%u ttl=%u time=%.1f ms\n", rx[i].len - 20, dst_s, seq, r[8],
					    (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0);
					ok = 1;
				} else if (rx[i].seq > last) {
					printf("(packet from the modem: %u bytes, proto %u, from %u.%u.%u.%u)\n", rx[i].len, r[9], r[12], r[13],
					    r[14], r[15]);
				}
				last = rx[i].seq > last ? rx[i].seq : last;
			}
		}
		if (!ok) {
			printf("icmp_seq=%u: no answer in 3 s\n", seq);
		}
		got += ok;
		sleep(1);
	}
	printf("%d of 4 answered\n", got);
	return got ? 0 : 1;
}

/* one DNS query (A record) over UDP through the call: any answer from the server shows the downlink works */
static int
cmd_dns(const char *server_s, const char *name)
{
	char srcbuf[32] = "";
	FILE *f = fopen("/var/tmp/datacall.ip", "r");
	if (f == NULL || fgets(srcbuf, sizeof(srcbuf), f) == NULL) {
		fprintf(stderr, "no source address: run qmitool datacall first\n");
		return 1;
	}
	fclose(f);
	srcbuf[strcspn(srcbuf, "\n")] = 0;
	struct in_addr src, dst;
	if (inet_aton(srcbuf, &src) == 0 || inet_aton(server_s, &dst) == 0) {
		return 2;
	}
	uint8_t pkt[1 + 512];
	memset(pkt, 0, sizeof(pkt));
	uint8_t *ip = pkt + 1, *udp = ip + 20, *q = udp + 8;
	int n = 0;
	q[0] = 0x51; q[1] = 0x57; q[2] = 0x01; q[5] = 1;       /* id, recursion desired, 1 question */
	n = 12;
	for (const char *l = name; *l;) {
		const char *dot = strchr(l, '.');
		int len = dot ? (int)(dot - l) : (int)strlen(l);
		q[n++] = (uint8_t)len;
		memcpy(q + n, l, (size_t)len);
		n += len;
		l += len + (dot ? 1 : 0);
	}
	q[n++] = 0;
	q[n++] = 0; q[n++] = 1;                                 /* type A */
	q[n++] = 0; q[n++] = 1;                                 /* class IN */
	int ulen = 8 + n, tlen = 20 + ulen;
	pkt[0] = 1;
	ip[0] = 0x45; ip[2] = (uint8_t)(tlen >> 8); ip[3] = (uint8_t)tlen; ip[4] = 0x53; ip[8] = 64; ip[9] = 17;
	memcpy(ip + 12, &src, 4);
	memcpy(ip + 16, &dst, 4);
	uint16_t c = ip_sum(ip, 20);
	ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
	udp[0] = 0xc3; udp[1] = 0x50; udp[3] = 53; udp[4] = (uint8_t)(ulen >> 8); udp[5] = (uint8_t)ulen;  /* 50000 -> 53, no checksum */
	struct rxrec rx[8];
	size_t rl = sizeof(rx);
	uint32_t last = 0;
	if (sysctlbyname("kern.sweet_ipa_rx", rx, &rl, NULL, 0) == 0) {
		for (unsigned i = 0; i < 8; i++) last = rx[i].seq > last ? rx[i].seq : last;
	}
	if (sysctlbyname("kern.sweet_ipa_tx", NULL, NULL, pkt, (size_t)(1 + tlen)) != 0) {
		fprintf(stderr, "kern.sweet_ipa_tx: %s\n", strerror(errno));
		return 1;
	}
	for (int w = 0; w < 300; w++) {
		usleep(10000);
		rl = sizeof(rx);
		if (sysctlbyname("kern.sweet_ipa_rx", rx, &rl, NULL, 0) != 0) continue;
		for (unsigned i = 0; i < 8; i++) {
			if (rx[i].seq <= last) continue;
			const uint8_t *r = rx[i].data;
			printf("answer: %u bytes, proto %u, from %u.%u.%u.%u", rx[i].len, r[9], r[12], r[13], r[14], r[15]);
			if (r[9] == 17 && rx[i].len >= 28 + 12) {
				const uint8_t *d = r + 28;
				printf(", DNS rcode %u, %u answers", d[3] & 15, d[7]);
			}
			printf("\n");
			return 0;
		}
	}
	printf("no answer in 3 s\n");
	return 1;
}

/* one TCP SYN through the call: a SYN-ACK or RST back shows the downlink works */
static int
cmd_syn(const char *dst_s, int port)
{
	char srcbuf[32] = "";
	FILE *f = fopen("/var/tmp/datacall.ip", "r");
	if (f == NULL || fgets(srcbuf, sizeof(srcbuf), f) == NULL) {
		return 1;
	}
	fclose(f);
	srcbuf[strcspn(srcbuf, "\n")] = 0;
	struct in_addr src, dst;
	if (inet_aton(srcbuf, &src) == 0 || inet_aton(dst_s, &dst) == 0) {
		return 2;
	}
	uint8_t pkt[1 + 40] = { 1 };
	uint8_t *ip = pkt + 1, *t = ip + 20;
	ip[0] = 0x45; ip[3] = 40; ip[4] = 0x54; ip[6] = 0x40; ip[8] = 64; ip[9] = 6;
	memcpy(ip + 12, &src, 4);
	memcpy(ip + 16, &dst, 4);
	uint16_t c = ip_sum(ip, 20);
	ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
	t[0] = 0xc3; t[1] = 0x51; t[2] = (uint8_t)(port >> 8); t[3] = (uint8_t)port;
	t[4] = 0x12; t[5] = 0x34; t[6] = 0x56; t[7] = 0x78;
	t[12] = 0x50; t[13] = 0x02; t[14] = 0xff; t[15] = 0xff;           /* 20-byte header, SYN, window */
	uint8_t ph[12 + 20];
	memcpy(ph, ip + 12, 8);
	ph[8] = 0; ph[9] = 6; ph[10] = 0; ph[11] = 20;
	memcpy(ph + 12, t, 20);
	c = ip_sum(ph, sizeof(ph));
	t[16] = (uint8_t)(c >> 8); t[17] = (uint8_t)c;
	struct rxrec rx[8];
	size_t rl = sizeof(rx);
	uint32_t last = 0;
	if (sysctlbyname("kern.sweet_ipa_rx", rx, &rl, NULL, 0) == 0) {
		for (unsigned i = 0; i < 8; i++) last = rx[i].seq > last ? rx[i].seq : last;
	}
	if (sysctlbyname("kern.sweet_ipa_tx", NULL, NULL, pkt, sizeof(pkt)) != 0) {
		fprintf(stderr, "kern.sweet_ipa_tx: %s\n", strerror(errno));
		return 1;
	}
	for (int w = 0; w < 300; w++) {
		usleep(10000);
		rl = sizeof(rx);
		if (sysctlbyname("kern.sweet_ipa_rx", rx, &rl, NULL, 0) != 0) continue;
		for (unsigned i = 0; i < 8; i++) {
			if (rx[i].seq <= last) continue;
			const uint8_t *r = rx[i].data;
			printf("answer: %u bytes proto %u from %u.%u.%u.%u flags %02x\n", rx[i].len, r[9], r[12], r[13], r[14], r[15],
			    rx[i].len > 33 ? r[33] : 0);
			return 0;
		}
	}
	printf("no answer in 3 s\n");
	return 1;
}

/* the last raw frames the kernel received from the IPA (kern.sweet_ipa_raw, kernel 0228+) */
static int
cmd_ipa_raw(void)
{
	struct { uint32_t seq; uint8_t chan, pad; uint16_t len; uint8_t data[64]; } raw[8];
	size_t rl = sizeof(raw);
	if (sysctlbyname("kern.sweet_ipa_raw", raw, &rl, NULL, 0) != 0) {
		fprintf(stderr, "kern.sweet_ipa_raw: %s\n", strerror(errno));
		return 1;
	}
	for (unsigned i = 0; i < 8; i++) {
		if (raw[i].seq == 0) continue;
		printf("#%u %s %u bytes:", raw[i].seq, raw[i].chan == 3 ? "modem RX" : "LAN RX", raw[i].len);
		for (unsigned k = 0; k < raw[i].len && k < 64; k++) printf(" %02x", raw[i].data[k]);
		printf("\n");
	}
	return 0;
}

/* qmitool peek "g 1c000 32" / "i 810" / "s 290 32" / "wi 810 VALUE": kern.sweet_ipa_peek (kernel 0229+) */
static int
cmd_peek(int argc, char **argv)
{
	char req[64] = "";
	for (int i = 0; i < argc; i++) {
		strlcat(req, argv[i], sizeof(req));
		if (i + 1 < argc) strlcat(req, " ", sizeof(req));
	}
	if (sysctlbyname("kern.sweet_ipa_peek", NULL, NULL, req, strlen(req)) != 0) {
		fprintf(stderr, "kern.sweet_ipa_peek: %s\n", strerror(errno));
		return 1;
	}
	char out[1100];
	size_t ol = sizeof(out);
	if (sysctlbyname("kern.sweet_ipa_peek", out, &ol, NULL, 0) != 0) {
		return 1;
	}
	fputs(out, stdout);
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
	if (!strcmp(cmd, "datacall")) return cmd_datacall(argc > 2 ? argv[2] : NULL);
	if (!strcmp(cmd, "ipa-raw")) return cmd_ipa_raw();
	if (!strcmp(cmd, "pdp-up")) return cmd_pdp_up();
	if (!strcmp(cmd, "autodata")) return cmd_autodata(argc > 2 ? argv[2] : NULL);
	if (!strcmp(cmd, "http") && argc > 3) return cmd_http(argv[2], argv[3], argc > 4 ? argv[4] : "/");
	if (!strcmp(cmd, "syn") && argc > 3) return cmd_syn(argv[2], atoi(argv[3]));
	if (!strcmp(cmd, "peek") && argc > 3) return cmd_peek(argc - 2, argv + 2);
	if (!strcmp(cmd, "dns") && argc > 3) return cmd_dns(argv[2], argv[3]);
	if (!strcmp(cmd, "ping") && argc > 2) return cmd_ping(argv[2], argc > 3 ? argv[3] : NULL);
	fprintf(stderr, "unknown command %s\n", cmd);
	return 2;
}
