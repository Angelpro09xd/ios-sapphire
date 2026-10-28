/*
 * SapphireButtonRegenerator: the ring/silent switch, on the volume buttons.
 *
 * sapphire has no ring/silent switch. Here Volume Up and Volume Down pressed together (the second within
 * 250 ms of the first, both held) do what flipping an iPhone's switch does, through SpringBoard's own
 * switch path: -[SpringBoard _updateRingerState:withVisuals:updatePreferenceRegister:] (what
 * -[SpringBoard _ringerChanged:] calls for the switch's HID event; state 1 ringer on, 0 silent) stores
 * the switch state, offers it to SBHardwareButtonService's ringer consumers, then
 * -[SBRingerControl setRingerMuted:] (com.apple.springboard.ringerstate, SBRingerChangedNotification,
 * the sound defaults: the status bar bell, calls and notifications) and
 * -[SBRingerControl activateRingerHUDFromMuteSwitch:] (the "Silent Mode On/Off" banner and its haptic).
 * If SBUIHasHIDRingerSwitch() says there is no switch, that method does nothing, so the two
 * SBRingerControl calls are made here instead (-toggleRingerMute also returns at once when there is one).
 *
 * The chord: SBVolumeHardwareButton hands each press to SBVolumeHardwareButtonActions
 * (-volume{Increase,Decrease}PressDownWithModifiers: on down, -volume{Increase,Decrease}PressUp on up),
 * which steps the volume on down. A down that comes with the other button idle is held back:
 *   the other button goes down within 250 ms  chord: both presses are dropped (no volume step, no HUD),
 *                                             the ringer toggles once, and neither button does anything
 *                                             more until both are up (holding does not repeat);
 *   the button comes up first (a tap)         the held-back down is replayed, then the up;
 *   250 ms pass with the button still held    the down is replayed (holding then repeats as usual).
 * So a tap changes the volume at release instead of at press, a hold 250 ms later, and a chord never
 * touches it -- nothing to undo. -cancelVolumePress (screenshot, SOS) drops a held-back down.
 *
 * Screen off: a chord with the display asleep (-[SBBacklightController screenIsOn] NO) first lights it
 * the way tap-to-wake does, -[SBLockScreenManager _wakeScreenForTapToWake] (-unlockUIFromSource:
 * TapToWake withOptions: TurnOnScreenFirst + StartFadeIn: the lock screen, still locked), so the
 * ringer HUD is seen; if the screen is still off 0.4 s later, -[SBBacklightController
 * turnOnScreenFullyWithBacklightSource:] is tried. 3 s after the chord it is put back to sleep as the
 * idle timer does, -[SBBacklightController _startFadeOutAnimationFromLockSource:] (IdleTimer, 13;
 * SBScreenSleepCoordinator -sleepAndLockUIFromSource:completion:), unless meanwhile the screen was
 * touched (-[SpringBoard sendEvent:], touch events), a volume button was pressed again, the UI was
 * unlocked or the screen went off by itself.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>

#define WINDOW_NS (250 * NSEC_PER_MSEC)

enum { VUP, VDOWN };
static const char *names[2] = { "vol+", "vol-" };

static IMP orig_down[2], orig_up[2], orig_cancel;
static id actions;                      /* the SBVolumeHardwareButtonActions (lives as long as SpringBoard) */
static int pending = -1;                /* button whose down is held back */
static long long pending_mods;
static unsigned generation;             /* invalidates the 250 ms timer */
static BOOL swallowed[2];               /* part of a chord: ignored until its up */
static BOOL cancelling;

static SEL down_sel[2], up_sel[2];

static void
call_down(int b, long long mods)
{
	((void (*)(id, SEL, long long))orig_down[b])(actions, down_sel[b], mods);
}

static void
call_up(int b)
{
	((void (*)(id, SEL))orig_up[b])(actions, up_sel[b]);
}

static id
msg(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	return (o != nil && [o respondsToSelector:s]) ? ((id (*)(id, SEL))objc_msgSend)(o, s) : nil;
}

static BOOL
is_muted(id rc)
{
	return ((BOOL (*)(id, SEL))objc_msgSend)(rc, sel_registerName("isRingerMuted"));
}

static void
toggle_ringer(void)
{
	id sb = msg((id)objc_getClass("UIApplication"), "sharedApplication");
	id rc = msg(sb, "ringerControl");
	if (rc == nil || ![rc respondsToSelector:sel_registerName("isRingerMuted")]) {
		NSLog(@"SapphireButtonRegenerator: no ringer control");
		return;
	}
	BOOL want = !is_muted(rc);
	BOOL (*has_switch)(void) = (BOOL (*)(void))dlsym(RTLD_DEFAULT, "SBUIHasHIDRingerSwitch");
	SEL upd = sel_registerName("_updateRingerState:withVisuals:updatePreferenceRegister:");
	const char *how;
	if (has_switch != NULL && has_switch() && [sb respondsToSelector:upd]) {
		/* as -[SpringBoard _ringerChanged:] does for the switch: visuals, not the side switch preference */
		((void (*)(id, SEL, int, BOOL, BOOL))objc_msgSend)(sb, upd, want ? 0 : 1, YES, NO);
		how = "switch path";
	} else {
		how = "ringer control";
		SEL set = sel_registerName("setRingerMuted:"), hud = sel_registerName("activateRingerHUDFromMuteSwitch:");
		if ([rc respondsToSelector:set]) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(rc, set, want);
		}
		if ([rc respondsToSelector:hud]) {
			((void (*)(id, SEL, int))objc_msgSend)(rc, hud, want ? 0 : 1);
		}
	}
	BOOL now = is_muted(rc);
	NSLog(@"SapphireButtonRegenerator: chord -> silent mode %s (ringer muted=%d, via %s%s)", now ? "ON" : "OFF", now, how,
	    now == want ? "" : ", NOT applied: a ringer consumer took it");
}

/* screen-off chord: wake, then sleep again unless the user did something */

#define LOCK_SOURCE_IDLE_TIMER 13
#define BACKLIGHT_SOURCE_UNKNOWN 0

static BOOL sleep_armed;                /* screen was lit by a chord, not yet put back */
static BOOL user_acted;                 /* touch / new button press since then */
static unsigned wake_generation;
static IMP orig_send_event;

static id
shared(const char *cls)
{
	return msg((id)objc_getClass(cls), "sharedInstance");
}

static BOOL
get_bool(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	return (o != nil && [o respondsToSelector:s]) ? ((BOOL (*)(id, SEL))objc_msgSend)(o, s) : NO;
}

static long long
backlight_state(id bc)
{
	SEL s = sel_registerName("backlightState");
	return (bc != nil && [bc respondsToSelector:s]) ? ((long long (*)(id, SEL))objc_msgSend)(bc, s) : -1;
}

static void
sleep_back(unsigned g)
{
	if (g != wake_generation || !sleep_armed) {
		return;
	}
	sleep_armed = NO;
	id bc = shared("SBBacklightController"), lsm = shared("SBLockScreenManager");
	BOOL on = get_bool(bc, "screenIsOn"), locked = get_bool(lsm, "isUILocked");
	if (user_acted || !on || !locked) {
		NSLog(@"SapphireButtonRegenerator: screen left as is (user acted=%d, screen on=%d, locked=%d)", user_acted, on, locked);
		return;
	}
	SEL fade = sel_registerName("_startFadeOutAnimationFromLockSource:");
	if ([bc respondsToSelector:fade]) {
		((void (*)(id, SEL, int))objc_msgSend)(bc, fade, LOCK_SOURCE_IDLE_TIMER);
		NSLog(@"SapphireButtonRegenerator: screen back to sleep (fade out, IdleTimer source)");
	} else {
		NSLog(@"SapphireButtonRegenerator: no -_startFadeOutAnimationFromLockSource:, screen left on");
	}
}

static void
check_lit(unsigned g)
{
	if (g != wake_generation) {
		return;
	}
	id bc = shared("SBBacklightController");
	if (get_bool(bc, "screenIsOn")) {
		NSLog(@"SapphireButtonRegenerator: screen on (backlight state %lld)", backlight_state(bc));
		return;
	}
	SEL full = sel_registerName("turnOnScreenFullyWithBacklightSource:");
	if ([bc respondsToSelector:full]) {
		NSLog(@"SapphireButtonRegenerator: still off after tap-to-wake (state %lld), trying turnOnScreenFullyWithBacklightSource:",
		    backlight_state(bc));
		((void (*)(id, SEL, long long))objc_msgSend)(bc, full, BACKLIGHT_SOURCE_UNKNOWN);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
		id b = shared("SBBacklightController");
		NSLog(@"SapphireButtonRegenerator: after fallback screen on=%d (state %lld)", get_bool(b, "screenIsOn"), backlight_state(b));
	});
}

/* before the ringer toggles: light an asleep screen, arm the sleep-back */
static void
wake_if_off(void)
{
	id bc = shared("SBBacklightController"), lsm = shared("SBLockScreenManager");
	if (bc == nil || lsm == nil) {
		return;
	}
	BOOL on = get_bool(bc, "screenIsOn");
	NSLog(@"SapphireButtonRegenerator: screen before chord: %s (backlight state %lld, locked=%d)", on ? "on" : "off",
	    backlight_state(bc), get_bool(lsm, "isUILocked"));
	if (on) {
		return;                         /* lit already (perhaps by an earlier chord): leave the timer as is */
	}
	SEL tap = sel_registerName("_wakeScreenForTapToWake");
	if (![lsm respondsToSelector:tap]) {
		NSLog(@"SapphireButtonRegenerator: no -_wakeScreenForTapToWake");
		return;
	}
	((void (*)(id, SEL))objc_msgSend)(lsm, tap);
	NSLog(@"SapphireButtonRegenerator: woke screen (tap-to-wake path)");
	unsigned g = ++wake_generation;
	sleep_armed = YES;
	user_acted = NO;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ check_lit(g); });
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ sleep_back(g); });
}

static void
my_send_event(id self, SEL sel, id event)
{
	if (sleep_armed && !user_acted && event != nil &&
	    ((long long (*)(id, SEL))objc_msgSend)(event, sel_registerName("type")) == 0 /* UIEventTypeTouches */) {
		user_acted = YES;
		NSLog(@"SapphireButtonRegenerator: touch while lit by chord, screen stays on");
	}
	((void (*)(id, SEL, id))orig_send_event)(self, sel, event);
}

static void
press_down(id self, int b, long long mods)
{
	int other = !b;
	actions = self;
	if (sleep_armed && !swallowed[b] && !user_acted) {
		user_acted = YES;               /* a new press, not the chord's own */
		NSLog(@"SapphireButtonRegenerator: %s pressed while lit by chord, screen stays on", names[b]);
	}
	if (swallowed[b]) {
		return;
	}
	if (swallowed[other]) {                 /* pressed again while the chord's other button is still held */
		swallowed[b] = YES;
		NSLog(@"SapphireButtonRegenerator: %s down during chord, ignored", names[b]);
		return;
	}
	if (pending == other) {
		generation++;
		pending = -1;
		swallowed[VUP] = swallowed[VDOWN] = YES;
		NSLog(@"SapphireButtonRegenerator: %s+%s chord", names[other], names[b]);
		wake_if_off();
		toggle_ringer();
		return;
	}
	if (pending == b) {
		return;
	}
	if (pending >= 0) {                     /* (cannot happen: the other case is above) */
		call_down(b, mods);
		return;
	}
	pending = b;
	pending_mods = mods;
	unsigned g = ++generation;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, WINDOW_NS), dispatch_get_main_queue(), ^{
		if (generation == g && pending == b) {
			pending = -1;
			call_down(b, pending_mods);     /* held alone: volume as usual (repeats from here) */
		}
	});
}

static void
press_up(int b)
{
	if (cancelling || actions == nil) {
		call_up(b);
		return;
	}
	if (swallowed[b]) {
		swallowed[b] = NO;
		return;
	}
	if (pending == b) {                     /* a tap: its volume step now */
		pending = -1;
		generation++;
		call_down(b, pending_mods);
	}
	call_up(b);
}

static void my_inc_down(id self, SEL sel, long long mods) { (void)sel; press_down(self, VUP, mods); }
static void my_dec_down(id self, SEL sel, long long mods) { (void)sel; press_down(self, VDOWN, mods); }
static void my_inc_up(id self, SEL sel) { (void)sel; actions = self; press_up(VUP); }
static void my_dec_up(id self, SEL sel) { (void)sel; actions = self; press_up(VDOWN); }

static void
my_cancel(id self, SEL sel)
{
	actions = self;
	pending = -1;
	generation++;
	cancelling = YES;
	((void (*)(id, SEL))orig_cancel)(self, sel);
	cancelling = NO;
}

__attribute__((constructor)) static void
install(void)
{
	Class c = objc_getClass("SBVolumeHardwareButtonActions");
	if (c == Nil) {
		return;
	}
	const char *sels[5] = { "volumeIncreasePressDownWithModifiers:", "volumeDecreasePressDownWithModifiers:",
		"volumeIncreasePressUp", "volumeDecreasePressUp", "cancelVolumePress" };
	Method m[5];
	for (int i = 0; i < 5; i++) {
		m[i] = class_getInstanceMethod(c, sel_registerName(sels[i]));
		if (m[i] == NULL) {
			NSLog(@"SapphireButtonRegenerator: no -[SBVolumeHardwareButtonActions %s], not installed", sels[i]);
			return;
		}
	}
	down_sel[VUP] = sel_registerName(sels[0]);
	down_sel[VDOWN] = sel_registerName(sels[1]);
	up_sel[VUP] = sel_registerName(sels[2]);
	up_sel[VDOWN] = sel_registerName(sels[3]);
	orig_down[VUP] = method_setImplementation(m[0], (IMP)my_inc_down);
	orig_down[VDOWN] = method_setImplementation(m[1], (IMP)my_dec_down);
	orig_up[VUP] = method_setImplementation(m[2], (IMP)my_inc_up);
	orig_up[VDOWN] = method_setImplementation(m[3], (IMP)my_dec_up);
	orig_cancel = method_setImplementation(m[4], (IMP)my_cancel);
	Class sb = objc_getClass("SpringBoard");
	SEL se = sel_registerName("sendEvent:");
	Method sm = sb != Nil ? class_getInstanceMethod(sb, se) : NULL;
	if (sm != NULL) {                       /* on SpringBoard itself, not on UIApplication */
		IMP inherited = method_getImplementation(sm);
		if (class_addMethod(sb, se, (IMP)my_send_event, method_getTypeEncoding(sm))) {
			orig_send_event = inherited;
		} else {
			orig_send_event = method_setImplementation(sm, (IMP)my_send_event);
		}
	} else {
		NSLog(@"SapphireButtonRegenerator: no -[SpringBoard sendEvent:], touches not watched");
	}
	NSLog(@"SapphireButtonRegenerator: volume buttons hooked (vol+ & vol- together toggles silent mode)");
}
