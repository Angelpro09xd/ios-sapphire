/*
 * fpd: unlock with the in-display fingerprint sensor (kernel 0261's Goodix TA client).
 *
 * At start: every *.so in /var/root/fp (the sensor's factory calibration and the enrolled templates,
 * copies made from the TA's RAM store; Android's persist is never read or written) goes into the
 * kernel's file store (kern.sweet_qsee_file), then the TA is initialised (kern.sweet_qsee = 10,
 * retried with back-off). With no template there is nothing to match against: fpd stops there.
 *
 * Then it waits on darwin notifications (screen blanked / lock state / first unlock) and only while
 *   the screen is on, MobileKeyBag says locked (1), the device was unlocked since boot and the
 *   kernel has not locked fingerprints out (5 failures until the next passcode unlock)
 * it polls kern.sweet_fod_seq every 30 ms. A new press runs op 26 (local HBM spot on, capture and match
 * until match / lift / 3 s): on a match op 27 unlocks the kernel's keybag and
 * com.sapphire.fp.unlocked is posted (SpringBoard's SapphireFingerprint tweak then dismisses the lock
 * screen); no match posts com.sapphire.fp.nomatch, a lockout com.sapphire.fp.locked_out.
 * com.sapphire.fp.state (notify state) is 1 while fingerprint unlock is available (locked, after the
 * first unlock, templates, no lockout), else 0: the lock screen glyph follows it.
 *
 * Log: os_log (subsystem com.sapphire.fpd) and /var/root/fp/fpd.log (256 KB, then fpd.log.1).
 * Files in /var/root/fp: fpd.noscreen = ignore the screen state (if the blank notification is missing).
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
#include <time.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <notify.h>
#include <os/log.h>
#include <dispatch/dispatch.h>

#define FP_DIR          "/var/root/fp"
#define LOG_PATH        FP_DIR "/fpd.log"
#define LOG_OLD         FP_DIR "/fpd.log.1"
#define LOG_MAX         (256 * 1024)
#define FILE_MAX        (8 * 1024 * 1024)
#define POLL_MS         30
#define RECHECK_TICKS   100             /* lock state re-read every 3 s while polling */
#define SLOW_RECHECK_S  30              /* and every 30 s otherwise, in case a notification was missed */
#define BOOT_SETTLE_S   120             /* notifications reach jobs started early only later (arplay) */

#define OP_INIT         10
#define OP_AUTH         26
#define OP_UNLOCK       27
#define GF_NOT_MATCH    (-1008)
#define GF_LIFTED       (-4)

#define N_UNLOCKED      "com.sapphire.fp.unlocked"
#define N_NOMATCH       "com.sapphire.fp.nomatch"
#define N_LOCKED_OUT    "com.sapphire.fp.locked_out"
#define N_STATE         "com.sapphire.fp.state"
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

/* sysctls, by MIB */

struct oid { const char *name; int mib[CTL_MAXNAME]; size_t len; bool ok; };
static struct oid o_qsee = { "kern.sweet_qsee" }, o_press = { "kern.sweet_fod_press" }, o_seq = { "kern.sweet_fod_seq" };

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

/* writes op: 0, or the errno of the write (the op's own result for 27, EPERM for a lockout on 26) */
static int
qsee_write(int op)
{
	return sysctl(o_qsee.mib, (u_int)o_qsee.len, NULL, NULL, &op, sizeof op) == 0 ? 0 : errno;
}

/* MobileKeyBag (dlopen: nothing else of it is needed) */

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

/* files */

static bool
is_template(const char *n)
{
	return strncmp(n, "finger_", 7) == 0 && strncmp(n, "finger_extra", 12) != 0 && strstr(n, "_bak") == NULL;
}

static bool
ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && strcmp(s + a - b, suf) == 0;
}

/* the kernel store already holds templates (fpd restarted): keep its copies, they may be newer */
static bool
store_has_templates(void)
{
	size_t l = 0;
	if (sysctlbyname("kern.sweet_qsee_files", NULL, &l, NULL, 0) != 0 || l == 0) {
		return false;
	}
	char *b = calloc(1, l + 1);
	bool r = false;
	if (b != NULL && sysctlbyname("kern.sweet_qsee_files", b, &l, NULL, 0) == 0) {
		for (char *p = b; (p = strstr(p, "finger_")) != NULL; p += 7) {
			if (strncmp(p, "finger_extra", 12) != 0 && (p == b || p[-1] == '\n' || p[-1] == ' ')) {
				r = true;
				break;
			}
		}
	}
	free(b);
	return r;
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

/* returns the number of templates in the directory */
static int
load_files(bool skip)
{
	DIR *d = opendir(FP_DIR);
	if (d == NULL) {
		LOG("no %s: %s", FP_DIR, strerror(errno));
		return 0;
	}
	int templates = 0, loaded = 0, failed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !ends_with(e->d_name, ".so")) {
			continue;
		}
		if (is_template(e->d_name)) {
			templates++;
		}
		if (skip) {
			continue;
		}
		if (load_one(FP_DIR, e->d_name) == 0) {
			loaded++;
		} else {
			failed++;
		}
	}
	closedir(d);
	if (skip) {
		LOG("the kernel's store already has templates: files not loaded again");
	} else {
		LOG("%d files loaded, %d failed, %d templates", loaded, failed, templates);
	}
	return templates;
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

/* the state machine, on one serial queue */

static dispatch_queue_t q;
static dispatch_source_t poll_timer;
static int tok_blank = -1, tok_display = -1, tok_state = -1;
static int templates;
static bool polling, lockedout, have_ready;
static uint64_t last_ready;
static int last_seq, ticks;
static int last_lock = -100, last_ubs = -1, last_screen = -1;

static void reevaluate(const char *why);

static void
post(const char *name)
{
	notify_post(name);
	LOG("posted %s", name);
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

static bool
is_match(int v)
{
	/* the finger id comes back as raw 32 bits (may read negative); errors are small negatives */
	return v != 0 && !(v <= -1 && v >= -5000);
}

static void
authenticate(int seq, int press)
{
	int v = 0;
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
		post(N_LOCKED_OUT);
		reevaluate("lockout");
		return;
	}
	if (v == GF_NOT_MATCH) {
		LOG("press %d (down %d): no match", seq, press);
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
	err = qsee_write(OP_UNLOCK);
	if (err == EAGAIN) {
		usleep(50 * 1000);
		err = qsee_write(OP_UNLOCK);
	}
	if (err != 0) {
		LOG("op 27 (keybag unlock) refused: %s", strerror(err));
		return;
	}
	LOG("keybag unlocked by fingerprint");
	post(N_UNLOCKED);
	reevaluate("unlocked");
}

static void
poll_tick(void)
{
	int seq = 0, press = 0;
	if (oid_read(&o_seq, &seq) != 0) {
		return;
	}
	if (seq != last_seq) {
		last_seq = seq;
		oid_read(&o_press, &press);
		authenticate(seq, press);
		if (!polling) {
			return;
		}
	}
	if (++ticks >= RECHECK_TICKS) {
		ticks = 0;
		reevaluate(NULL);
	}
}

static void
start_polling(void)
{
	if (polling) {
		return;
	}
	polling = true;
	ticks = 0;
	int press = 0;
	oid_read(&o_seq, &last_seq);
	oid_read(&o_press, &press);
	if (press) {
		last_seq--;             /* a finger already on the sensor counts as a new press */
	}
	poll_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(poll_timer, dispatch_time(DISPATCH_TIME_NOW, 0), POLL_MS * NSEC_PER_MSEC,
	    5 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(poll_timer, ^{ poll_tick(); });
	dispatch_resume(poll_timer);
}

static void
stop_polling(void)
{
	if (!polling) {
		return;
	}
	polling = false;
	dispatch_source_cancel(poll_timer);
	poll_timer = NULL;
}

static void
reevaluate(const char *why)
{
	uint64_t blanked = 0, display = 0;
	if (tok_blank >= 0) {
		notify_get_state(tok_blank, &blanked);
	}
	if (tok_display >= 0) {
		notify_get_state(tok_display, &display);
	}
	int screen_on = blanked == 0 || access(FP_DIR "/fpd.noscreen", F_OK) == 0;
	int lock = MKBGetDeviceLockState_(NULL);
	int ubs = MKBDeviceUnlockedSinceBoot_() != 0;
	if (lock == 0 && lockedout) {
		lockedout = false;
		LOG("unlocked: fingerprint lockout cleared");
	}
	if (why != NULL || lock != last_lock || ubs != last_ubs || screen_on != last_screen) {
		LOG("state (%s): screen %s (blanked %llu, display %llu), lock state %d, unlocked since boot %d%s",
		    why ? why : "recheck", screen_on ? "on" : "off", blanked, display, lock, ubs,
		    lockedout ? ", locked out" : "");
	}
	last_lock = lock;
	last_ubs = ubs;
	last_screen = screen_on;
	bool ready = templates > 0 && lock == 1 && ubs && !lockedout;
	publish_ready(ready);
	if (ready && screen_on) {
		start_polling();
	} else {
		stop_polling();
	}
}

static void
watch(const char *name, int *tok)
{
	int t = -1;
	uint32_t r = notify_register_dispatch(name, &t, q, ^(int token) {
		(void)token;
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

int
main(void)
{
	olog = os_log_create("com.sapphire.fpd", "fpd");
	LOG("fpd starting (pid %d)", getpid());

	if (!oid_init(&o_qsee) || !oid_init(&o_press) || !oid_init(&o_seq)) {
		LOG("this kernel has no kern.sweet_qsee / kern.sweet_fod_*: nothing to do");
		return 0;
	}
	if (!mkb_init()) {
		LOG("MobileKeyBag not usable: nothing to do");
		return 0;
	}

	bool skip = store_has_templates();
	templates = load_files(skip);

	int err = EIO, v = -1;
	for (int i = 0, wait = 2; i < 5; i++, wait *= 2) {
		err = qsee_write(OP_INIT);
		v = -1;
		if (err == 0) {
			oid_read(&o_qsee, &v);
		}
		LOG("op 10 (TA init), try %d: %s, result %d", i + 1, err ? strerror(err) : "ok", v);
		if (err == 0 && v == 0) {
			break;
		}
		if (err == ENODEV) {
			LOG("QSEE is off in this boot (boot-arg qsee=1 missing)");
			return 0;
		}
		if (i < 4) {
			sleep(wait);
		}
	}
	if (err != 0 || v != 0) {
		LOG("the TA did not initialise: giving up (no retry loop: exit 0)");
		return 0;
	}
	if (templates == 0) {
		LOG("no enrolled finger in %s: TA initialised, nothing else to do", FP_DIR);
		return 0;
	}

	time_t age = boot_age();
	if (age < BOOT_SETTLE_S) {
		LOG("boot is %ld s old: waiting %ld s before watching notifications", (long)age, (long)(BOOT_SETTLE_S - age));
		sleep((unsigned)(BOOT_SETTLE_S - age));
	}

	q = dispatch_queue_create("com.sapphire.fpd", DISPATCH_QUEUE_SERIAL);
	if (notify_register_check(N_STATE, &tok_state) != NOTIFY_STATUS_OK) {
		tok_state = -1;
	}
	dispatch_sync(q, ^{
		watch(N_BLANKED, &tok_blank);
		watch(N_DISPLAY, &tok_display);
		watch(N_KB_LOCK, NULL);
		watch(N_KB_FIRST, NULL);
		watch(N_SB_LOCK, NULL);
		reevaluate("start");
	});
	dispatch_source_t slow = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(slow, dispatch_time(DISPATCH_TIME_NOW, SLOW_RECHECK_S * NSEC_PER_SEC),
	    SLOW_RECHECK_S * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(slow, ^{
		if (!polling) {
			reevaluate(NULL);
		}
	});
	dispatch_resume(slow);
	dispatch_main();
}
