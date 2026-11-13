/*
 * SapphireDisplayLink: UIKit's in-process animations at the panel's rate (120 Hz).
 *
 * The display link of UIViewInProcessAnimationManager, which runs the animations done in the app's own
 * process (a scroll view's page settling on the home screen, among them), asks CADisplayLink for 20..30
 * frames a second; on an iPhone with ProMotion that saves power, here the panel refreshes at 120 whatever is
 * drawn, and a page swipe ended in most of a second of frames 33.3 ms apart. The display links made for that
 * target are marked, and the rates asked of them are replaced by 60..120, 120 preferred: the panel's rate
 * (with xnu 0302's sweetfb_hz=240 CoreAnimation is told 240, and these animations at 240 would only cost
 * SpringBoard twice the work for frames the panel cannot show). Every other
 * display link keeps what it asks (the clock's 10 and 60, the motion effects' 32...): at 120 they would only
 * wake the process more often.
 *
 * Only Objective-C method replacement (no function hooks). /var/jb/var/sapphire-dl-off turns it off (at the
 * next launch of each process). After HackiOS userland/src/inject_sb.c display_link_rates() (7caa457, MIT).
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <unistd.h>
#include <stdio.h>
#include <dlfcn.h>
#include <mach/mach_time.h>

/* /var/tmp/dl-log present at launch: every display link made and every rate asked go to /var/tmp/dl.<process>.log */
static FILE *dl_log;
static unsigned dl_logged;

static void
say_caller(const char *what, id link, const char *detail, void *ret)
{
	if (dl_log == NULL || dl_logged++ > 2000) {
		return;
	}
	Dl_info info;
	const char *img = "?", *sym = "?";
	if (dladdr(ret, &info)) {
		img = info.dli_fname && strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : "?";
		sym = info.dli_sname ? info.dli_sname : "?";
	}
	fprintf(dl_log, "%llu %s %p %s by %s`%s\n", (unsigned long long)mach_absolute_time(), what, (__bridge void *)link, detail, img, sym);
	fflush(dl_log);
}

#define TAG "SapphireDisplayLink: "
#define FULL_FOR "UIViewInProcessAnimationManager"

typedef struct {
	float minimum, maximum, preferred;
} frame_rate_range;

static id (*o_with)(id, SEL, id, SEL);
static void (*o_fps)(id, SEL, long);
static void (*o_range)(id, SEL, frame_rate_range);
static char full_key;                   /* the mark a display link made for FULL_FOR carries */

static bool
full_rate(id link)
{
	return objc_getAssociatedObject(link, &full_key) != nil;
}

static id
my_with(id self, SEL sel, id target, SEL action)
{
	id link = o_with(self, sel, target, action);
	if (link != nil && target != nil && strcmp(object_getClassName(target), FULL_FOR) == 0) {
		objc_setAssociatedObject(link, &full_key, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
	}
	if (dl_log != NULL) {
		char d[256];
		snprintf(d, sizeof(d), "for -[%s %s]", target ? object_getClassName(target) : "nil", action ? sel_getName(action) : "");
		say_caller("made", link, d, __builtin_return_address(0));
	}
	return link;
}

/* the rate given to the marked links: /var/jb/var/sapphire-dl-fps (a number) or 120; read once a process */
static long full_fps = 120;

static void
my_fps(id self, SEL sel, long fps)
{
	if (dl_log != NULL) {
		char d[64];
		snprintf(d, sizeof(d), "fps %ld%s", fps, full_rate(self) ? " (-> 120)" : "");
		say_caller("rate", self, d, __builtin_return_address(0));
	}
	o_fps(self, sel, full_rate(self) ? full_fps : fps);
}

static void
my_range(id self, SEL sel, frame_rate_range range)
{
	if (dl_log != NULL) {
		char d[96];
		snprintf(d, sizeof(d), "range %.0f..%.0f pref %.0f%s", range.minimum, range.maximum, range.preferred,
		    full_rate(self) ? " (-> 60..120, 120)" : "");
		say_caller("rate", self, d, __builtin_return_address(0));
	}
	if (full_rate(self)) {
		range = (frame_rate_range){ (float)(full_fps < 60 ? full_fps : 60), (float)full_fps, (float)full_fps };
	}
	o_range(self, sel, range);
}

__attribute__((constructor)) static void
init(void)
{
	if (access("/var/jb/var/sapphire-dl-off", F_OK) == 0) {
		return;
	}
	FILE *f = fopen("/var/jb/var/sapphire-dl-fps", "r");
	if (f != NULL) {
		long v = 0;
		if (fscanf(f, "%ld", &v) == 1 && v >= 20 && v <= 240) {
			full_fps = v;
		}
		fclose(f);
	}
	if (access("/var/tmp/dl-log", F_OK) == 0) {
		char path[256];
		snprintf(path, sizeof(path), "/var/tmp/dl.%s.log", getprogname());
		dl_log = fopen(path, "a");
	}
	Class link = objc_getClass("CADisplayLink");
	if (link == Nil) {
		return;
	}
	Method made = class_getClassMethod(link, sel_registerName("displayLinkWithTarget:selector:"));
	Method fps = class_getInstanceMethod(link, sel_registerName("setPreferredFramesPerSecond:"));
	Method range = class_getInstanceMethod(link, sel_registerName("setPreferredFrameRateRange:"));
	if (made == NULL || fps == NULL || range == NULL) {
		NSLog(@TAG "CADisplayLink methods missing (made %p fps %p range %p): nothing changed", made, fps, range);
		return;
	}
	o_with = (id (*)(id, SEL, id, SEL))method_setImplementation(made, (IMP)my_with);
	o_fps = (void (*)(id, SEL, long))method_setImplementation(fps, (IMP)my_fps);
	o_range = (void (*)(id, SEL, frame_rate_range))method_setImplementation(range, (IMP)my_range);
	(void)TAG;
}
