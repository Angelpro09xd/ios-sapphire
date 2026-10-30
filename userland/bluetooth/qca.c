/*
 * qca.c: Qualcomm WCN3988 bring-up over the kernel's /dev/qcabt (sapphire kernel 0280), after
 * Linux drivers/bluetooth/btqca.c + hci_qca.c. Included by qcabtfw.c and btshim.c.
 *
 * Kernel side (0280): `sysctl kern.sweet_bt=1` = rails, SE3 clock, pins, 115200 8N1 RTS/CTS;
 * `=2` = power pulse + HCI_Reset (refused with EBUSY while /dev/qcabt is open). /dev/qcabt is
 * root 0600, one opener, raw H4 bytes both ways. The baud ioctl (QCABT_SETBAUD, qcabt.h) is
 * optional: without it everything stays at 115200 and the NVM is told so.
 *
 * Version reply on sapphire: 04 ff 0e | 00 02 | 0a000000 0100 0102 50010240
 *   cresp 0x00, rtype 0x02 (EDL_APP_VER_RES_EVT), product_id 0x0a, patch_ver 0x0001,
 *   rom_ver 0x0201, soc_id 0x40020150 -> soc_ver 0x01500201 -> WCN3988 rom 0x11 ->
 *   apbtfw11.tlv (its header: product 0xa, rom_build 0x201) + apnv11.bin.
 * The firmware partition's NVMs are type-4 containers: a type-2 NVM entry (the one with the
 * HCI tag 17 and the deep-sleep tag 27) and a type-3 entry. The tag tweaks are applied to every
 * type-2 entry inside; the file is sent whole, as Linux sends a whole NVM.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include "qcabt.h"

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int qca_verbose;
static int qfd = -1;

static uint64_t
q_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void
q_hex(const char *tag, const uint8_t *p, size_t n)
{
	char line[3 * 40 + 1];
	for (size_t i = 0; i < n; i += 40) {
		size_t k = 0;
		for (size_t j = i; j < n && j < i + 40; j++)
			k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x ", p[j]);
		say("%s %s", i ? "   " : tag, line);
	}
}

/* ---------------------------------------------------------------- RX: a thread, a ring */

static uint8_t rx_ring[65536];
static size_t rx_head, rx_tail;         /* head: write, tail: read */
static pthread_mutex_t rx_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rx_cv = PTHREAD_COND_INITIALIZER;
int qca_rx_notify_fd = -1;              /* btshim: a pipe poked after every read */

static void *
rx_thread(void *arg)
{
	uint8_t b[2048];
	for (;;) {
		ssize_t n = read(qfd, b, sizeof(b));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			usleep(2000);
			continue;
		}
		pthread_mutex_lock(&rx_mu);
		for (ssize_t i = 0; i < n; i++) {
			size_t nh = (rx_head + 1) % sizeof(rx_ring);
			if (nh == rx_tail)
				break;  /* full: drop */
			rx_ring[rx_head] = b[i];
			rx_head = nh;
		}
		pthread_cond_broadcast(&rx_cv);
		pthread_mutex_unlock(&rx_mu);
		if (qca_rx_notify_fd >= 0)
			(void)write(qca_rx_notify_fd, "", 1);
	}
	return NULL;
}

/* next received byte within ms; -1 on timeout */
static int
rx_byte(uint64_t deadline)
{
	pthread_mutex_lock(&rx_mu);
	while (rx_head == rx_tail) {
		uint64_t t = q_now();
		if (t >= deadline) {
			pthread_mutex_unlock(&rx_mu);
			return -1;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		uint64_t ns = (uint64_t)ts.tv_nsec + (deadline - t) * 1000000ull;
		ts.tv_sec += (time_t)(ns / 1000000000ull);
		ts.tv_nsec = (long)(ns % 1000000000ull);
		pthread_cond_timedwait(&rx_cv, &rx_mu, &ts);
	}
	int c = rx_ring[rx_tail];
	rx_tail = (rx_tail + 1) % sizeof(rx_ring);
	pthread_mutex_unlock(&rx_mu);
	return c;
}

/* bytes already received and not consumed (btshim drains them after bring-up) */
size_t
qca_rx_take(uint8_t *out, size_t max)
{
	size_t n = 0;
	pthread_mutex_lock(&rx_mu);
	while (rx_tail != rx_head && n < max) {
		out[n++] = rx_ring[rx_tail];
		rx_tail = (rx_tail + 1) % sizeof(rx_ring);
	}
	pthread_mutex_unlock(&rx_mu);
	return n;
}

/* one H4 packet (type byte first) within ms; returns its length, -1 on timeout */
static uint8_t pkt[4 + 65536];

static int
rx_packet(unsigned ms)
{
	uint64_t dl = q_now() + ms;
	int c;
	for (;;) {
		if ((c = rx_byte(dl)) < 0)
			return -1;
		if (c >= 1 && c <= 5)
			break;
		if (c == 0xfd || c == 0xfe || c == 0xfc)
			say("qca: in-band-sleep byte %#x from the SoC (ignored)", c);
		else
			say("qca: junk byte %#x", c);
	}
	pkt[0] = (uint8_t)c;
	size_t hl = (c == 1 || c == 3) ? 3 : (c == 4 ? 2 : 4), n = 1;
	while (n < 1 + hl) {
		if ((c = rx_byte(dl)) < 0)
			return -1;
		pkt[n++] = (uint8_t)c;
	}
	size_t pl = pkt[0] == 4 ? pkt[2] : (pkt[0] == 1 || pkt[0] == 3) ? pkt[3] :
	    pkt[0] == 2 ? (size_t)(pkt[3] | (pkt[4] << 8)) : (size_t)(pkt[3] | ((pkt[4] & 0x3f) << 8));
	while (n < 1 + hl + pl) {
		if ((c = rx_byte(dl)) < 0)
			return -1;
		pkt[n++] = (uint8_t)c;
	}
	if (qca_verbose)
		q_hex("C> ", pkt, n);
	return (int)n;
}

/* ---------------------------------------------------------------- TX */

static int
q_write(const uint8_t *p, size_t n)
{
	if (qca_verbose && n <= 64)
		q_hex("H> ", p, n);
	while (n) {
		ssize_t w = write(qfd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				usleep(1000);
				continue;
			}
			say("qcabt write: %s", strerror(errno));
			return -1;
		}
		p += w, n -= (size_t)w;
	}
	return 0;
}

int
qca_cmd(uint16_t op, const void *par, uint8_t len)
{
	uint8_t c[4 + 255];
	c[0] = 1, c[1] = op & 0xff, c[2] = op >> 8, c[3] = len;
	if (len)
		memcpy(c + 4, par, len);
	return q_write(c, 4u + len);
}

int
qca_raw_write(const uint8_t *p, size_t n)
{
	return q_write(p, n);
}

/* Command Complete (or Status) of op within ms: status, -1 timeout; return params -> ret */
int
qca_wait_cc(uint16_t op, unsigned ms, uint8_t *ret, size_t *rlen)
{
	uint64_t dl = q_now() + ms;
	while (q_now() < dl) {
		int n = rx_packet((unsigned)(dl - q_now()));
		if (n < 0)
			break;
		if (pkt[0] == 4 && pkt[1] == 0x0e && n >= 7 && (pkt[4] | (pkt[5] << 8)) == op) {
			if (ret) {
				memcpy(ret, pkt + 6, (size_t)n - 6);
				*rlen = (size_t)n - 6;
			}
			return pkt[6];
		}
		if (pkt[0] == 4 && pkt[1] == 0x0f && n >= 7 && (pkt[5] | (pkt[6] << 8)) == op)
			return pkt[3];
		if (!qca_verbose)
			say("qca: waiting for %#06x, got packet type %u event %#x (ignored)", op, pkt[0], pkt[1]);
	}
	return -1;
}

/* vendor event 0xFF [cresp 0x00][rtype]...: data after rtype -> out; -1 timeout */
static int
wait_vse(uint8_t rtype, unsigned ms, uint8_t *out, size_t *outlen)
{
	uint64_t dl = q_now() + ms;
	while (q_now() < dl) {
		int n = rx_packet((unsigned)(dl - q_now()));
		if (n < 0)
			break;
		if (pkt[0] == 4 && pkt[1] == 0xff && n >= 5 && pkt[4] == rtype) {
			if (out) {
				memcpy(out, pkt + 5, (size_t)n - 5);
				*outlen = (size_t)n - 5;
			}
			return 0;
		}
		if (pkt[0] == 4 && pkt[1] == 0x0e && n >= 7 && pkt[4] == 0x00 && pkt[5] == 0xfc) {
			/* the WCN3988 acks NVM segments with a Command Complete for 0xFC00, not a VSE */
			if (rtype == 0x04) {
				if (out) {
					out[0] = pkt[6];
					*outlen = 1;
				}
				return 0;
			}
			if (pkt[6])
				say("qca: 0xFC00 Command Complete status %#x", pkt[6]);
			continue;
		}
		if (!qca_verbose)
			say("qca: waiting for VSE %#x, got packet type %u event %#x (ignored)", rtype, pkt[0], pkt[1]);
	}
	return -1;
}

static void
drain(unsigned ms)
{
	while (rx_packet(ms) >= 0)
		;
}

/* ---------------------------------------------------------------- open, speed */

static int
sweet_bt_op(int op)
{
	if (sysctlbyname("kern.sweet_bt", NULL, NULL, &op, sizeof(op)) < 0) {
		say("sysctl kern.sweet_bt=%d: %s", op, strerror(errno));
		return -1;
	}
	return 0;
}

static pthread_t rx_tid;
static int host_baud_ok = -1;           /* QCABT_SETBAUD available? */

/* power_on: kern.sweet_bt=1 (rails, UART) and =2 (power pulse + HCI_Reset) before opening */
int
qca_open(int power_on)
{
	if (qfd >= 0)
		return 0;
	if (power_on && (sweet_bt_op(1) < 0 || sweet_bt_op(2) < 0))
		return -1;
	qfd = open(QCABT_DEV, O_RDWR);
	if (qfd < 0) {
		say("open %s: %s (run sysctl kern.sweet_bt=1 first)", QCABT_DEV, strerror(errno));
		return -1;
	}
	pthread_create(&rx_tid, NULL, rx_thread, NULL);
	return 0;
}

int
qca_fd(void)
{
	return qfd;
}

static int
host_speed(uint32_t b)
{
	if (host_baud_ok == 0)
		return -1;
	if (ioctl(qfd, QCABT_SETBAUD, &b) < 0) {
		if (host_baud_ok < 0)
			say("qca: the kernel has no QCABT_SETBAUD (%s): staying at 115200", strerror(errno));
		host_baud_ok = 0;
		return -1;
	}
	host_baud_ok = 1;
	return 0;
}

/* Linux hci_qca.c enum qca_baudrate */
int
qca_baud_code(uint32_t b)
{
	static const uint32_t t[] = { 115200, 57600, 38400, 19200, 9600, 230400, 250000, 460800, 500000,
		720000, 921600, 1000000, 1250000, 2000000, 3000000, 4000000, 1600000, 3200000, 3500000 };
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i] == b)
			return (int)i;
	return -1;
}

uint32_t qca_speed = 115200;            /* the speed both sides run at now */

/* 0xFC48 [code]; the SoC answers with a vendor event at the new speed (Linux drops it) */
int
qca_set_speed(uint32_t baud)
{
	int code = qca_baud_code(baud);
	if (code < 0 || host_speed(115200) < 0)    /* probe the ioctl before touching the SoC */
		return -1;
	int off = 0, on = 1;
	ioctl(qfd, QCABT_FLOW, &off);
	uint8_t c = (uint8_t)code;
	if (qca_cmd(0xfc48, &c, 1) < 0)
		return -1;
	ioctl(qfd, QCABT_DRAIN);
	usleep(15000);
	host_speed(baud);
	ioctl(qfd, QCABT_FLOW, &on);
	if (rx_packet(300) < 0)
		say("qca: no event after the baud change");
	qca_speed = baud;
	say("qca: UART now at %u baud", baud);
	return 0;
}

/* ---------------------------------------------------------------- version */

struct qca_version {
	uint32_t product_id, soc_id, soc_ver;
	uint16_t patch_ver, rom_ver;
	unsigned fw_rom;        /* the XX of apbtfwXX.tlv / apnvXX.bin */
};
struct qca_version qca_ver;

int
qca_read_version(void)
{
	uint8_t sub = 0x19, d[64];      /* EDL_PATCH_VER_REQ_CMD */
	size_t n = 0;
	for (int tries = 0; tries < 3; tries++) {
		if (qca_cmd(0xfc00, &sub, 1) < 0)
			return -1;
		if (wait_vse(0x02, 1000, d, &n) == 0 && n >= 12) {     /* EDL_APP_VER_RES_EVT */
			memcpy(&qca_ver.product_id, d, 4);
			memcpy(&qca_ver.patch_ver, d + 4, 2);
			memcpy(&qca_ver.rom_ver, d + 6, 2);
			memcpy(&qca_ver.soc_id, d + 8, 4);
			qca_ver.soc_ver = (qca_ver.soc_id << 16) | qca_ver.rom_ver;
			/* btqca.c qca_uart_setup: WCN3988 shifts by 5, the others by 4 */
			qca_ver.fw_rom = ((qca_ver.soc_ver & 0xf00) >> 5) | (qca_ver.soc_ver & 0xf);
			say("qca: product_id %#x patch_ver %#x rom_ver %#x soc_id %#x -> soc_ver %#010x, files apbtfw%02x.tlv + apnv%02x.bin",
			    qca_ver.product_id, qca_ver.patch_ver, qca_ver.rom_ver, qca_ver.soc_id, qca_ver.soc_ver,
			    qca_ver.fw_rom, qca_ver.fw_rom);
			return 0;
		}
		say("qca: no version event (try %d)", tries + 1);
	}
	return -1;
}

/* ---------------------------------------------------------------- download */

static uint8_t *
load_file(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		say("open %s: %s", path, strerror(errno));
		return NULL;
	}
	struct stat st;
	fstat(fd, &st);
	uint8_t *b = malloc((size_t)st.st_size + 1);
	ssize_t r = b ? read(fd, b, (size_t)st.st_size) : -1;
	close(fd);
	if (r != st.st_size) {
		free(b);
		return NULL;
	}
	*len = (size_t)st.st_size;
	return b;
}

int qca_keep_ibs;       /* -I: leave in-band sleep and deep sleep as the NVM has them */
int qca_nvm_inner;      /* -2: of a type-4 NVM container send only the type-2 entry */

/* one type-2 NVM body [off, end): the tweaks of btqca.c qca_tlv_check_data, adapted to a host
 * that does not speak in-band sleep yet */
static void
nvm_tweak(uint8_t *b, size_t off, size_t end)
{
	while (off + 12 <= end) {
		unsigned tag = (unsigned)(b[off] | (b[off + 1] << 8));
		unsigned tl = (unsigned)(b[off + 2] | (b[off + 3] << 8));
		uint8_t *d = b + off + 12;
		if (off + 12 + tl > end)
			break;
		if (tag == 17 && tl >= 3) {             /* EDL_TAG_ID_HCI: [0] bit 7 IBS, [2] baud code */
			uint8_t o0 = d[0], o2 = d[2];
			if (!qca_keep_ibs)
				d[0] &= 0x7f;
			d[2] = (uint8_t)qca_baud_code(qca_speed);
			say("qca: NVM tag 17 (HCI) %02x..%02x -> %02x..%02x (IBS %s, baud %u)", o0, o2, d[0], d[2],
			    d[0] & 0x80 ? "on" : "off", qca_speed);
		} else if (tag == 27 && tl >= 1 && !qca_keep_ibs) {    /* EDL_TAG_ID_DEEP_SLEEP */
			say("qca: NVM tag 27 (deep sleep) %02x -> %02x", d[0], d[0] & 0xfe);
			d[0] &= 0xfe;
		}
		off += 12 + tl;
	}
}

static void
nvm_prepare(uint8_t *b, size_t len)
{
	if (b[0] == 2) {
		nvm_tweak(b, 4, len);
		return;
	}
	if (b[0] != 4)
		return;
	for (size_t o = 4; o + 4 <= len;) {     /* container: [type][len24] entries */
		size_t el = (size_t)(b[o + 1] | (b[o + 2] << 8) | (b[o + 3] << 16));
		say("qca: NVM container entry type %u, %zu bytes", b[o], el);
		if (b[o] == 2 && o + 4 + el <= len)
			nvm_tweak(b, o + 4, o + 4 + el);
		o += 4 + el;
	}
}

/* btqca.c qca_download_firmware: the whole file in 243-byte EDL_PATCH_TLV_REQ (0xFC00/0x1E)
 * segments. A patch's download_mode (header byte 14): 0 = wait for the vendor event (rtype
 * EDL_TVL_DNLD_RES_EVT 0x04, data[0] == 0) after every segment, 1/3 = no vendor event, 2 = no
 * Command Complete. NVMs: mode 0 (Linux resets the mode for the NVM). */
int
qca_download(const char *path, int is_nvm)
{
	size_t len = 0;
	uint8_t *b = load_file(path, &len);
	if (!b || len < 4)
		return -1;
	unsigned mode = 0;
	if (!is_nvm && b[0] == 1 && len > 14)
		mode = b[14];
	if (!is_nvm && b[0] == 1)
		say("qca: patch %s: product %#x rom_build %#x patch_ver %#x, download_mode %u", path,
		    b[16] | (b[17] << 8), b[18] | (b[19] << 8), b[20] | (b[21] << 8), mode);
	uint8_t *send = b;
	if (is_nvm) {
		nvm_prepare(b, len);
		for (size_t o = 4; qca_nvm_inner && b[0] == 4 && o + 4 <= len;) {
			size_t el = (size_t)(b[o + 1] | (b[o + 2] << 8) | (b[o + 3] << 16));
			if (b[o] == 2 && o + 4 + el <= len) {
				send = b + o, len = 4 + el;
				say("qca: sending only the type-2 entry of the container");
				break;
			}
			o += 4 + el;
		}
	}
	say("qca: sending %s (%zu bytes, TLV type %u)", path, len, send[0]);
	uint64_t t0 = q_now();
	int segs = 0, wait_acks = !(mode & 1);
	for (size_t off = 0; off < len;) {
		size_t n = len - off > 243 ? 243 : len - off;
		uint8_t c[2 + 243];
		c[0] = 0x1e, c[1] = (uint8_t)n;
		memcpy(c + 2, send + off, n);
		if (qca_cmd(0xfc00, c, (uint8_t)(n + 2)) < 0)
			goto fail;
		if (wait_acks) {
			uint8_t d[16];
			size_t dl = 0;
			if (wait_vse(0x04, 2000, d, &dl) < 0) {
				if (segs == 0 && is_nvm) {
					say("qca: no ack for the first NVM segment: continuing without acks");
					wait_acks = 0;
				} else {
					say("qca: no ack for segment %d", segs);
					goto fail;
				}
			} else if (dl && d[0] != 0) {
				say("qca: segment %d refused, code %#x", segs, d[0]);
				goto fail;
			}
		}
		off += n, segs++;
	}
	if (!wait_acks)
		usleep(100000);
	drain(100);
	say("qca: %s done: %d segments in %llu ms", path, segs, (unsigned long long)(q_now() - t0));
	free(b);
	return 0;
fail:
	free(b);
	return -1;
}

/* EDL_WRITE_BD_ADDR (0xFC14), 6 bytes little-endian; a[] as printed (MSB first) */
int
qca_set_bdaddr(const uint8_t a[6])
{
	/* the WCN3988 takes it MSB first (sent reversed, Read BD_ADDR showed it reversed) */
	if (qca_cmd(0xfc14, a, 6) < 0)
		return -1;
	uint64_t dl = q_now() + 1000;
	while (q_now() < dl) {
		int n = rx_packet((unsigned)(dl - q_now()));
		if (n < 0)
			break;
		if (pkt[0] == 4 && pkt[1] == 0xff)
			return 0;
		if (pkt[0] == 4 && pkt[1] == 0x0e && n >= 7 && pkt[4] == 0x14 && pkt[5] == 0xfc)
			return pkt[6] ? -1 : 0;
	}
	say("qca: no answer to Write BD_ADDR");
	return -1;
}

int
qca_reset(void)
{
	if (qca_cmd(0x0c03, NULL, 0) < 0)
		return -1;
	int st = qca_wait_cc(0x0c03, 3000, NULL, NULL);
	say("qca: HCI_Reset -> %s %d", st < 0 ? "timeout" : "status", st);
	return st;
}

void
qca_print_identity(void)
{
	uint8_t r[64];
	size_t rl = 0;
	qca_cmd(0x1001, NULL, 0);
	if (qca_wait_cc(0x1001, 1000, r, &rl) == 0 && rl >= 9)
		say("qca: Read Local Version: HCI %u rev %#x, LMP %u subversion %#x, manufacturer %u",
		    r[1], r[2] | (r[3] << 8), r[4], r[7] | (r[8] << 8), r[5] | (r[6] << 8));
	qca_cmd(0x1009, NULL, 0);
	if (qca_wait_cc(0x1009, 1000, r, &rl) == 0 && rl >= 7)
		say("qca: Read BD_ADDR: %02x:%02x:%02x:%02x:%02x:%02x", r[6], r[5], r[4], r[3], r[2], r[1]);
}

/* the whole sequence after the kernel's power-on: version, (speed), patch, NVM, reset, address */
int
qca_bringup(const char *dir, const char *ramp, const char *nvm, uint32_t speed, const uint8_t addr[6])
{
	char rp[512], np[512];
	if (qca_read_version() < 0)
		return -1;
	if (!ramp) {
		snprintf(rp, sizeof(rp), "%s/apbtfw%02x.tlv", dir, qca_ver.fw_rom);
		ramp = rp;
	}
	if (!nvm) {
		snprintf(np, sizeof(np), "%s/apnv%02x.bin", dir, qca_ver.fw_rom);
		nvm = np;
	}
	if (speed && speed != 115200 && qca_set_speed(speed) < 0)
		say("qca: staying at %u baud", qca_speed);
	if (qca_download(ramp, 0) < 0 || qca_download(nvm, 1) < 0)
		return -1;
	if (qca_reset() != 0)
		return -1;
	if (addr && qca_set_bdaddr(addr) < 0)
		say("qca: BD address not set");
	return 0;
}
