/*
 * vibd: the vibration motor (kernel 0263's PM7250B LDO vibrator, an ERM: no LRA, so haptics are short pulses)
 * for every process, through darwin notifications -- nobody else touches the sysctls.
 *
 *   com.sapphire.vib.<preset>   tap 20, light 15, medium 30, heavy 45, selection 12, keyboard 14, soft 18,
 *                               rigid 25, long 400, success 30,60,30, warning 40,80,40, error 50,60,50,60,50,
 *                               notify 150,120,150, ring 800,400,800 (once)
 *   com.sapphire.vib.ms         on for the notify state's ms (cap 2000; 0 stops)
 *   com.sapphire.vib.pat        a pattern packed in the notify state: byte i = step i in 10 ms units
 *                               (on, off, on, ...; up to 8 steps, the first 0 byte ends it)
 *   com.sapphire.vib.stop       stops (and ends a ring)
 *   com.sapphire.vib.ring_start the call ring: 800,400,800 every 2.4 s until ring_stop / stop (or 60 s)
 *   com.sapphire.vib.ring_stop
 * kern.sweet_vibrate (ms) / kern.sweet_vibrate_pattern ("on,off,..."): a new request replaces the running one.
 * Requests closer than 8 ms to the previous one are dropped (a haptic posted twice by two paths).
 * Log: /var/tmp/vibd.log (64 KB, then .1), only starts, errors and rings.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <dispatch/dispatch.h>

#define LOG_PATH "/var/tmp/vibd.log"
#define RING_PATTERN "800,400,800"
#define RING_PERIOD_MS 2400
#define RING_MAX_S 60

static void
logf_(const char *fmt, ...)
{
	char msg[400], line[480];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	struct stat st;
	if (stat(LOG_PATH, &st) == 0 && st.st_size > 64 * 1024) {
		rename(LOG_PATH, LOG_PATH ".1");
	}
	int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) {
		return;
	}
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	int n = snprintf(line, sizeof line, "%02d-%02d %02d:%02d:%02d %s\n", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
	    tm.tm_sec, msg);
	if (n > 0) {
		write(fd, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
	}
	close(fd);
}
#define LOG(...) logf_(__VA_ARGS__)

static int mib_ms[CTL_MAXNAME], mib_pat[CTL_MAXNAME];
static size_t len_ms, len_pat;
static uint64_t last_abs;
static mach_timebase_info_data_t tb;
static dispatch_queue_t q;
static dispatch_source_t ring_timer;
static time_t ring_since;
static unsigned errors;

static uint64_t
ms_since_last(void)
{
	uint64_t now = mach_absolute_time();
	uint64_t d = (now - last_abs) * tb.numer / tb.denom / 1000000;
	return d;
}

static bool
debounce(void)
{
	if (last_abs != 0 && ms_since_last() < 8) {
		return false;
	}
	last_abs = mach_absolute_time();
	return true;
}

static void
note_error(const char *what, int e)
{
	if (errors++ < 20) {
		LOG("%s: %s", what, strerror(e));
	}
}

static void
vib_ms(int ms)
{
	if (ms < 0) {
		ms = 0;
	}
	if (ms > 2000) {
		ms = 2000;
	}
	if (sysctl(mib_ms, (u_int)len_ms, NULL, NULL, &ms, sizeof ms) != 0) {
		note_error("kern.sweet_vibrate", errno);
	}
}

static void
vib_pattern(const char *p)
{
	if (strchr(p, ',') == NULL) {
		vib_ms(atoi(p));
		return;
	}
	if (sysctl(mib_pat, (u_int)len_pat, NULL, NULL, (void *)p, strlen(p) + 1) != 0) {
		note_error("kern.sweet_vibrate_pattern", errno);
	}
}

static void
ring_stop(const char *why)
{
	if (ring_timer != NULL) {
		dispatch_source_cancel(ring_timer);
		ring_timer = NULL;
		vib_ms(0);
		LOG("ring stopped (%s)", why);
	}
}

static void
ring_start(void)
{
	ring_since = time(NULL);
	if (ring_timer != NULL) {
		return;
	}
	LOG("ring started");
	ring_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
	dispatch_source_set_timer(ring_timer, dispatch_time(DISPATCH_TIME_NOW, 0), RING_PERIOD_MS * NSEC_PER_MSEC,
	    50 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(ring_timer, ^{
		if (time(NULL) - ring_since > RING_MAX_S) {
			ring_stop("60 s");
			return;
		}
		vib_pattern(RING_PATTERN);
		last_abs = mach_absolute_time();
	});
	dispatch_resume(ring_timer);
}

static const struct { const char *name, *pattern; } presets[] = {
	{ "tap", "20" }, { "light", "15" }, { "medium", "30" }, { "heavy", "45" }, { "selection", "12" },
	{ "keyboard", "14" }, { "soft", "18" }, { "rigid", "25" }, { "long", "400" },
	{ "success", "30,60,30" }, { "warning", "40,80,40" }, { "error", "50,60,50,60,50" },
	{ "notify", "150,120,150" }, { "ring", RING_PATTERN },
};

static void
on(const char *name, void (^h)(int token))
{
	int t;
	if (notify_register_dispatch(name, &t, q, h) != NOTIFY_STATUS_OK) {
		LOG("cannot watch %s", name);
	}
}

int
main(void)
{
	mach_timebase_info(&tb);
	len_ms = len_pat = CTL_MAXNAME;
	if (sysctlnametomib("kern.sweet_vibrate", mib_ms, &len_ms) != 0 ||
	    sysctlnametomib("kern.sweet_vibrate_pattern", mib_pat, &len_pat) != 0) {
		LOG("no kern.sweet_vibrate in this kernel: idle");
		for (;;) {
			pause();
		}
	}
	/* notifications reach jobs started early in the boot only later (arplay, fpd): wait until it is 2 min old */
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
	q = dispatch_queue_create("com.sapphire.vibd", DISPATCH_QUEUE_SERIAL);
	dispatch_sync(q, ^{
		for (size_t i = 0; i < sizeof presets / sizeof presets[0]; i++) {
			char n[64];
			snprintf(n, sizeof n, "com.sapphire.vib.%s", presets[i].name);
			const char *pat = presets[i].pattern;
			on(n, ^(int t) {
				(void)t;
				if (ring_timer == NULL && debounce()) {
					vib_pattern(pat);
				}
			});
		}
		on("com.sapphire.vib.ms", ^(int t) {
			uint64_t v = 0;
			notify_get_state(t, &v);
			if (ring_timer == NULL && (v == 0 || debounce())) {
				vib_ms((int)(v > 2000 ? 2000 : v));
			}
		});
		on("com.sapphire.vib.pat", ^(int t) {
			uint64_t v = 0;
			notify_get_state(t, &v);
			char p[64] = "";
			size_t n = 0;
			for (int i = 0; i < 8; i++) {
				unsigned step = (unsigned)((v >> (8 * i)) & 0xff);
				if (step == 0) {
					break;
				}
				n += (size_t)snprintf(p + n, sizeof p - n, "%s%u", i ? "," : "", step * 10);
			}
			if (n > 0 && ring_timer == NULL && debounce()) {
				vib_pattern(p);
			}
		});
		on("com.sapphire.vib.stop", ^(int t) {
			(void)t;
			ring_stop("stop");
			vib_ms(0);
		});
		on("com.sapphire.vib.ring_start", ^(int t) {
			(void)t;
			ring_start();
		});
		on("com.sapphire.vib.ring_stop", ^(int t) {
			(void)t;
			ring_stop("ring_stop");
		});
	});
	LOG("vibd started (pid %d)", getpid());
	dispatch_main();
}
