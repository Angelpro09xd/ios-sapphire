/*
 * btshim: Apple's bluetoothd on the sapphire's Qualcomm WCN3988 (research/bluetooth.md).
 *
 * How bluetoothd (iOS 16.7, 20H392) reaches its controller, from its disassembly:
 *  - BT_HCI_TRANSPORT="<transport>:<driver>:<options>" in the environment overrides the device
 *    tree. Transports: USB H4DS H4BC H4 H5 APPLEBT VIRT PCIE PCIESW. Drivers: USB POSIX APPLEBT
 *    VIRT PCIE PCIESW. On a real iPhone X the device tree gives transport-encoding 3 = H4BC over
 *    the POSIX driver, which is NOT a tty: it is the AppleSerialShim Skywalk nexus (kernel
 *    control "com.apple.uart.sk.bluetooth" + os_channel). /dev/uart.bluetooth is only access()ed.
 *  - The VIRT driver ("VirtualCTRL") opens <options>-in O_WRONLY (host -> controller bytes) and
 *    <options>-out O_RDONLY|O_NONBLOCK (controller -> host bytes), and a thread select()s and
 *    read()s the second one. With the H4 transport the bytes are plain H4 (type byte + packet).
 *  So bluetoothd runs with BT_HCI_TRANSPORT=VIRT:VIRT:/var/run/btshim/hci, and this program owns
 *  the two FIFOs /var/run/btshim/hci-in and hci-out.
 *  - bluetoothd starts only if the local address is non-zero (BT_DEVICE_ADDRESS=aa:bb:.. in the
 *    environment, else the device tree's local-mac-address) and if /dev/cu.bluetooth or
 *    /dev/uart.bluetooth exists or an IOService is named "marconi-bt" or "bluetooth"
 *    ("No bluetooth on this device." otherwise).
 *  - Before HCI, bluetoothd asks com.apple.BlueTool (XPC, mach service) to run its "boot" /
 *    "init" / "deepsleep" scripts: {kMsgId: "RunBuiltinScript", kMsgArgs: {script, btaddress}}
 *    -> reply {result: int64 (0 = ok), manufacturer: int64, firmware: string}, and
 *    {kMsgId: "RunCommand", kMsgArgs: {command}} -> {result, output}. Apple's BlueTool only knows
 *    Broadcom/ACI chips, so "btshim bluetool" replaces it (same mach service name) and forwards
 *    the script names to the daemon over /var/run/btshim/ctl.
 *  - Then the standard HCI init chain (every step fatal on error except Reset and Read BD_ADDR):
 *    Reset, Read Local Version, Read Local Supported Commands, Read BD_ADDR, Read Local Features,
 *    Read Local Extended Features, LE Read Local Features, Set Event Mask (+ Page 2), LE Set Event
 *    Mask, Write Simple Pairing Mode, Write LE Host Support, LE Read Buffer Size (v2 or v1), LE
 *    Read Supported States, LE Read Filter Accept List Size, Read Buffer Size, LE Read Periodic
 *    Advertiser List Size, LE Set Host Feature, LE Set Default Subrate, Host Buffer Size, Set
 *    Controller To Host Flow Control, Write Default Erroneous Data Reporting; then the Broadcom
 *    vendor commands of bcm_vsc.c (the chipset family comes from the platform, not from HCI, so
 *    an iPhone10,3 always gets the Broadcom set).
 *
 * Modes:
 *   btshim daemon [opts]   the bridge (launchd job, root). Real controller on /dev/qcabt.
 *   btshim daemon -F       fake controller, no hardware: answers like a BCM4357B1 and logs every
 *                          packet, to record what bluetoothd sends.
 *   btshim bluetool        the com.apple.BlueTool replacement (launchd job, MachServices).
 *   btshim bdstart         bluetoothd's launchd program: execs it with DYLD_INSERT_LIBRARIES=btbridge.dylib.
 * The Qualcomm bring-up itself is qca.c (shared with the standalone qcabtfw).
 * Options: -d fw-dir (/var/root/btfw)  -r rampatch.tlv  -n nvm.bin  -a aa:bb:cc:dd:ee:ff
 *          -s speed (default 115200; faster needs QCABT_SETBAUD)  -p fifo-prefix  -v  -I
 *          -w seconds: the bring-up waits until the boot is that old (120)
 * Log: /var/tmp/btshim.log (and stderr).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

/* libxpc (libSystem): the SDK in use ships no xpc headers, so the few calls are declared here */
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
extern xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
extern void xpc_connection_resume(xpc_connection_t);
extern void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
extern xpc_type_t xpc_get_type(xpc_object_t);
extern const char *xpc_dictionary_get_string(xpc_object_t, const char *);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t, const char *);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern void xpc_dictionary_set_int64(xpc_object_t, const char *, int64_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char *, const char *);
extern void xpc_release(xpc_object_t);
#include "qcabt.h"

#define RUN_DIR         "/var/run/btshim"
#define CTL_SOCK        RUN_DIR "/ctl"
#define DEF_PREFIX      RUN_DIR "/hci"
#define LOG_PATH        "/var/tmp/btshim.log"
#define FW_DIR          "/var/root/btfw"

#include "qca.c"

/* ---------------------------------------------------------------- options, log */

static const char *opt_dir = FW_DIR, *opt_ramp, *opt_nvm;   /* NULL: picked from the version */
static const char *opt_prefix = DEF_PREFIX;
static uint32_t opt_speed = 115200;
static uint8_t opt_bdaddr[6] = { 0x02, 0x5a, 0x50, 0x48, 0x52, 0x45 };  /* MSB first as printed */
static int opt_have_addr, opt_fake, opt_verbose;
static long opt_settle = 120;   /* -w: seconds of boot age before the bring-up */
static int log_fd = -1;

void
say(const char *fmt, ...)
{
	char buf[1024];
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int n = snprintf(buf, sizeof(buf), "%ld.%03d [%d] ", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), getpid());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	if (log_fd < 0)
		log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (log_fd >= 0)
		(void)write(log_fd, buf, (size_t)n);
	(void)write(2, buf, (size_t)n);
}

static void
hexdump(const char *tag, const uint8_t *p, size_t n)
{
	char line[3 * 64 + 8];
	size_t i = 0;
	while (i < n) {
		size_t k = 0, j;
		for (j = 0; j < 48 && i < n; j++, i++)
			k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x ", p[i]);
		say("%s %s", tag, line);
		tag = "  ";
	}
}

static void
msleep(unsigned ms)
{
	usleep(ms * 1000);
}

static int
parse_addr(const char *s, uint8_t a[6])
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		a[i] = (uint8_t)v[i];
	return 0;
}

/* ---------------------------------------------------------------- H4 framing */

struct h4 {
	uint8_t buf[4 + 65536];
	size_t have, need;      /* need: total length once the header is known, 0 = still unknown */
};

/* feeds bytes; calls fn for every complete packet (buf[0] = H4 type) */
static void
h4_feed(struct h4 *h, const uint8_t *p, size_t n, void (*fn)(const uint8_t *, size_t))
{
	while (n) {
		if (h->have == 0 && (p[0] < 1 || p[0] > 5)) {
			say("h4: junk byte %02x dropped", p[0]);
			p++, n--;
			continue;
		}
		h->buf[h->have++] = *p++;
		n--;
		uint8_t t = h->buf[0];
		size_t hl = (t == 1 || t == 3) ? 3 : (t == 4 ? 2 : 4);
		if (h->need == 0 && h->have == 1 + hl) {
			size_t pl;
			if (t == 1 || t == 3)
				pl = h->buf[3];
			else if (t == 4)
				pl = h->buf[2];
			else if (t == 2)
				pl = (size_t)(h->buf[3] | (h->buf[4] << 8));
			else
				pl = (size_t)(h->buf[3] | ((h->buf[4] & 0x3f) << 8));
			h->need = 1 + hl + pl;
		}
		if (h->need && h->have == h->need) {
			fn(h->buf, h->have);
			h->have = h->need = 0;
		}
	}
}

/* ---------------------------------------------------------------- the host side (bluetoothd) */

static int host_rd = -1, host_wr = -1;  /* we read <prefix>-in, write <prefix>-out */

static void
to_host(const uint8_t *p, size_t n)
{
	if (opt_verbose)
		hexdump("C>H", p, n);
	while (n) {
		ssize_t w = write(host_wr, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				msleep(1);
				continue;
			}
			say("host write: %s", strerror(errno));
			return;
		}
		p += w, n -= (size_t)w;
	}
}

static void
host_cc(uint16_t op, const uint8_t *ret, size_t rlen)
{
	uint8_t e[3 + 3 + 255];
	if (rlen > 252)
		rlen = 252;
	e[0] = 4, e[1] = 0x0e, e[2] = (uint8_t)(3 + rlen);
	e[3] = 1, e[4] = op & 0xff, e[5] = op >> 8;
	memcpy(e + 6, ret, rlen);
	to_host(e, 6 + rlen);
}

static void
host_cs(uint16_t op, uint8_t status)
{
	uint8_t e[7] = { 4, 0x0f, 4, status, 1, (uint8_t)(op & 0xff), (uint8_t)(op >> 8) };
	to_host(e, sizeof(e));
}

/* ---------------------------------------------------------------- the controller (qca.c) */

static struct h4 c_h4;                  /* controller -> us */
static int controller_up, ctl_notify[2] = { -1, -1 };

static int
c_write(const uint8_t *p, size_t n)
{
	if (opt_verbose)
		hexdump("H>C", p, n);
	return qca_raw_write(p, n);
}

static int
controller_bringup(void)
{
	if (controller_up)
		return 0;
	if (qca_open(1) < 0)
		return -1;
	if (qca_bringup(opt_dir, opt_ramp, opt_nvm, opt_speed, opt_bdaddr) < 0) {
		qca_close();    /* the next try power-cycles the BT side again */
		return -1;
	}
	qca_print_identity();
	/* the SoC's own debug log arrives as ACL on handle 0x2EDC; turn it off as Linux does for WCN399x
	 * (QCA_DISABLE_LOGGING 0xFC17 [0x14 0x00]); from_controller drops what still comes */
	{
		uint8_t off[2] = { 0x14, 0x00 };
		qca_cmd(0xfc17, off, 2);
		int st = qca_wait_cc(0xfc17, 500, NULL, NULL);
		say("SoC logging off (0xFC17) -> %d", st);
	}
	if (ctl_notify[0] < 0 && pipe(ctl_notify) == 0) {
		fcntl(ctl_notify[0], F_SETFL, O_NONBLOCK);
		fcntl(ctl_notify[1], F_SETFL, O_NONBLOCK);
	}
	qca_rx_notify_fd = ctl_notify[1];
	controller_up = 1;
	return 0;
}

/* ---------------------------------------------------------------- what bluetoothd is told */

/* identity reported to bluetoothd (Broadcom, like the iPhone X's BCM4357B1); adjust after a
 * capture of a real D22's Read Local Version if the stack turns out to check it */
#define ID_HCI_VER      0x09            /* 5.0 */
#define ID_HCI_REV      0x0000
#define ID_LMP_VER      0x09
#define ID_MANUF        0x000f          /* Broadcom */
#define ID_LMP_SUBVER   0x2209

static const uint8_t fake_features[8] = { 0xff, 0xfe, 0x8f, 0xfe, 0xd8, 0x3f, 0x5b, 0x87 };
static const uint8_t fake_le_features[8] = { 0xff, 0x01, 0, 0, 0, 0, 0, 0 };

/* a reply to a command bluetoothd sent while no controller answers it (fake mode), or the
 * Broadcom vendor commands that must never reach the Qualcomm chip (both modes) */
static void
answer_locally(uint16_t op, const uint8_t *par, size_t plen)
{
	uint8_t r[256];
	size_t n = 1;
	memset(r, 0, sizeof(r));
	uint8_t ogf = (uint8_t)(op >> 10);
	switch (op) {
	case 0x1001:    /* Read Local Version */
		r[1] = ID_HCI_VER, r[2] = ID_HCI_REV & 0xff, r[3] = ID_HCI_REV >> 8, r[4] = ID_LMP_VER;
		r[5] = ID_MANUF & 0xff, r[6] = ID_MANUF >> 8, r[7] = ID_LMP_SUBVER & 0xff, r[8] = ID_LMP_SUBVER >> 8;
		n = 9;
		break;
	case 0x1002:    /* Read Local Supported Commands: everything but LE Read Buffer Size v2 (41.5) */
		memset(r + 1, 0xff, 64);
		r[1 + 41] &= (uint8_t)~0x20;
		n = 65;
		break;
	case 0x1003:
		memcpy(r + 1, fake_features, 8);
		n = 9;
		break;
	case 0x1004:    /* Read Local Extended Features (page) */
		r[1] = plen ? par[0] : 0, r[2] = 2;
		if (r[1] == 0)
			memcpy(r + 3, fake_features, 8);
		else if (r[1] == 1)
			r[3] = 0x0f;
		n = 11;
		break;
	case 0x1005:    /* Read Buffer Size: ACL 1021 x 8, SCO 64 x 12 */
		r[1] = 0xfd, r[2] = 0x03, r[3] = 64, r[4] = 8, r[5] = 0, r[6] = 12, r[7] = 0;
		n = 8;
		break;
	case 0x1009:    /* Read BD_ADDR */
		for (int i = 0; i < 6; i++)
			r[1 + i] = opt_bdaddr[5 - i];
		n = 7;
		break;
	case 0x2002:    /* LE Read Buffer Size v1 */
		r[1] = 0xfb, r[2] = 0, r[3] = 8;
		n = 4;
		break;
	case 0x2060:    /* LE Read Buffer Size v2 */
		r[1] = 0xfb, r[2] = 0, r[3] = 8;
		n = 7;
		break;
	case 0x2003:
		memcpy(r + 1, fake_le_features, 8);
		n = 9;
		break;
	case 0x0c5a:    /* Read Default Erroneous Data Reporting: [status][mode 0] */
		n = 2;
		break;
	case 0x200f:    /* LE Read Filter Accept List Size */
		r[1] = 16;
		n = 2;
		break;
	case 0x201c:    /* LE Read Supported States */
		memset(r + 1, 0xff, 8);
		n = 9;
		break;
	case 0x204a:    /* LE Read Periodic Advertiser List Size */
		r[1] = 8;
		n = 2;
		break;
	case 0x200b: case 0x2036: case 0x2041:  /* LE scan / adv params: status only */
		break;
	case 0x0c14:    /* Read Local Name */
		strcpy((char *)r + 1, "BCM4357B1 37.4MHz Hercules-E USI");
		n = 249;
		break;
	case 0xfc79:    /* BCM Read Verbose Config Version Info: chip id, target, build base, build num */
		r[1] = 0x57, r[2] = 0x07, r[3] = 0x13, r[4] = 0x00, r[5] = 0x10, r[6] = 0x01;
		n = 7;
		break;
	case 0xfce2:    /* Apple "Olympic" multiplexed VSC: the reply must echo the sub-opcode (else "timedout waiting
			 * for controller", e.g. BD_VSC_OLYMPIC_ANTENNA_PREF_B, sub-op 0x15, and a restart) */
		/* fall through */
	case 0xfce9:    /* also multiplexed; byte-stream callbacks (_BCM_ByteStreamEventParser) want more than
			 * one byte: [status][sub-op][8 zero bytes] */
		r[1] = plen ? par[0] : 0;
		n = 10;
		break;
	case 0xfd48:    /* BCM LE create multi-adv instance: [status][instance] (bcm_vsc.c asserts on the byte) */
		{
			static uint8_t inst;
			r[1] = (uint8_t)(++inst ? inst : ++inst);
			n = 2;
		}
		break;
	default:
		/* other Broadcom VSCs: status only until a parser is seen to need more (record.sh) */
		/* byte-stream VSCs (_BCM_ByteStreamEventParser, e.g. BD_VSC_FACTORY_CAL_READ_TABLE 0xFD1B) need
		 * more than one byte or the caller times out and bluetoothd restarts: [status][first param][zeros] */
		if (ogf == 0x3f) {
			r[1] = plen ? par[0] : 0;
			n = 32;
		}
		break;
	}
	/* link control commands and the LE connection-type ones answer with Command Status */
	if (ogf == 0x01 && op != 0x0402 && op != 0x040b && op != 0x040c && op != 0x040d && op != 0x040e &&
	    op != 0x041a && op != 0x0434) {
		host_cs(op, 0);
		say("fake: Command Status for %#06x (len %zu)", op, plen);
		return;
	}
	if (op == 0x200d || op == 0x2016 || op == 0x2019 || op == 0x2025 || op == 0x2026 || op == 0x2043 ||
	    op == 0x2032 || op == 0x2013) {
		host_cs(op, 0);
		say("fake: Command Status for %#06x (len %zu)", op, plen);
		return;
	}
	host_cc(op, r, n);
	if (opt_verbose || ogf == 0x3f)
		say("%s: Command Complete for %#06x (ogf %#x ocf %#x, %zu param bytes in, %zu out)",
		    opt_fake ? "fake" : "vsc", op, ogf, op & 0x3ff, plen, n);
}

/* real mode: Broadcom vendor commands with a Qualcomm meaning */
#include "le_ext.c"

/* one line per classic-BT milestone (inquiry, scan enable, connections, pairing), also without -v */
static unsigned inq_results;

static void
trace_cmd(uint16_t op, const uint8_t *par, size_t plen)
{
	switch (op) {
	case 0x0401: say("BR: inquiry started"); inq_results = 0; break;
	case 0x0402: say("BR: inquiry cancelled"); break;
	case 0x0405: say("BR: create connection"); break;
	case 0x0409: say("BR: accept connection request (role %u)", plen >= 7 ? par[6] : 0); break;
	case 0x040a: say("BR: reject connection request"); break;
	case 0x0411: say("BR: authentication requested"); break;
	case 0x042b: say("BR: IO capability reply (cap %u)", plen >= 7 ? par[6] : 0); break;
	case 0x042c: say("BR: user confirmation reply (yes)"); break;
	case 0x042d: say("BR: user confirmation negative reply"); break;
	case 0x0c1a: say("BR: write scan enable %#x (1 inquiry scan = discoverable, 2 page scan = connectable)",
	    plen ? par[0] : 0); break;
	case 0x0c13: say("BR: local name set"); break;
	default: break;
	}
}

static void
trace_event(const uint8_t *p, size_t n)
{
	if (p[0] != 4 || n < 3)
		return;
	switch (p[1]) {
	case 0x01: say("BR: inquiry complete, %u results", inq_results); break;
	case 0x02: case 0x22: case 0x2f:
		if (inq_results++ < 20)
			say("BR: inquiry result (event %#x, %u so far)", p[1], inq_results);
		break;
	case 0x03: say("BR: connection complete status %#x handle %#x", p[3], n >= 6 ? p[4] | (p[5] << 8) : 0); break;
	case 0x04: say("BR: connection request (class %02x%02x%02x)", n >= 12 ? p[11] : 0, n >= 11 ? p[10] : 0,
	    n >= 10 ? p[9] : 0); break;
	case 0x05: say("BR: disconnection complete reason %#x", n >= 7 ? p[6] : 0); break;
	case 0x06: say("BR: authentication complete status %#x", p[3]); break;
	case 0x31: say("BR: IO capability request"); break;
	case 0x32: say("BR: IO capability response (remote cap %u)", n >= 10 ? p[9] : 0); break;
	case 0x33: say("BR: user confirmation request"); break;
	case 0x36: say("BR: simple pairing complete status %#x", p[3]); break;
	case 0x18: say("BR: link key notification"); break;
	case 0x17: say("BR: link key request"); break;
	case 0x16: say("BR: PIN code request"); break;
	default: break;
	}
}

static int
translate_vsc(uint16_t op, const uint8_t *par, size_t plen)
{
	if (le_vsc(op, par, plen))
		return 0;
	switch (op) {
	case 0xfc01:    /* BCM Write_BD_ADDR (LE order) -> QCA 0xFC14 */
		if (plen >= 6) {
			for (int i = 0; i < 6; i++)
				opt_bdaddr[5 - i] = par[i];
			if (qca_cmd(0xfc14, opt_bdaddr, 6) < 0)   /* MSB first; its answer is dropped in from_controller */
				return -1;
			say("vsc: Write_BD_ADDR -> QCA 0xFC14");
		}
		answer_locally(op, par, plen);
		return 0;
	default:
		answer_locally(op, par, plen);
		return 0;
	}
}

/* the opcode of a Command Complete / Status we must not pass on (answers to our own commands) */
static int drop_vendor_events = 1;

/* "scan N" on the control socket: btshim's own passive LE scan for N seconds, straight to the WCN (a radio
 * test independent of bluetoothd): its Command Completes and the advertising reports are kept from
 * bluetoothd and counted */
static int scan_test;
static time_t scan_end;
static unsigned scan_reports, scan_devs;
static uint8_t scan_seen[64][6];

static int scan_ext;     /* legacy LE scan commands answered 0x0C (Command Disallowed): use the extended ones */

static void
scan_cmd(int on)
{
	if (scan_ext) {
		if (on) {
			/* LE Set Extended Scan Parameters: public, accept all, 1M PHY, passive, 60 ms / 60 ms */
			uint8_t sp[8] = { 0x00, 0x00, 0x01, 0x00, 0x60, 0x00, 0x60, 0x00 };
			qca_cmd(0x2041, sp, 8);
			usleep(50000);
		}
		uint8_t se[6] = { (uint8_t)on, 0x00, 0, 0, 0, 0 };
		qca_cmd(0x2042, se, 6);
		return;
	}
	if (on) {
		/* LE Set Scan Parameters: passive, interval = window = 0x60 (60 ms), public, accept all */
		uint8_t sp[7] = { 0x00, 0x60, 0x00, 0x60, 0x00, 0x00, 0x00 };
		qca_cmd(0x200b, sp, 7);
		usleep(50000);
	}
	uint8_t se[2] = { (uint8_t)on, 0x00 };
	qca_cmd(0x200c, se, 2);
}

static void
scan_seen_add(const uint8_t *addr, unsigned type, int rssi)
{
	scan_reports++;
	for (unsigned k = 0; k < scan_devs; k++)
		if (!memcmp(scan_seen[k], addr, 6))
			return;
	if (scan_devs < 64) {
		memcpy(scan_seen[scan_devs++], addr, 6);
		say("scan test: device %u, address type %u, rssi %d dBm", scan_devs, type, rssi);
	}
}

static void
from_controller(const uint8_t *p, size_t n)
{
	if (scan_test) {
		uint16_t cop = (uint16_t)(p[4] | (p[5] << 8));
		if (p[0] == 4 && p[1] == 0x0e && n >= 7 && (cop == 0x200b || cop == 0x200c || cop == 0x2041 || cop == 0x2042)) {
			say("scan test: %#06x -> status %#x", cop, p[6]);
			if (p[6] == 0x0c && (cop == 0x200b || cop == 0x200c) && scan_test == 1 && !scan_ext) {
				say("scan test: legacy scan disallowed, switching to extended scanning");
				scan_ext = 1;
				scan_cmd(1);
			}
			return;
		}
		if (p[0] == 4 && p[1] == 0x3e && n >= 5 && p[3] == 0x0d) {      /* LE Extended Advertising Report */
			unsigned nr = p[4];
			const uint8_t *r = p + 5;
			for (unsigned i = 0; i < nr && r + 24 <= p + n; i++) {
				scan_seen_add(r + 3, r[2], (int8_t)r[13]);
				r += 24 + r[23];
			}
			return;
		}
		if (p[0] == 4 && p[1] == 0x3e && n >= 5 && p[3] == 0x02) {      /* LE Advertising Report */
			unsigned nr = p[4];
			const uint8_t *r = p + 5;
			for (unsigned i = 0; i < nr && r + 9 <= p + n; i++) {
				unsigned dl = r[8];
				int8_t rssi = (r + 9 + dl < p + n) ? (int8_t)r[9 + dl] : 0;
				scan_seen_add(r + 2, r[1], rssi);
				r += 10 + dl;
			}
			return;
		}
	}

	if (p[0] == 4 && p[1] == 0xff && drop_vendor_events) {
		if (opt_verbose)
			hexdump("drop VSE", p, n);
		return;
	}
	trace_event(p, n);
	if (own_event(p, n) || le_event(p, n))
		return;
	if (p[0] == 2 && n >= 5) {
		unsigned h = (unsigned)(p[1] | (p[2] << 8)) & 0x0fff;
		if (h == 0x2edc || h == 0x2edd) {       /* QCA_DEBUG_HANDLE / memdump: not for bluetoothd */
			static unsigned dropped;
			if (dropped++ % 100 == 0)
				say("dropped SoC debug ACL (handle %#x), %u so far", h, dropped);
			return;
		}
	}
	if (p[0] == 4 && p[1] == 0x0f && n >= 7 && p[3] != 0) {
		/* Command Status with an error: an OGF 3 (controller & baseband) or OGF 8 (LE) command the WCN
		 * does not know (those normally end with Command Complete): report success as a Command Complete */
		uint16_t op = (uint16_t)(p[5] | (p[6] << 8));
		say("WCN: Command Status %#x for %#06x", p[3], op);
		if ((op >> 10) == 0x03 || (op >> 10) == 0x08) {
			host_cc(op, (const uint8_t *)"", 1);
			return;
		}
	}
	if (p[0] == 4 && p[1] == 0x0e && n == 7 && p[6] != 0 && (p[4] | (p[5] << 8)) < 0xfc00) {
		/* a write-type command (status is its only return parameter) the WCN refuses, e.g. Write Default
		 * Erroneous Data Reporting 0x0C5B (unknown command): bluetoothd treats an error in its init chain as
		 * fatal and restarts, so report success */
		uint8_t q[7];
		memcpy(q, p, 7);
		say("WCN refused %#06x with status %#x: reported as success", q[4] | (q[5] << 8), q[6]);
		q[6] = 0;
		to_host(q, 7);
		return;
	}
	if (p[0] == 4 && p[1] == 0x0e && n >= 6) {
		uint16_t op = (uint16_t)(p[4] | (p[5] << 8));
		if (op >= 0xfc00) {             /* our 0xFC14 etc. */
			if (opt_verbose)
				hexdump("drop CC", p, n);
			return;
		}
		if (op == 0x1002 && n >= 7 + 64 && p[6] == 0) {
			/* Read Local Supported Commands: bluetoothd skips (and then fails its init chain on) commands
			 * whose bit is clear; claim Read/Write Default Erroneous Data Reporting (octet 18 bits 2, 3),
			 * which btshim answers itself */
			uint8_t q[7 + 64];
			memcpy(q, p, sizeof(q));
			q[7 + 18] |= 0x0c;
			to_host(q, sizeof(q));
			return;
		}
		if (op == 0x1001 && n >= 15) {  /* Read Local Version: report the Broadcom identity */
			uint8_t q[16];
			memcpy(q, p, 15);
			/* 04 0e 0c 01 01 10 | status hci_ver rev(2) lmp_ver manufacturer(2) subversion(2) */
			say("Read Local Version from the WCN: manufacturer %u subversion %#x -> reported as Broadcom",
			    q[11] | (q[12] << 8), q[13] | (q[14] << 8));
			q[11] = ID_MANUF & 0xff, q[12] = ID_MANUF >> 8;
			q[13] = ID_LMP_SUBVER & 0xff, q[14] = ID_LMP_SUBVER >> 8;
			to_host(q, 15);
			return;
		}
	}
	to_host(p, n);
}

static void
from_host(const uint8_t *p, size_t n)
{
	if (opt_verbose || opt_fake)
		hexdump("H>  ", p, n);
	if (p[0] == 1 && n >= 4) {
		uint16_t op = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *par = p + 4;
		size_t plen = n - 4;
		if (opt_fake) {
			answer_locally(op, par, plen);
			return;
		}
		if ((op >> 10) == 0x3f) {
			translate_vsc(op, par, plen);
			return;
		}
		trace_cmd(op, par, plen);
		if ((op >> 10) == 0x08 && le_legacy(op, par, plen))
			return;
		/* commands the WCN3988 does not implement but bluetoothd's init chain requires to succeed */
		if (op == 0x0c5b || op == 0x0c5a) {     /* Write / Read Default Erroneous Data Reporting */
			say("answering %#06x locally (not in the WCN)", op);
			answer_locally(op, par, plen);
			return;
		}
	}
	if (opt_fake || !controller_up)
		return;         /* ACL/SCO/ISO to nowhere */
	c_write(p, n);
}

/* ---------------------------------------------------------------- daemon */

static struct h4 h_h4;

static int
make_fifo(const char *path)
{
	if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
		say("mkfifo %s: %s", path, strerror(errno));
		return -1;
	}
	chmod(path, 0666);
	/* O_RDWR: a FIFO opened this way never blocks and always has a reader and a writer, so
	 * bluetoothd's O_WRONLY open does not hang and its reader never sees EOF */
	int fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		say("open %s: %s", path, strerror(errno));
	return fd;
}

static int
ctl_listen(void)
{
	unlink(CTL_SOCK);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strlcpy(a.sun_path, CTL_SOCK, sizeof(a.sun_path));
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
		say("control socket: %s", strerror(errno));
		return -1;
	}
	chmod(CTL_SOCK, 0666);
	return s;
}

/* real mode: "boot"/"init" requests that came before the controller was up wait here */
static int pending[64], npending, bringup_failed;

static void
ctl_reply(int c, int result, const char *line)
{
	say("bluetool request \"%s\" -> %d", line, result);
	char out[32];
	int k = snprintf(out, sizeof(out), "%d\n", result);
	(void)write(c, out, (size_t)k);
	close(c);
}

/* one request line from "btshim bluetool": boot | init | deepsleep | reset | cmd <text> */
static void
ctl_serve(int s)
{
	int c = accept(s, NULL, NULL);
	if (c < 0)
		return;
	char line[256];
	ssize_t n = read(c, line, sizeof(line) - 1);
	if (n <= 0) {
		close(c);
		return;
	}
	line[n] = 0;
	line[strcspn(line, "\r\n")] = 0;
	if (!opt_fake && !controller_up && (!strcmp(line, "boot") || !strcmp(line, "init"))) {
		if (bringup_failed) {
			ctl_reply(c, 1, line);
		} else if (npending < (int)(sizeof(pending) / sizeof(pending[0]))) {
			say("bluetool request \"%s\": waits for the bring-up", line);
			pending[npending++] = c;
		} else {
			ctl_reply(c, 1, line);
		}
		return;
	}
	if (!strncmp(line, "scan", 4)) {
		int secs = atoi(line + 4);
		if (!controller_up || opt_fake) {
			ctl_reply(c, 1, line);
			return;
		}
		scan_test = 1;
		scan_reports = scan_devs = 0;
		scan_end = time(NULL) + (secs > 0 ? secs : 10);
		scan_cmd(1);
		say("scan test: started for %d s", secs > 0 ? secs : 10);
		ctl_reply(c, 0, line);
		return;
	}
	/* deepsleep, reset and RunCommand text: nothing to do yet (the WCN stays up) */
	ctl_reply(c, 0, line);
}

static long
boot_age(void)
{
	struct timeval bt;
	size_t sz = sizeof(bt);
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };
	if (sysctl(mib, 2, &bt, &sz, NULL, 0) < 0)
		return 1 << 20;
	return (long)(time(NULL) - bt.tv_sec);
}

/* bluetoothd's start gate: /dev/uart.bluetooth (or cu.bluetooth) must exist; it is only access()ed.
 * The kernel should create it; if it did not, try a node of our own (devfs: gone at reboot). */
static void
uart_node(void)
{
	if (access("/dev/uart.bluetooth", F_OK) == 0 || access("/dev/cu.bluetooth", F_OK) == 0)
		return;
	if (mknod("/dev/uart.bluetooth", S_IFCHR | 0666, makedev(3, 2)) == 0)
		say("made /dev/uart.bluetooth (char 3,2) for bluetoothd's start check");
	else if (symlink("/dev/null", "/dev/uart.bluetooth") == 0)
		say("made /dev/uart.bluetooth -> /dev/null for bluetoothd's start check");
	else
		say("cannot make /dev/uart.bluetooth (%s): bluetoothd will say \"No bluetooth on this device\"",
		    strerror(errno));
}

static int
daemon_main(void)
{
	char in[256], out[256];
	mkdir(RUN_DIR, 0755);
	chmod(RUN_DIR, 0755);
	snprintf(in, sizeof(in), "%s-in", opt_prefix);
	snprintf(out, sizeof(out), "%s-out", opt_prefix);
	host_rd = make_fifo(in);
	host_wr = make_fifo(out);
	int ls = ctl_listen();
	if (host_rd < 0 || host_wr < 0 || ls < 0)
		return 1;
	uart_node();
	if (log_fd >= 0)
		fchmod(log_fd, 0666);   /* "btshim bluetool" runs as mobile and logs here too */
	say("btshim daemon: %s mode, host FIFOs %s / %s, speed %u", opt_fake ? "FAKE" : "real", in, out, opt_speed);
	signal(SIGPIPE, SIG_IGN);
	uint8_t buf[4096];
	/* real mode: the bring-up waits until the boot is opt_settle s old (like the other daemons);
	 * what bluetoothd writes before that is held, not answered */
	static uint8_t held[65536];
	size_t nheld = 0;
	int tries = 0;
	time_t next_try = time(NULL) + (boot_age() < opt_settle ? opt_settle - boot_age() : 0);
	if (!opt_fake)
		say("bring-up in %ld s", (long)(next_try - time(NULL)));
	for (;;) {
		if (!opt_fake && !controller_up && !bringup_failed && time(NULL) >= next_try) {
			if (controller_bringup() == 0) {
				say("controller up: %d bluetool request(s) answered, %zu held bytes passed on", npending, nheld);
				for (int i = 0; i < npending; i++)
					ctl_reply(pending[i], 0, "boot/init");
				npending = 0;
				h4_feed(&h_h4, held, nheld, from_host);
				nheld = 0;
			} else {
				/* never answer 1: bluetoothd asks BlueTool again at once and spins (seen: a request every
				 * 12 ms); pending requests keep waiting and the bring-up is retried */
				++tries;
				int wait = tries < 5 ? 30 : 120;
				say("bring-up failed (try %d): again in %d s", tries, wait);
				next_try = time(NULL) + wait;
			}
		}
		fd_set r;
		FD_ZERO(&r);
		FD_SET(host_rd, &r);
		FD_SET(ls, &r);
		int mx = host_rd > ls ? host_rd : ls;
		if (controller_up && ctl_notify[0] >= 0) {
			FD_SET(ctl_notify[0], &r);
			if (ctl_notify[0] > mx)
				mx = ctl_notify[0];
		}
		if (scan_test == 1 && time(NULL) >= scan_end) {
			scan_cmd(0);
			scan_test = 2;          /* still swallow the disable's Command Complete */
			scan_end = time(NULL) + 2;
			say("scan test: done, %u advertising reports from %u devices", scan_reports, scan_devs);
		} else if (scan_test == 2 && time(NULL) >= scan_end) {
			scan_test = 0;
		}
		struct timeval tv = { 1, 0 };
		int k = select(mx + 1, &r, NULL, NULL, opt_fake ? NULL : &tv);
		if (controller_up && !opt_fake)
			own_pump();     /* time out a lost answer to one of our own commands */
		if (k < 0) {
			if (errno == EINTR)
				continue;
			say("select: %s", strerror(errno));
			return 1;
		}
		if (FD_ISSET(ls, &r))
			ctl_serve(ls);
		if (FD_ISSET(host_rd, &r)) {
			ssize_t n = read(host_rd, buf, sizeof(buf));
			if (n > 0 && !opt_fake && !controller_up) {
				if (nheld + (size_t)n <= sizeof(held)) {
					memcpy(held + nheld, buf, (size_t)n);
					nheld += (size_t)n;
				}
			} else if (n > 0) {
				h4_feed(&h_h4, buf, (size_t)n, from_host);
			}
		}
		if (controller_up && ctl_notify[0] >= 0 && FD_ISSET(ctl_notify[0], &r)) {
			while (read(ctl_notify[0], buf, sizeof(buf)) > 0)
				;
			size_t n;
			while ((n = qca_rx_take(buf, sizeof(buf))) > 0)
				h4_feed(&c_h4, buf, n, from_controller);
		}
	}
}

/* ---------------------------------------------------------------- com.apple.BlueTool */

static int
ask_daemon(const char *req)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strlcpy(a.sun_path, CTL_SOCK, sizeof(a.sun_path));
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		say("bluetool: daemon not reachable: %s", strerror(errno));
		if (s >= 0)
			close(s);
		return 1;
	}
	(void)write(s, req, strlen(req));
	(void)write(s, "\n", 1);
	char b[32] = { 0 };
	struct timeval tv = { 300, 0 };  /* the daemon waits for the boot to settle, then ~25 s of download */
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	ssize_t n = read(s, b, sizeof(b) - 1);
	close(s);
	return n > 0 ? atoi(b) : 1;
}

static void
bluetool_message(xpc_connection_t peer, xpc_object_t msg)
{
	const char *id = xpc_dictionary_get_string(msg, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(msg, "kMsgArgs");
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	if (!reply)
		return;
	if (id && !strcmp(id, "RunBuiltinScript")) {
		const char *script = args ? xpc_dictionary_get_string(args, "script") : NULL;
		const char *addr = args ? xpc_dictionary_get_string(args, "btaddress") : NULL;
		say("bluetool: RunBuiltinScript \"%s\" btaddress %s", script ? script : "?", addr ? addr : "-");
		int r = script ? ask_daemon(script) : 1;
		xpc_dictionary_set_int64(reply, "result", r);
		xpc_dictionary_set_int64(reply, "manufacturer", 0);
		xpc_dictionary_set_string(reply, "firmware", "QCA WCN3988 apbtfw (btshim)");
	} else if (id && !strcmp(id, "RunCommand")) {
		const char *cmd = args ? xpc_dictionary_get_string(args, "command") : NULL;
		say("bluetool: RunCommand \"%s\"", cmd ? cmd : "?");
		xpc_dictionary_set_int64(reply, "result", 0);
		xpc_dictionary_set_string(reply, "output", "");
	} else {
		say("bluetool: message %s ignored", id ? id : "(no kMsgId)");
		xpc_dictionary_set_int64(reply, "result", 0);
	}
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
}

static int
bluetool_main(void)
{
	xpc_connection_t l = xpc_connection_create_mach_service("com.apple.BlueTool", NULL,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
			return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) == XPC_TYPE_DICTIONARY)
				bluetool_message(peer, m);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	say("bluetool: listening as com.apple.BlueTool");
	dispatch_main();
}

/* ---------------------------------------------------------------- main */

static void
usage(void)
{
	fprintf(stderr, "usage: btshim daemon [-F] [-d dir] [-r tlv] [-n nvm] [-a addr] [-s speed] "
	    "[-p fifo-prefix] [-w settle-s] [-v] [-I] | bluetool\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		usage();
	const char *mode = argv[1];
	const char *env = getenv("BT_DEVICE_ADDRESS");
	if (env && parse_addr(env, opt_bdaddr) == 0)
		opt_have_addr = 1;
	int c;
	optind = 2;
	while ((c = getopt(argc, argv, "Fd:r:n:a:s:p:vIw:")) != -1) {
		switch (c) {
		case 'F': opt_fake = 1; break;
		case 'd': opt_dir = optarg; break;
		case 'w': opt_settle = strtol(optarg, NULL, 0); break;
		case 'r': opt_ramp = optarg; break;
		case 'n': opt_nvm = optarg; break;
		case 'a':
			if (parse_addr(optarg, opt_bdaddr) < 0)
				usage();
			opt_have_addr = 1;
			break;
		case 's': opt_speed = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'p': opt_prefix = optarg; break;
		case 'v': opt_verbose = qca_verbose = 1; break;
		case 'I': qca_keep_ibs = 1; break;
		default: usage();
		}
	}
	if (!strcmp(mode, "bdstart")) {
		/* bluetoothd's launchd program: launchd drops DYLD_* for system daemons, so set it here and exec
		 * the real bluetoothd (same pid: the job's Mach services stay). btbridge.dylib answers bluetoothd's
		 * local-mac-address lookup (its BT_DEVICE_ADDRESS is zeroed in release builds). */
		setenv("DYLD_INSERT_LIBRARIES", "/redwood/lib/btbridge.dylib", 1);
		char *bargv[] = { "/usr/sbin/bluetoothd", NULL };
		execv(bargv[0], bargv);
		perror("btshim bdstart: exec bluetoothd");
		return 1;
	}
	if (!strcmp(mode, "daemon"))
		return daemon_main();
	if (!strcmp(mode, "scan")) {     /* btshim scan [seconds]: the daemon's LE scan test, result in its log */
		char req[32];
		snprintf(req, sizeof(req), "scan %s", argc > 2 ? argv[2] : "10");
		int r = ask_daemon(req);
		printf("scan test %s (see /var/tmp/btshim.log)\n", r == 0 ? "started" : "refused");
		return r;
	}
	if (!strcmp(mode, "bluetool"))
		return bluetool_main();
	usage();
	return 2;
}
