/*
 * fpd: the in-display fingerprint sensor for iOS (kernel 0261's Goodix TA client), version 2.
 *
 * Start: every *.so in /var/root/fp (the sensor's factory calibration and the enrolled templates, copies
 * made from the TA's RAM store; Android's persist is never read or written) goes into the kernel's file
 * store (kern.sweet_qsee_file), then the TA is initialised (kern.sweet_qsee = 10, retried with back-off).
 *
 * Lock screen: fpd sleeps on darwin notifications (screen blanked / lock state / first unlock / prefs) and
 * only while the screen is on, MobileKeyBag says locked (1), the device was unlocked since boot, a finger
 * is enrolled, unlocking is enabled in the prefs and the kernel has not locked fingerprints out, it polls
 * kern.sweet_fod_seq every 30 ms. A new press posts com.sapphire.fp.press and runs op 26 (spot lit, match
 * until match / lift / 3 s): a match runs op 27 (the kernel's keybag unlocks) and posts
 * com.sapphire.fp.unlocked (SpringBoard's SapphireFingerprint tweak dismisses the lock screen); no match
 * posts com.sapphire.fp.nomatch (state of com.sapphire.fp.fails = failures since the last unlock), a
 * lockout com.sapphire.fp.locked_out. com.sapphire.fp.state (notify state) is 1 while fingerprint unlock
 * is available, else 0. With the "screenOffWake" pref the sensor is also watched every 60 ms while the
 * screen is off: a press posts com.sapphire.fp.wake (the tweak lights the screen) and, once the screen is
 * on with the finger still down, authenticates. Whether presses arrive at all with the panel off depends
 * on the touch controller: logged ("press ... with the screen off").
 *
 * After a match the TA may have refined the template (study): dirty template / auth token / dynamic
 * base files are copied back to /var/root/fp a few seconds later (the first version of every file
 * replaced this way is kept in /var/root/fp/orig/). The factory calibration is never written.
 *
 * Control socket /var/run/fpd.sock (mode 0666; peers must be root or mobile), one command per line,
 * answers are lines (OK ... / ERR reason; lists end with END):
 *   STATUS                    OK key=value ... (enabled ready lock ubs screen enrolling testing fingers max
 *                             fails lockedout unlocks nomatch lockouts offpresses screenoffwake sensor version)
 *   LIST                      FINGER slot fid size name ... END   (fid 0: not known yet)
 *   RENAME slot name          OK
 *   DELETE slot / DELETE_ALL  OK (files removed from /var/root/fp and from the kernel's store, TA re-initialised)
 *   ENROLL_BEGIN [name]       OK enrolled-count, then per touch PRESS and PROGRESS remaining code first
 *                             (remaining -1 + gf error code when the touch was refused), at the end DONE slot
 *                             or FAILED reason; only while unlocked, at most 5 fingers (the TA scans finger_0_0..4)
 *   ENROLL_CANCEL             CANCELLED
 *   TEST                      OK, PLACE, then PRESS and TEST MATCH fid slot name / TEST NOMATCH / TEST LIFTED /
 *                             TEST TIMEOUT / TEST ERR code  (op 20 with the spot lit: no unlock, no lockout count)
 *   TEST_CANCEL               CANCELLED
 *   LOG [n]                   L line ... END (the last n lines of fpd.log)
 *   PREFS                     OK (prefs re-read; also on darwin notification com.sapphire.fp.prefs)
 * Lock-screen authentication pauses while enrolling or testing.
 *
 * Files: /var/root/fp/fingers.plist {slot: {name, fid, created}}; prefs read from
 * /var/mobile/Library/Preferences/com.sapphire.fingerprint.plist (unlockEnabled, screenOffWake, haptics: vibd
 * presets success on unlock, error on no match, long on lockout, light / warning per enroll touch);
 * /var/root/fp/fpd.noscreen = ignore the screen state.
 * Log: os_log (subsystem com.sapphire.fpd) and /var/root/fp/fpd.log (256 KB, then fpd.log.1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysctl.h>
#include <notify.h>
#include <os/log.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

#define FP_DIR          "/var/root/fp"
#define ORIG_DIR        FP_DIR "/orig"
#define NAMES_PATH      FP_DIR "/fingers.plist"
#define LOG_PATH        FP_DIR "/fpd.log"
#define LOG_OLD         FP_DIR "/fpd.log.1"
#define PREFS_PATH      "/var/mobile/Library/Preferences/com.sapphire.fingerprint.plist"
#define SOCK_PATH       "/var/run/fpd.sock"
#define LOG_MAX         (256 * 1024)
#define FILE_MAX        (8 * 1024 * 1024)
#define MAX_FINGERS     5
#define POLL_MS         30
#define POLL_OFF_MS     60
#define RECHECK_TICKS   100             /* lock state re-read every 3 s while polling */
#define SLOW_RECHECK_S  30              /* and every 30 s otherwise, in case a notification was missed */
#define BOOT_SETTLE_S   120             /* notifications reach jobs started early only later (arplay) */
#define ENROLL_IDLE_S   90
#define TEST_IDLE_S     20

#define OP_INIT         10
#define OP_STOP         12
#define OP_ENROLL_BEGIN 14
#define OP_ENROLL_TOUCH 15
#define OP_ENROLL_END   16
#define OP_AUTH_ARG     20
#define OP_AUTH         26
#define OP_UNLOCK       27
#define ARG_SPOT        0x100           /* kern.sweet_qsee_arg bit 8: the spot lit by local HBM */
#define GF_NOT_MATCH    (-1008)
#define GF_LIFTED       (-4)

#define N_UNLOCKED      "com.sapphire.fp.unlocked"
#define N_NOMATCH       "com.sapphire.fp.nomatch"
#define N_LOCKED_OUT    "com.sapphire.fp.locked_out"
#define N_PRESS         "com.sapphire.fp.press"
#define N_WAKE          "com.sapphire.fp.wake"
#define N_STATE         "com.sapphire.fp.state"
#define N_FAILS         "com.sapphire.fp.fails"
#define N_PREFS         "com.sapphire.fp.prefs"
#define N_BLANKED       "com.apple.springboard.hasBlankedScreen"
#define N_DISPLAY       "com.apple.iokit.hid.displayStatus"
#define N_KB_LOCK       "com.apple.mobile.keybagd.lock_status"
#define N_KB_FIRST      "com.apple.mobile.keybagd.first_unlock"
#define N_SB_LOCK       "com.apple.springboard.lockstate"

static os_log_t olog;

static void
logf_(const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	os_log(olog, "%{public}s", msg);

	struct stat st;
	if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX) {
		rename(LOG_PATH, LOG_OLD);
	}
	int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0) {
		return;
	}
	char line[600];
	struct timeval tv;
	gettimeofday(&tv, NULL);
	struct tm tm;
	localtime_r(&tv.tv_sec, &tm);
	int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", tm.tm_year + 1900, tm.tm_mon + 1,
	    tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000), msg);
	if (n > 0) {
		write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
	}
	close(fd);
}
#define LOG(...) logf_(__VA_ARGS__)

/* ------------------------------------------------------------------ sysctls */

struct oid { const char *name; int mib[CTL_MAXNAME]; size_t len; bool ok; };
static struct oid o_qsee = { .name = "kern.sweet_qsee" }, o_press = { .name = "kern.sweet_fod_press" },
                  o_seq = { .name = "kern.sweet_fod_seq" }, o_arg = { .name = "kern.sweet_qsee_arg" };

static bool
oid_init(struct oid *o)
{
	o->len = CTL_MAXNAME;
	o->ok = sysctlnametomib(o->name, o->mib, &o->len) == 0;
	return o->ok;
}

static int
oid_read(struct oid *o, int *v)
{
	size_t l = sizeof *v;
	return sysctl(o->mib, (u_int)o->len, v, &l, NULL, 0) == 0 ? 0 : errno;
}

static int
oid_write(struct oid *o, int v)
{
	return sysctl(o->mib, (u_int)o->len, NULL, NULL, &v, sizeof v) == 0 ? 0 : errno;
}

/* writes op: 0, or the errno of the write (the op's own result for 27, EPERM for a lockout on 26) */
static int
qsee_write(int op)
{
	return oid_write(&o_qsee, op);
}

/* op, then its read-back value; returns the write's errno */
static int
qsee_op_value(int op, int *v)
{
	int err = qsee_write(op);
	*v = 0;
	if (err == 0) {
		err = oid_read(&o_qsee, v);
	}
	return err;
}

static int
fod_press(void)
{
	int p = 0;
	oid_read(&o_press, &p);
	return p;
}

/* ------------------------------------------------------------------ MobileKeyBag (dlopen) */

static int (*MKBGetDeviceLockState_)(void *);
static int (*MKBDeviceUnlockedSinceBoot_)(void);

static bool
mkb_init(void)
{
	void *h = dlopen("/System/Library/PrivateFrameworks/MobileKeyBag.framework/MobileKeyBag", RTLD_LAZY);
	if (h != NULL) {
		MKBGetDeviceLockState_ = (int (*)(void *))dlsym(h, "MKBGetDeviceLockState");
		MKBDeviceUnlockedSinceBoot_ = (int (*)(void))dlsym(h, "MKBDeviceUnlockedSinceBoot");
	}
	return MKBGetDeviceLockState_ != NULL && MKBDeviceUnlockedSinceBoot_ != NULL;
}

/* ------------------------------------------------------------------ files */

static bool
ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && strcmp(s + a - b, suf) == 0;
}

/* finger_0_N.so (N 0..4) -> N; anything else -1 */
static int
template_slot(const char *n)
{
	if (strncmp(n, "finger_0_", 9) != 0 || n[9] < '0' || n[9] > '0' + MAX_FINGERS - 1 || strcmp(n + 10, ".so") != 0) {
		return -1;
	}
	return n[9] - '0';
}

/* files of a slot: the template, its backup, its extra info (+ backup) */
static void
slot_files(int slot, char names[4][48])
{
	snprintf(names[0], 48, "finger_0_%d.so", slot);
	snprintf(names[1], 48, "finger_0_%d_bak.so", slot);
	snprintf(names[2], 48, "finger_extra_info_0_%d.so", slot);
	snprintf(names[3], 48, "finger_extra_info_0_%d_bak.so", slot);
}

/* bit mask of the slots with a template in /var/root/fp */
static unsigned
template_mask(void)
{
	unsigned m = 0;
	for (int s = 0; s < MAX_FINGERS; s++) {
		char p[128];
		struct stat st;
		snprintf(p, sizeof p, FP_DIR "/finger_0_%d.so", s);
		if (stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
			m |= 1u << s;
		}
	}
	return m;
}

static int
popcount5(unsigned m)
{
	int n = 0;
	for (int s = 0; s < MAX_FINGERS; s++) {
		n += (m >> s) & 1;
	}
	return n;
}

static int
load_one(const char *dir, const char *name)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return errno;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > FILE_MAX) {
		close(fd);
		return EINVAL;
	}
	size_t nl = strlen(name) + 1, n = (size_t)st.st_size;
	char *buf = malloc(nl + n);
	if (buf == NULL) {
		close(fd);
		return ENOMEM;
	}
	memcpy(buf, name, nl);
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, buf + nl + got, n - got);
		if (r <= 0) {
			break;
		}
		got += (size_t)r;
	}
	close(fd);
	int err = got != n ? EIO : (sysctlbyname("kern.sweet_qsee_file", NULL, NULL, buf, nl + n) == 0 ? 0 : errno);
	free(buf);
	LOG("load %s (%zu bytes): %s", name, n, err ? strerror(err) : "ok");
	return err;
}

/* a file out of the kernel's store (name + NUL, no bytes) */
static int
store_clear(const char *name)
{
	int r = sysctlbyname("kern.sweet_qsee_file", NULL, NULL, (void *)name, strlen(name) + 1) == 0 ? 0 : errno;
	LOG("store: %s cleared: %s", name, r ? strerror(r) : "ok");
	return r;
}

/* the store's list ("name size dirty source\n" per file), malloc'd, or NULL */
static char *
store_list(void)
{
	size_t l = 0;
	if (sysctlbyname("kern.sweet_qsee_files", NULL, &l, NULL, 0) != 0 || l == 0) {
		return NULL;
	}
	l += 256;
	char *b = calloc(1, l + 1);
	if (b != NULL && sysctlbyname("kern.sweet_qsee_files", b, &l, NULL, 0) != 0) {
		free(b);
		return NULL;
	}
	return b;
}

/* the kernel store already holds templates (fpd restarted): keep its copies, they may be newer */
static bool
store_has_templates(void)
{
	char *b = store_list();
	bool r = false;
	for (char *line = b; line != NULL && *line; ) {
		char *nl = strchr(line, '\n');
		if (nl != NULL) {
			*nl = 0;
		}
		char name[128];
		if (sscanf(line, "%127s", name) == 1 && template_slot(name) >= 0) {
			r = true;
		}
		line = nl != NULL ? nl + 1 : NULL;
	}
	free(b);
	return r;
}

static void
load_files(bool skip)
{
	if (skip) {
		LOG("the kernel's store already has templates: files not loaded again");
		return;
	}
	DIR *d = opendir(FP_DIR);
	if (d == NULL) {
		LOG("no %s: %s", FP_DIR, strerror(errno));
		return;
	}
	int loaded = 0, failed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !ends_with(e->d_name, ".so")) {
			continue;
		}
		if (load_one(FP_DIR, e->d_name) == 0) {
			loaded++;
		} else {
			failed++;
		}
	}
	closedir(d);
	LOG("%d files loaded, %d failed, %d templates", loaded, failed, popcount5(template_mask()));
}

/* may fpd copy this store file back to /var/root/fp? (never the factory calibration) */
static bool
savable(const char *n, bool new_files_ok)
{
	if (strncmp(n, "gf_cali", 7) == 0 || strcmp(n, "gf_extra_feature.so") == 0 || strchr(n, '/') != NULL || n[0] == '.') {
		return false;
	}
	if (strcmp(n, "gf_dynamic_persist.so") == 0 || strncmp(n, "auth_token", 10) == 0 || strncmp(n, "auth_att", 8) == 0) {
		return true;
	}
	if (strncmp(n, "finger_", 7) == 0 && ends_with(n, ".so")) {
		if (new_files_ok) {
			return true;
		}
		char p[256];
		snprintf(p, sizeof p, FP_DIR "/%s", n);
		return access(p, F_OK) == 0;    /* study updates of files we have; nothing re-created */
	}
	return false;
}

static int
copy_file(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY);
	if (in < 0) {
		return errno;
	}
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		int e = errno;
		close(in);
		return e;
	}
	static char b[65536];
	ssize_t r;
	int err = 0;
	while ((r = read(in, b, sizeof b)) > 0) {
		if (write(out, b, (size_t)r) != r) {
			err = EIO;
			break;
		}
	}
	close(in);
	close(out);
	return err;
}

/* one store file to /var/root/fp/NAME (atomically; the first version replaced is kept in orig/) */
static int
export_file(const char *name)
{
	if (sysctlbyname("kern.sweet_qsee_filesel", NULL, NULL, (void *)name, strlen(name)) != 0) {
		return errno;
	}
	size_t n = 0;
	if (sysctlbyname("kern.sweet_qsee_fileout", NULL, &n, NULL, 0) != 0) {
		return errno;
	}
	if (n == 0 || n > FILE_MAX) {
		return EINVAL;
	}
	char *buf = malloc(n);
	if (buf == NULL) {
		return ENOMEM;
	}
	if (sysctlbyname("kern.sweet_qsee_fileout", buf, &n, NULL, 0) != 0) {
		int e = errno;
		free(buf);
		return e;
	}
	char path[256], tmp[256], orig[256];
	snprintf(path, sizeof path, FP_DIR "/%s", name);
	snprintf(tmp, sizeof tmp, FP_DIR "/.%s.tmp", name);
	snprintf(orig, sizeof orig, ORIG_DIR "/%s", name);
	if (access(path, F_OK) == 0 && access(orig, F_OK) != 0) {
		mkdir(ORIG_DIR, 0700);
		int e = copy_file(path, orig);
		LOG("first replacement of %s: original kept in orig/ (%s)", name, e ? strerror(e) : "ok");
	}
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	int err = 0;
	if (fd < 0) {
		err = errno;
	} else {
		if (write(fd, buf, n) != (ssize_t)n) {
			err = EIO;
		}
		fsync(fd);
		close(fd);
		if (err == 0 && rename(tmp, path) != 0) {
			err = errno;
		}
		if (err != 0) {
			unlink(tmp);
		}
	}
	free(buf);
	LOG("saved %s (%zu bytes): %s", name, n, err ? strerror(err) : "ok");
	return err;
}

/*
 * copies the store's dirty files back to /var/root/fp, then hands them to the store again (clean, so the
 * next change shows). Returns the slot of a template that is new on disk, or -1.
 */
static int
save_dirty(bool new_files_ok)
{
	unsigned before = template_mask();
	char *b = store_list();
	int saved = 0;
	for (char *line = b; line != NULL && *line; ) {
		char *nl = strchr(line, '\n');
		if (nl != NULL) {
			*nl = 0;
		}
		char name[128], dirty[16];
		unsigned size = 0;
		if (sscanf(line, "%127s %u %15s", name, &size, dirty) == 3 && strcmp(dirty, "dirty") == 0 && size > 0 &&
		    savable(name, new_files_ok)) {
			if (export_file(name) == 0) {
				saved++;
				load_one(FP_DIR, name);
			}
		}
		line = nl != NULL ? nl + 1 : NULL;
	}
	free(b);
	unsigned added = template_mask() & ~before;
	if (saved > 0) {
		LOG("%d store files saved to %s", saved, FP_DIR);
	}
	for (int s = 0; s < MAX_FINGERS; s++) {
		if (added & (1u << s)) {
			return s;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ names (fingers.plist) */

static CFPropertyListRef
plist_read(const char *path, CFOptionFlags opt)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	char buf[32768];
	ssize_t n = read(fd, buf, sizeof buf);
	close(fd);
	if (n <= 0) {
		return NULL;
	}
	CFDataRef data = CFDataCreate(NULL, (const UInt8 *)buf, n);
	CFPropertyListRef p = CFPropertyListCreateWithData(NULL, data, opt, NULL, NULL);
	CFRelease(data);
	if (p != NULL && CFGetTypeID(p) != CFDictionaryGetTypeID()) {
		CFRelease(p);
		p = NULL;
	}
	return p;
}

static CFMutableDictionaryRef
names_load(void)
{
	CFMutableDictionaryRef d = (CFMutableDictionaryRef)plist_read(NAMES_PATH, kCFPropertyListMutableContainersAndLeaves);
	if (d == NULL) {
		d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return d;
}

static void
names_save(CFDictionaryRef d)
{
	CFDataRef data = CFPropertyListCreateData(NULL, d, kCFPropertyListXMLFormat_v1_0, 0, NULL);
	if (data == NULL) {
		return;
	}
	int fd = open(FP_DIR "/.fingers.plist.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		bool ok = write(fd, CFDataGetBytePtr(data), (size_t)CFDataGetLength(data)) == CFDataGetLength(data);
		close(fd);
		if (ok) {
			rename(FP_DIR "/.fingers.plist.tmp", NAMES_PATH);
		}
	}
	CFRelease(data);
}

static CFStringRef
slot_key(int slot)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("%d"), slot);
}

/* name of a slot into out (UTF-8, newlines replaced); fid into *fid */
static void
names_get(CFDictionaryRef d, int slot, char *out, size_t max, unsigned *fid)
{
	snprintf(out, max, "Huella %d", slot + 1);
	*fid = 0;
	CFStringRef k = slot_key(slot);
	CFDictionaryRef e = CFDictionaryGetValue(d, k);
	CFRelease(k);
	if (e == NULL || CFGetTypeID(e) != CFDictionaryGetTypeID()) {
		return;
	}
	CFStringRef n = CFDictionaryGetValue(e, CFSTR("name"));
	if (n != NULL && CFGetTypeID(n) == CFStringGetTypeID()) {
		CFStringGetCString(n, out, (CFIndex)max, kCFStringEncodingUTF8);
		for (char *p = out; *p; p++) {
			if (*p == '\n' || *p == '\r') {
				*p = ' ';
			}
		}
	}
	CFNumberRef f = CFDictionaryGetValue(e, CFSTR("fid"));
	if (f != NULL && CFGetTypeID(f) == CFNumberGetTypeID()) {
		long long v = 0;
		CFNumberGetValue(f, kCFNumberLongLongType, &v);
		*fid = (unsigned)v;
	}
}

static CFMutableDictionaryRef
names_entry(CFMutableDictionaryRef d, int slot, bool reset)
{
	CFStringRef k = slot_key(slot);
	CFMutableDictionaryRef e = (CFMutableDictionaryRef)CFDictionaryGetValue(d, k);
	if (reset || e == NULL || CFGetTypeID(e) != CFDictionaryGetTypeID()) {
		e = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDateRef now = CFDateCreate(NULL, CFAbsoluteTimeGetCurrent());
		CFDictionarySetValue(e, CFSTR("created"), now);
		CFRelease(now);
		CFDictionarySetValue(d, k, e);
		CFRelease(e);
	}
	CFRelease(k);
	return e;
}

static void
names_set(int slot, const char *name, bool reset)
{
	CFMutableDictionaryRef d = names_load();
	CFMutableDictionaryRef e = names_entry(d, slot, reset);
	CFStringRef n = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	if (n != NULL) {
		CFDictionarySetValue(e, CFSTR("name"), n);
		CFRelease(n);
	}
	names_save(d);
	CFRelease(d);
}

static void
names_set_fid(int slot, unsigned fid)
{
	CFMutableDictionaryRef d = names_load();
	CFMutableDictionaryRef e = names_entry(d, slot, false);
	long long f = fid;
	CFNumberRef num = CFNumberCreate(NULL, kCFNumberLongLongType, &f);
	CFDictionarySetValue(e, CFSTR("fid"), num);
	CFRelease(num);
	names_save(d);
	CFRelease(d);
}

static void
names_remove(int slot)
{
	CFMutableDictionaryRef d = names_load();
	CFStringRef k = slot_key(slot);
	CFDictionaryRemoveValue(d, k);
	CFRelease(k);
	names_save(d);
	CFRelease(d);
}

/* ------------------------------------------------------------------ prefs */

static bool pref_unlock = true, pref_screen_off = false, pref_haptics = true;

/* a vibd preset (com.sapphire.vib.NAME, userland/haptics/vibd.c), if the haptics pref is on */
static void
haptic(const char *name)
{
	if (pref_haptics) {
		char n[64];
		snprintf(n, sizeof n, "com.sapphire.vib.%s", name);
		notify_post(n);
	}
}

static bool
dict_bool(CFDictionaryRef d, CFStringRef k, bool dflt)
{
	CFTypeRef v = CFDictionaryGetValue(d, k);
	if (v == NULL) {
		return dflt;
	}
	if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
		return CFBooleanGetValue(v);
	}
	if (CFGetTypeID(v) == CFNumberGetTypeID()) {
		int i = 0;
		CFNumberGetValue(v, kCFNumberIntType, &i);
		return i != 0;
	}
	return dflt;
}

static void
prefs_load(void)
{
	bool u = true, s = false, h = true;
	CFPropertyListRef p = plist_read(PREFS_PATH, kCFPropertyListImmutable);
	if (p != NULL) {
		u = dict_bool(p, CFSTR("unlockEnabled"), true);
		s = dict_bool(p, CFSTR("screenOffWake"), false);
		h = dict_bool(p, CFSTR("haptics"), true);
		CFRelease(p);
	}
	if (u != pref_unlock || s != pref_screen_off) {
		LOG("prefs: unlock with fingerprint %s, screen-off wake %s", u ? "on" : "off", s ? "on" : "off");
	}
	pref_unlock = u;
	pref_screen_off = s;
	pref_haptics = h;
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

/* ------------------------------------------------------------------ state (all on one serial queue) */

struct client {
	int fd;
	dispatch_source_t rs;
	char buf[1024];
	size_t n;
	bool gone;
};

static dispatch_queue_t q;
static dispatch_source_t poll_timer;
static int poll_ms;                     /* interval of poll_timer, 0 = stopped */
static int tok_blank = -1, tok_display = -1, tok_state = -1, tok_fails = -1;
static bool lockedout, have_ready, qsee_ok;
static uint64_t last_ready;
static int last_seq, ticks;
static int last_lock = -100, last_ubs = -1, last_screen = -1;
static int cur_lock = -1, cur_ubs;
static int fails;
static unsigned st_unlocks, st_nomatch, st_lockouts, st_offpresses;
static bool save_pending;

/* enrollment / test */
static struct client *enroll_c, *test_c;
static dispatch_source_t enroll_timer, test_timer;
static int enroll_seq, test_seq, enroll_steps, enroll_first, saved_arg;
static time_t enroll_deadline, test_deadline;
static char enroll_name[128];

static void reevaluate(const char *why);

static void
post(const char *name)
{
	notify_post(name);
	LOG("posted %s", name);
}

static void
send_line(struct client *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
send_line(struct client *c, const char *fmt, ...)
{
	if (c == NULL || c->gone) {
		return;
	}
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b - 1, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof b - 2) {
		n = (int)sizeof b - 2;
	}
	b[n++] = '\n';
	size_t off = 0;
	int spins = 0;
	while (off < (size_t)n && spins < 100) {
		ssize_t w = write(c->fd, b + off, (size_t)n - off);
		if (w < 0 && (errno == EINTR || errno == EAGAIN)) {
			spins++;
			usleep(10 * 1000);
			continue;
		}
		if (w <= 0) {
			break;
		}
		off += (size_t)w;
	}
}

static void
publish_ready(bool ready)
{
	if (have_ready && last_ready == (uint64_t)ready) {
		return;
	}
	have_ready = true;
	last_ready = ready;
	if (tok_state >= 0) {
		notify_set_state(tok_state, ready);
	}
	notify_post(N_STATE);
	LOG("fingerprint unlock %s", ready ? "available" : "unavailable");
}

static void
set_fails(int n)
{
	fails = n;
	if (tok_fails >= 0) {
		notify_set_state(tok_fails, (uint64_t)n);
	}
}

static bool
is_match(int v)
{
	/* the finger id comes back as raw 32 bits (may read negative); errors are small negatives */
	return v != 0 && !(v <= -1 && v >= -5000);
}

static bool
screen_is_on(void)
{
	uint64_t blanked = 0;
	if (tok_blank >= 0) {
		notify_get_state(tok_blank, &blanked);
	}
	return blanked == 0 || access(FP_DIR "/fpd.noscreen", F_OK) == 0;
}

static void
schedule_save(void)
{
	if (save_pending) {
		return;
	}
	save_pending = true;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), q, ^{
		save_pending = false;
		if (enroll_c == NULL) {
			save_dirty(false);
		}
	});
}

static void
authenticate(int seq, int press)
{
	int v = 0;
	post(N_PRESS);
	int err = qsee_write(OP_AUTH);
	if (err == 0) {
		err = oid_read(&o_qsee, &v);
		if (err != 0) {
			LOG("press %d: op 26 result unreadable: %s", seq, strerror(err));
			return;
		}
	} else if (err != EPERM) {
		LOG("press %d: op 26 failed: %s", seq, strerror(err));
		return;
	}
	if (err == EPERM || v == -1) {
		LOG("press %d: fingerprints locked out until the next passcode unlock", seq);
		lockedout = true;
		st_lockouts++;
		haptic("long");
		post(N_LOCKED_OUT);
		reevaluate("lockout");
		return;
	}
	if (v == GF_NOT_MATCH) {
		st_nomatch++;
		set_fails(fails + 1);
		LOG("press %d (down %d): no match (%d failed)", seq, press, fails);
		haptic("error");
		post(N_NOMATCH);
		return;
	}
	if (v == GF_LIFTED) {
		LOG("press %d: finger lifted before a match", seq);
		return;
	}
	if (!is_match(v)) {
		LOG("press %d: op 26 error %d", seq, v);
		return;
	}
	LOG("press %d: match (finger %u)", seq, (unsigned)v);
	set_fails(0);
	err = qsee_write(OP_UNLOCK);
	if (err == EAGAIN) {
		usleep(50 * 1000);
		err = qsee_write(OP_UNLOCK);
	}
	if (err != 0) {
		LOG("op 27 (keybag unlock) refused: %s", strerror(err));
		return;
	}
	st_unlocks++;
	LOG("keybag unlocked by fingerprint");
	haptic("success");
	post(N_UNLOCKED);
	schedule_save();
	reevaluate("unlocked");
}

/* screen off, locked: a press -> light the screen, then authenticate if the finger is still down */
static void
screen_off_press(int seq)
{
	st_offpresses++;
	LOG("press %d with the screen off (the touch controller reports while the panel is off)", seq);
	post(N_WAKE);
	int waited = 0;
	while (!screen_is_on() && waited < 1500) {
		usleep(50 * 1000);
		waited += 50;
	}
	if (!screen_is_on()) {
		LOG("press %d: the screen did not come on within 1.5 s", seq);
		return;
	}
	int press = fod_press();
	LOG("press %d: screen on after %d ms, finger %s", seq, waited, press ? "still down" : "lifted");
	if (press) {
		authenticate(seq, press);
	}
}

static void
poll_tick(void)
{
	int seq = 0;
	if (oid_read(&o_seq, &seq) != 0) {
		return;
	}
	if (seq != last_seq) {
		last_seq = seq;
		if (poll_ms == POLL_OFF_MS && !screen_is_on()) {
			screen_off_press(seq);
		} else {
			authenticate(seq, fod_press());
		}
		if (poll_ms == 0) {
			return;
		}
	}
	if (++ticks >= RECHECK_TICKS) {
		ticks = 0;
		reevaluate(NULL);
	}
}

static void
set_polling(int ms)
{
	if (ms == poll_ms) {
		return;
	}
	if (poll_timer != NULL) {
		dispatch_source_cancel(poll_timer);
		poll_timer = NULL;
	}
	if (ms != 0 && poll_ms == 0) {
		ticks = 0;
		oid_read(&o_seq, &last_seq);
		if (fod_press()) {
			last_seq--;             /* a finger already on the sensor counts as a new press */
		}
	}
	if (ms != poll_ms) {
		LOG("sensor watch: %s", ms == POLL_MS ? "every 30 ms" : ms == POLL_OFF_MS ? "every 60 ms (screen off)" : "off");
	}
	poll_ms = ms;
	if (ms == 0) {
		return;
	}
	poll_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(poll_timer, dispatch_time(DISPATCH_TIME_NOW, 0), (uint64_t)ms * NSEC_PER_MSEC,
	    5 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(poll_timer, ^{ poll_tick(); });
	dispatch_resume(poll_timer);
}

static void
reevaluate(const char *why)
{
	uint64_t display = 0;
	if (tok_display >= 0) {
		notify_get_state(tok_display, &display);
	}
	int screen_on = screen_is_on();
	int lock = MKBGetDeviceLockState_(NULL);
	int ubs = MKBDeviceUnlockedSinceBoot_() != 0;
	cur_lock = lock;
	cur_ubs = ubs;
	if (lock == 0 && (lockedout || fails != 0)) {
		if (lockedout) {
			LOG("unlocked: fingerprint lockout cleared");
		}
		lockedout = false;
		set_fails(0);
	}
	if (why != NULL || lock != last_lock || ubs != last_ubs || screen_on != last_screen) {
		LOG("state (%s): screen %s (display %llu), lock state %d, unlocked since boot %d%s%s%s",
		    why ? why : "recheck", screen_on ? "on" : "off", display, lock, ubs, lockedout ? ", locked out" : "",
		    enroll_c ? ", enrolling" : "", test_c ? ", testing" : "");
	}
	last_lock = lock;
	last_ubs = ubs;
	last_screen = screen_on;
	bool ready = qsee_ok && template_mask() != 0 && lock == 1 && ubs && !lockedout && pref_unlock;
	publish_ready(ready);
	bool busy = enroll_c != NULL || test_c != NULL;
	if (ready && !busy && screen_on) {
		set_polling(POLL_MS);
	} else if (ready && !busy && pref_screen_off) {
		set_polling(POLL_OFF_MS);
	} else {
		set_polling(0);
	}
}

/* ------------------------------------------------------------------ enrollment */

static void
enroll_stop(void)
{
	if (enroll_timer != NULL) {
		dispatch_source_cancel(enroll_timer);
		enroll_timer = NULL;
	}
	oid_write(&o_arg, saved_arg);
	enroll_c = NULL;
	reevaluate("enrollment over");
}

/* drop a partial enrollment: AUTH_CANCEL + sensor off (op 12), then the TA initialised again (op 10) */
static void
enroll_abort(const char *why)
{
	LOG("enrollment cancelled: %s", why);
	int e12 = qsee_write(OP_STOP);
	int e10 = qsee_write(OP_INIT);
	if (e10 != 0) {
		usleep(500 * 1000);
		e10 = qsee_write(OP_INIT);
	}
	qsee_ok = e10 == 0;
	LOG("op 12 %s, op 10 %s", e12 ? strerror(e12) : "ok", e10 ? strerror(e10) : "ok");
	enroll_stop();
}

static void
enroll_done(void)
{
	int v = 0;
	int err = qsee_op_value(OP_ENROLL_END, &v);
	LOG("op 16 (save templates): %s", err ? strerror(err) : "ok");
	struct client *c = enroll_c;
	if (err != 0) {
		send_line(c, "FAILED save %d", err);
		enroll_abort("op 16 failed");
		return;
	}
	int slot = save_dirty(true);
	if (slot < 0) {
		LOG("enrollment: no new template file appeared in the store");
		send_line(c, "FAILED nofile");
		enroll_stop();
		return;
	}
	names_set(slot, enroll_name, true);
	LOG("enrollment complete: slot %d \"%s\", %d touches", slot, enroll_name, enroll_steps);
	haptic("success");
	send_line(c, "DONE %d", slot);
	enroll_stop();
}

static void
enroll_tick(void)
{
	if (enroll_c == NULL) {
		return;
	}
	if (time(NULL) > enroll_deadline) {
		send_line(enroll_c, "FAILED timeout");
		enroll_abort("no touch for a while");
		return;
	}
	int seq = 0;
	if (oid_read(&o_seq, &seq) != 0 || seq == enroll_seq) {
		return;
	}
	enroll_seq = seq;
	enroll_deadline = time(NULL) + ENROLL_IDLE_S;
	send_line(enroll_c, "PRESS");
	int v = 0;
	int err = qsee_op_value(OP_ENROLL_TOUCH, &v);
	if (err != 0) {
		LOG("op 15: %s", strerror(err));
		send_line(enroll_c, "PROGRESS -1 %d %d", -err, enroll_first);
		return;
	}
	enroll_steps++;
	if (v >= 0) {
		if (enroll_first < 0) {
			enroll_first = v + 1;
		}
		LOG("enroll touch %d: %d samples to go", enroll_steps, v);
		haptic("light");
		send_line(enroll_c, "PROGRESS %d 0 %d", v, enroll_first);
		if (v == 0) {
			enroll_done();
		}
	} else {
		LOG("enroll touch %d: error %d", enroll_steps, -v);
		haptic("warning");
		send_line(enroll_c, "PROGRESS -1 %d %d", -v, enroll_first);
	}
}

static void
cmd_enroll_begin(struct client *c, const char *name)
{
	if (enroll_c != NULL || test_c != NULL) {
		send_line(c, "ERR busy");
		return;
	}
	if (!qsee_ok) {
		send_line(c, "ERR sensor");
		return;
	}
	int lock = MKBGetDeviceLockState_(NULL);
	if (lock != 0 && lock != 3) {
		send_line(c, "ERR locked");
		return;
	}
	int n = popcount5(template_mask());
	if (n >= MAX_FINGERS) {
		send_line(c, "ERR full");
		return;
	}
	saved_arg = 0;
	oid_read(&o_arg, &saved_arg);
	oid_write(&o_arg, ARG_SPOT);
	int err = qsee_write(OP_ENROLL_BEGIN);
	if (err != 0) {
		oid_write(&o_arg, saved_arg);
		LOG("op 14 (enroll begin): %s", strerror(err));
		send_line(c, "ERR begin %d", err);
		return;
	}
	snprintf(enroll_name, sizeof enroll_name, "%s", name != NULL ? name : "");
	if (!enroll_name[0]) {
		snprintf(enroll_name, sizeof enroll_name, "Huella %d", n + 1);
	}
	enroll_c = c;
	enroll_steps = 0;
	enroll_first = -1;
	enroll_deadline = time(NULL) + ENROLL_IDLE_S;
	oid_read(&o_seq, &enroll_seq);
	reevaluate("enrollment");      /* lock-screen watch off */
	LOG("enrollment started (\"%s\", %d fingers enrolled)", enroll_name, n);
	send_line(c, "OK %d", n);
	enroll_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(enroll_timer, dispatch_time(DISPATCH_TIME_NOW, 0), POLL_MS * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(enroll_timer, ^{ enroll_tick(); });
	dispatch_resume(enroll_timer);
}

/* ------------------------------------------------------------------ test (authenticate without unlocking) */

static void
test_stop(void)
{
	if (test_timer != NULL) {
		dispatch_source_cancel(test_timer);
		test_timer = NULL;
	}
	test_c = NULL;
	reevaluate("test over");
}

/* the slot a matched finger id belongs to (learnt when only one finger is enrolled), or -1 */
static int
slot_of_fid(unsigned fid, char *name, size_t max)
{
	CFMutableDictionaryRef d = names_load();
	unsigned m = template_mask();
	int slot = -1;
	for (int s = 0; s < MAX_FINGERS && slot < 0; s++) {
		unsigned f;
		char n[128];
		names_get(d, s, n, sizeof n, &f);
		if ((m & (1u << s)) && f == fid) {
			slot = s;
		}
	}
	CFRelease(d);
	if (slot < 0 && popcount5(m) == 1) {
		for (int s = 0; s < MAX_FINGERS; s++) {
			if (m & (1u << s)) {
				slot = s;
			}
		}
		names_set_fid(slot, fid);
		LOG("finger id %u is slot %d (the only finger)", fid, slot);
	}
	name[0] = 0;
	if (slot >= 0) {
		CFMutableDictionaryRef d2 = names_load();
		unsigned f;
		names_get(d2, slot, name, max, &f);
		CFRelease(d2);
	}
	return slot;
}

static void
test_tick(void)
{
	if (test_c == NULL) {
		return;
	}
	if (time(NULL) > test_deadline) {
		send_line(test_c, "TEST TIMEOUT");
		test_stop();
		return;
	}
	int seq = 0;
	if (oid_read(&o_seq, &seq) != 0 || seq == test_seq) {
		return;
	}
	test_seq = seq;
	send_line(test_c, "PRESS");
	int old = 0;
	oid_read(&o_arg, &old);
	oid_write(&o_arg, ARG_SPOT);
	struct timeval t0, t1;
	gettimeofday(&t0, NULL);
	int v = GF_NOT_MATCH, err = 0, tries = 0;
	for (;;) {
		err = qsee_op_value(OP_AUTH_ARG, &v);
		tries++;
		if (err != 0 || is_match(v)) {
			break;
		}
		gettimeofday(&t1, NULL);
		long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
		if (!fod_press() || ms > 3000) {
			break;
		}
	}
	oid_write(&o_arg, old);
	if (err != 0) {
		LOG("test: op 20 failed: %s", strerror(err));
		send_line(test_c, "TEST ERR %d", err);
	} else if (is_match(v)) {
		char name[128];
		int slot = slot_of_fid((unsigned)v, name, sizeof name);
		LOG("test: match, finger %u (slot %d) after %d tries", (unsigned)v, slot, tries);
		send_line(test_c, "TEST MATCH %u %d %s", (unsigned)v, slot, name);
	} else if (v == GF_NOT_MATCH) {
		LOG("test: no match after %d tries", tries);
		send_line(test_c, "%s", tries <= 1 && !fod_press() ? "TEST LIFTED" : "TEST NOMATCH");
	} else {
		LOG("test: error %d after %d tries", v, tries);
		send_line(test_c, "TEST ERR %d", v);
	}
	test_stop();
}

static void
cmd_test(struct client *c)
{
	if (enroll_c != NULL || test_c != NULL) {
		send_line(c, "ERR busy");
		return;
	}
	if (!qsee_ok || template_mask() == 0) {
		send_line(c, "%s", qsee_ok ? "ERR nofingers" : "ERR sensor");
		return;
	}
	test_c = c;
	test_deadline = time(NULL) + TEST_IDLE_S;
	oid_read(&o_seq, &test_seq);
	reevaluate("test");
	send_line(c, "OK");
	send_line(c, "PLACE");
	test_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(test_timer, dispatch_time(DISPATCH_TIME_NOW, 0), POLL_MS * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(test_timer, ^{ test_tick(); });
	dispatch_resume(test_timer);
}

/* ------------------------------------------------------------------ other commands */

static void
cmd_list(struct client *c)
{
	CFMutableDictionaryRef d = names_load();
	unsigned m = template_mask();
	for (int s = 0; s < MAX_FINGERS; s++) {
		if (!(m & (1u << s))) {
			continue;
		}
		char p[128], name[128];
		unsigned fid;
		struct stat st;
		snprintf(p, sizeof p, FP_DIR "/finger_0_%d.so", s);
		long long size = stat(p, &st) == 0 ? (long long)st.st_size : 0;
		names_get(d, s, name, sizeof name, &fid);
		send_line(c, "FINGER %d %u %lld %s", s, fid, size, name);
	}
	CFRelease(d);
	send_line(c, "END");
}

static void
delete_slot(int s)
{
	char names[4][48];
	slot_files(s, names);
	for (int i = 0; i < 4; i++) {
		char p[128];
		snprintf(p, sizeof p, FP_DIR "/%s", names[i]);
		if (unlink(p) == 0) {
			LOG("removed %s", p);
		}
		store_clear(names[i]);
	}
	names_remove(s);
}

static void
reinit_ta(void)
{
	int err = EIO;
	for (int i = 0; i < 3 && err != 0; i++) {
		err = qsee_write(OP_INIT);
		if (err != 0) {
			usleep(500 * 1000);
		}
	}
	qsee_ok = err == 0;
	LOG("TA initialised again: %s", err ? strerror(err) : "ok");
}

static void
cmd_delete(struct client *c, const char *arg, bool all)
{
	if (enroll_c != NULL || test_c != NULL) {
		send_line(c, "ERR busy");
		return;
	}
	int lock = MKBGetDeviceLockState_(NULL);
	if (lock != 0 && lock != 3) {
		send_line(c, "ERR locked");
		return;
	}
	unsigned m = template_mask();
	if (all) {
		for (int s = 0; s < MAX_FINGERS; s++) {
			if (m & (1u << s)) {
				delete_slot(s);
			}
		}
	} else {
		int s = arg != NULL ? atoi(arg) : -1;
		if (arg == NULL || s < 0 || s >= MAX_FINGERS || !(m & (1u << s))) {
			send_line(c, "ERR noslot");
			return;
		}
		delete_slot(s);
	}
	reinit_ta();
	reevaluate("deleted");
	send_line(c, "%s", qsee_ok ? "OK" : "ERR reinit");
}

static void
cmd_log(struct client *c, int n)
{
	if (n <= 0 || n > 400) {
		n = 100;
	}
	int fd = open(LOG_PATH, O_RDONLY);
	if (fd >= 0) {
		struct stat st;
		if (fstat(fd, &st) == 0) {
			off_t want = (off_t)n * 200;
			off_t start = st.st_size > want ? st.st_size - want : 0;
			size_t len = (size_t)(st.st_size - start);
			char *b = malloc(len + 1);
			if (b != NULL) {
				ssize_t r = pread(fd, b, len, start);
				b[r > 0 ? r : 0] = 0;
				char *p = b;
				if (start > 0) {                /* a partial first line */
					char *nl = strchr(p, '\n');
					p = nl != NULL ? nl + 1 : p + strlen(p);
				}
				int total = 0;
				for (char *s = p; *s; s++) {
					total += *s == '\n';
				}
				int skip = total > n ? total - n : 0;
				for (char *line = p; *line; ) {
					char *nl = strchr(line, '\n');
					if (nl != NULL) {
						*nl = 0;
					}
					if (skip > 0) {
						skip--;
					} else {
						send_line(c, "L %s", line);
					}
					if (nl == NULL) {
						break;
					}
					line = nl + 1;
				}
				free(b);
			}
		}
		close(fd);
	}
	send_line(c, "END");
}

static void
handle_line(struct client *c, char *line)
{
	size_t l = strlen(line);
	while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' ')) {
		line[--l] = 0;
	}
	char *arg = strchr(line, ' ');
	if (arg != NULL) {
		*arg++ = 0;
		while (*arg == ' ') {
			arg++;
		}
	}
	if (strcmp(line, "STATUS") == 0) {
		send_line(c, "OK enabled=%d ready=%d lock=%d ubs=%d screen=%d enrolling=%d testing=%d fingers=%d max=%d "
		    "fails=%d lockedout=%d unlocks=%u nomatch=%u lockouts=%u offpresses=%u screenoffwake=%d sensor=%d version=2",
		    pref_unlock, (int)last_ready, cur_lock, cur_ubs, screen_is_on(), enroll_c != NULL, test_c != NULL,
		    popcount5(template_mask()), MAX_FINGERS, fails, lockedout, st_unlocks, st_nomatch, st_lockouts,
		    st_offpresses, pref_screen_off, qsee_ok);
	} else if (strcmp(line, "LIST") == 0) {
		cmd_list(c);
	} else if (strcmp(line, "RENAME") == 0) {
		char *name = arg != NULL ? strchr(arg, ' ') : NULL;
		int s = arg != NULL ? atoi(arg) : -1;
		if (name == NULL || name[1] == 0 || s < 0 || s >= MAX_FINGERS || !(template_mask() & (1u << s))) {
			send_line(c, "ERR noslot");
		} else {
			names_set(s, name + 1, false);
			LOG("slot %d renamed", s);
			send_line(c, "OK");
		}
	} else if (strcmp(line, "DELETE") == 0) {
		cmd_delete(c, arg, false);
	} else if (strcmp(line, "DELETE_ALL") == 0) {
		cmd_delete(c, NULL, true);
	} else if (strcmp(line, "ENROLL_BEGIN") == 0) {
		cmd_enroll_begin(c, arg);
	} else if (strcmp(line, "ENROLL_CANCEL") == 0) {
		if (enroll_c == c) {
			enroll_abort("by the app");
		}
		send_line(c, "CANCELLED");
	} else if (strcmp(line, "TEST") == 0) {
		cmd_test(c);
	} else if (strcmp(line, "TEST_CANCEL") == 0) {
		if (test_c == c) {
			test_stop();
		}
		send_line(c, "CANCELLED");
	} else if (strcmp(line, "LOG") == 0) {
		cmd_log(c, arg != NULL ? atoi(arg) : 100);
	} else if (strcmp(line, "PREFS") == 0) {
		prefs_load();
		reevaluate("prefs");
		send_line(c, "OK");
	} else if (line[0] != 0) {
		send_line(c, "ERR unknown");
	}
}

static void
client_gone(struct client *c)
{
	if (c->gone) {
		return;
	}
	if (enroll_c == c) {
		enroll_abort("the app went away");
	}
	if (test_c == c) {
		test_stop();
	}
	c->gone = true;
	dispatch_source_cancel(c->rs);      /* the cancel handler (later, on q) closes and frees */
}

static void
client_read(struct client *c)
{
	if (c->gone) {
		return;
	}
	ssize_t r = read(c->fd, c->buf + c->n, sizeof c->buf - 1 - c->n);
	if (r <= 0) {
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
			return;
		}
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
	if (c->n >= sizeof c->buf - 1) {        /* a line too long: dropped */
		c->n = 0;
	}
}

static void
accept_client(int ls)
{
	int fd = accept(ls, NULL, NULL);
	if (fd < 0) {
		return;
	}
	uid_t uid = (uid_t)-1;
	gid_t gid;
	if (getpeereid(fd, &uid, &gid) != 0 || (uid != 0 && uid != 501)) {
		LOG("control socket: peer uid %d refused", (int)uid);
		close(fd);
		return;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	struct client *c = calloc(1, sizeof *c);
	if (c == NULL) {
		close(fd);
		return;
	}
	c->fd = fd;
	c->rs = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, q);
	dispatch_source_set_event_handler(c->rs, ^{ client_read(c); });
	dispatch_source_set_cancel_handler(c->rs, ^{
		close(c->fd);
		free(c);
	});
	dispatch_resume(c->rs);
}

static void
start_socket(void)
{
	unlink(SOCK_PATH);
	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ls < 0) {
		LOG("control socket: %s", strerror(errno));
		return;
	}
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", SOCK_PATH);
	if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 4) != 0) {
		LOG("control socket %s: %s", SOCK_PATH, strerror(errno));
		close(ls);
		return;
	}
	chmod(SOCK_PATH, 0666);
	fcntl(ls, F_SETFL, fcntl(ls, F_GETFL) | O_NONBLOCK);
	dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)ls, 0, q);
	dispatch_source_set_event_handler(s, ^{ accept_client(ls); });
	dispatch_resume(s);
	LOG("control socket %s ready", SOCK_PATH);
}

static void
watch(const char *name, int *tok, void (^extra)(void))
{
	int t = -1;
	uint32_t r = notify_register_dispatch(name, &t, q, ^(int token) {
		(void)token;
		if (extra != NULL) {
			extra();
		}
		reevaluate(name);
	});
	if (r != NOTIFY_STATUS_OK) {
		LOG("cannot watch %s (%u)", name, r);
		t = -1;
	}
	if (tok != NULL) {
		*tok = t;
	}
}

static void __attribute__((noreturn))
idle_forever(void)
{
	for (;;) {
		pause();
	}
}

int
main(void)
{
	olog = os_log_create("com.sapphire.fpd", "fpd");
	signal(SIGPIPE, SIG_IGN);
	mkdir(FP_DIR, 0700);
	LOG("fpd 2 starting (pid %d)", getpid());

	/* kept alive by launchd: with nothing to do, idle instead of exiting (no restart loop) */
	if (!oid_init(&o_qsee) || !oid_init(&o_press) || !oid_init(&o_seq)) {
		LOG("this kernel has no kern.sweet_qsee / kern.sweet_fod_*: idle");
		idle_forever();
	}
	oid_init(&o_arg);
	if (!mkb_init()) {
		LOG("MobileKeyBag not usable: idle");
		idle_forever();
	}
	prefs_load();

	load_files(store_has_templates());

	int err = EIO, v = -1;
	for (int i = 0, wait = 2; i < 5; i++, wait *= 2) {
		err = qsee_op_value(OP_INIT, &v);
		LOG("op 10 (TA init), try %d: %s, result %d", i + 1, err ? strerror(err) : "ok", v);
		if (err == 0 && v == 0) {
			break;
		}
		if (err == ENODEV) {
			LOG("QSEE is off in this boot (boot-arg qsee=1 missing): idle");
			idle_forever();
		}
		if (i < 4) {
			sleep(wait);
		}
	}
	qsee_ok = err == 0 && v == 0;
	if (!qsee_ok) {
		LOG("the TA did not initialise: no unlock, no enrollment (the app can still list fingers)");
	}

	q = dispatch_queue_create("com.sapphire.fpd", DISPATCH_QUEUE_SERIAL);
	if (notify_register_check(N_STATE, &tok_state) != NOTIFY_STATUS_OK) {
		tok_state = -1;
	}
	if (notify_register_check(N_FAILS, &tok_fails) != NOTIFY_STATUS_OK) {
		tok_fails = -1;
	}
	dispatch_sync(q, ^{
		set_fails(0);
		start_socket();
	});

	time_t age = boot_age();
	if (age < BOOT_SETTLE_S) {
		LOG("boot is %ld s old: waiting %ld s before watching notifications", (long)age, (long)(BOOT_SETTLE_S - age));
		sleep((unsigned)(BOOT_SETTLE_S - age));
	}

	dispatch_sync(q, ^{
		watch(N_BLANKED, &tok_blank, NULL);
		watch(N_DISPLAY, &tok_display, NULL);
		watch(N_KB_LOCK, NULL, NULL);
		watch(N_KB_FIRST, NULL, NULL);
		watch(N_SB_LOCK, NULL, NULL);
		watch(N_PREFS, NULL, ^{ prefs_load(); });
		reevaluate("start");
	});
	dispatch_source_t slow = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(slow, dispatch_time(DISPATCH_TIME_NOW, SLOW_RECHECK_S * NSEC_PER_SEC),
	    SLOW_RECHECK_S * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(slow, ^{
		if (poll_ms != POLL_MS) {
			reevaluate(NULL);
		}
	});
	dispatch_resume(slow);
	dispatch_main();
}
