/*
 * SapphireHaptics: iOS haptics and alert vibrations on sapphire's vibration motor (an ERM, driven by vibd).
 * Loaded into every UIKit app and SpringBoard; it only posts darwin notifications (com.sapphire.vib.*,
 * vibd.c) -- no sysctl, nothing blocking.
 *
 * UIKit haptics. Every feedback generator ends in -[UIFeedbackGenerator _playFeedback:] (impact, selection,
 * notification, switches, pickers, context menus, drag...), which UIKit only reaches when System Haptics is
 * on: that hook vibrates, with the kind the public call in progress gave (thread-local) or one guessed from
 * the generator's class. The public calls (-impactOccurred, -impactOccurredWithIntensity:,
 * -selectionChanged, -notificationOccurred:) vibrate themselves only if UIKit did not reach _playFeedback:.
 * Keyboard: -[_UIKBFeedbackGenerator actionOccurred:(atLocation:)], when the keyboard's haptic feedback is
 * on (Settings > Sounds & Haptics > Keyboard Feedback, com.apple.keyboard.preferences, or the Huella app).
 * Mapping (ERM pulses): light 15, medium 30, heavy 45, soft 18, rigid 25 ms, selection 12, keyboard 14,
 * success / warning / error patterns.
 *
 * AudioServicesPlaySystemSound(WithCompletion) / AudioServicesPlayAlertSound(WithCompletion) (ElleKit's
 * MSHookFunction): 4095 kSystemSoundID_Vibrate and 1011 long, 1519 peek light, 1520 pop medium, 1521 nope
 * error, 1350 medium, 1351/1352 long; the sound is still played as before. Off with the file
 * /var/jb/var/sapphire-haptics-noaudiohook.
 *
 * Alerts: -[TLAlert play / playWithCompletionHandler:] (ToneLibrary: notifications, messages, alarms...)
 * vibrate with the alert's own vibration pattern (TLVibrationManager, packed into 8 steps) when
 * -[TLVibrationManager shouldVibrateForCurrentRingerSwitchState] says so (Settings' vibrate on ring /
 * silent); an incoming-call alert (type 1) starts vibd's repeating ring, -stop/-stopWithOptions: ends it.
 * SpringBoard also follows TUCallCenterCallStatusChangedNotification: ring while
 * -[TUCallCenter incomingCall] exists, stop when it is answered or declined.
 *
 * Prefs (the Huella app, /var/mobile/Library/Preferences/com.sapphire.fingerprint.plist, re-read on
 * com.sapphire.fp.prefs): uiHaptics (YES), keyboardHaptics (NO: follows iOS), alertVibration (YES).
 * Accessibility > Touch > Vibration off (_AXSVibrationDisabled) silences everything.
 */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <notify.h>
#include <mach/mach_time.h>

#define TAG "SapphireHaptics: "
#define PREFS_PATH @"/var/mobile/Library/Preferences/com.sapphire.fingerprint.plist"

static BOOL p_ui = YES, p_kb_ours, p_kb_ios, p_alerts = YES;
static int (*AXSVibrationDisabled)(void);
static int tok_ms = -1, tok_pat = -1;
static uint64_t last_post;
static mach_timebase_info_data_t tb;

static __thread const char *pending;   /* the kind the public call in progress asked for */
static __thread int fired, depth;

/* ------------------------------------------------------------------ posting */

static BOOL
vib_allowed(void)
{
	return AXSVibrationDisabled == NULL || !AXSVibrationDisabled();
}

/* drops a second haptic within 25 ms (one tap reaching two paths) */
static BOOL
debounce(void)
{
	uint64_t now = mach_absolute_time();
	uint64_t ms = (now - last_post) * tb.numer / tb.denom / 1000000;
	if (last_post != 0 && ms < 25) {
		return NO;
	}
	last_post = now;
	return YES;
}

static void
vib(const char *preset)
{
	if (preset == NULL || !vib_allowed() || !debounce()) {
		return;
	}
	char n[64];
	snprintf(n, sizeof n, "com.sapphire.vib.%s", preset);
	notify_post(n);
}

static void
vib_packed(uint64_t packed)
{
	if (packed == 0 || !vib_allowed() || !debounce()) {
		return;
	}
	if (tok_pat < 0 && notify_register_check("com.sapphire.vib.pat", &tok_pat) != NOTIFY_STATUS_OK) {
		tok_pat = -1;
		return;
	}
	notify_set_state(tok_pat, packed);
	notify_post("com.sapphire.vib.pat");
}

static void
vib_raw(const char *name)
{
	notify_post(name);
}

/* ------------------------------------------------------------------ hooking */

static BOOL
hook(Class c, const char *sel, IMP imp, IMP *orig)
{
	SEL s = sel_registerName(sel);
	Method m = c != Nil ? class_getInstanceMethod(c, s) : NULL;
	if (m == NULL) {
		return NO;
	}
	IMP inherited = method_getImplementation(m);
	if (class_addMethod(c, s, imp, method_getTypeEncoding(m))) {
		*orig = inherited;              /* was inherited: the override stays on this class */
	} else {
		*orig = method_setImplementation(m, imp);
	}
	return YES;
}

/* ------------------------------------------------------------------ UIKit generators */

static const char *
impact_preset(id gen)
{
	Ivar iv = class_getInstanceVariable([gen class], "_style");
	long long style = 1;
	if (iv != NULL) {
		const char *t = ivar_getTypeEncoding(iv);
		if (t != NULL && (t[0] == 'q' || t[0] == 'l' || t[0] == 'i')) {
			char *base = (char *)(__bridge void *)gen;
			style = t[0] == 'q' ? *(long long *)(base + ivar_getOffset(iv)) : *(int *)(base + ivar_getOffset(iv));
		}
	}
	switch (style) {
	case 0: return "light";
	case 2: return "heavy";
	case 3: return "soft";
	case 4: return "rigid";
	default: return "medium";
	}
}

/* a kind for generators reached without a public call (UIKit's own controls) */
static const char *
guess_preset(id gen)
{
	NSString *n = NSStringFromClass([gen class]);
	if ([gen isKindOfClass:objc_getClass("UIImpactFeedbackGenerator")]) {
		return impact_preset(gen);
	}
	if ([n rangeOfString:@"Selection"].location != NSNotFound || [n rangeOfString:@"Retarget"].location != NSNotFound ||
	    [n rangeOfString:@"Drag"].location != NSNotFound) {
		return "selection";
	}
	if ([n rangeOfString:@"Notification"].location != NSNotFound) {
		return "success";
	}
	if ([n rangeOfString:@"Edge"].location != NSNotFound) {
		return "light";
	}
	if ([n rangeOfString:@"Keyboard"].location != NSNotFound) {
		return (p_kb_ours || p_kb_ios) ? "keyboard" : NULL;
	}
	if ([n rangeOfString:@"Impact"].location != NSNotFound) {
		return "medium";
	}
	return "tap";
}

static IMP o_play;
static void
my_play(id self, SEL sel, id feedback)
{
	if (depth == 0 && p_ui) {
		fired = 1;
		vib(pending != NULL ? pending : guess_preset(self));
	}
	depth++;
	((void (*)(id, SEL, id))o_play)(self, sel, feedback);
	depth--;
}

/* runs a public call: its own kind for _playFeedback:, and the vibration itself if UIKit did not get there */
static void
public_call(const char *preset, void (^call)(void))
{
	const char *saved = pending;
	int saved_fired = fired;
	pending = preset;
	fired = 0;
	call();
	if (!fired && depth == 0 && p_ui) {
		vib(preset);
	}
	pending = saved;
	fired = saved_fired;
}

static IMP o_impact, o_impact_i, o_selection, o_notification;

static void
my_impact(id self, SEL sel)
{
	public_call(impact_preset(self), ^{ ((void (*)(id, SEL))o_impact)(self, sel); });
}

static void
my_impact_i(id self, SEL sel, CGFloat intensity)
{
	const char *p = impact_preset(self);
	if (intensity < 0.4) {
		p = "light";
	}
	public_call(p, ^{ ((void (*)(id, SEL, CGFloat))o_impact_i)(self, sel, intensity); });
}

static void
my_selection(id self, SEL sel)
{
	public_call("selection", ^{ ((void (*)(id, SEL))o_selection)(self, sel); });
}

static void
my_notification(id self, SEL sel, NSInteger type)
{
	const char *p = type == 0 ? "success" : type == 1 ? "warning" : "error";
	public_call(p, ^{ ((void (*)(id, SEL, NSInteger))o_notification)(self, sel, type); });
}

/* keyboard */
static IMP o_kb, o_kb_loc;

static void
kb_vib(void)
{
	if (depth == 0 && (p_kb_ours || p_kb_ios)) {
		vib("keyboard");
	}
}

static void
my_kb(id self, SEL sel, long long action)
{
	kb_vib();
	depth++;
	((void (*)(id, SEL, long long))o_kb)(self, sel, action);
	depth--;
}

static void
my_kb_loc(id self, SEL sel, long long action, CGPoint loc)
{
	kb_vib();
	depth++;
	((void (*)(id, SEL, long long, CGPoint))o_kb_loc)(self, sel, action, loc);
	depth--;
}

/* ------------------------------------------------------------------ AudioServices */

typedef uint32_t SystemSoundID_;
static void (*o_ass)(SystemSoundID_), (*o_asa)(SystemSoundID_);
static void (*o_assc)(SystemSoundID_, id), (*o_asac)(SystemSoundID_, id);

static const char *
sound_preset(SystemSoundID_ s)
{
	switch (s) {
	case 4095: case 1011: case 1351: case 1352: return "long";
	case 1519: return "light";
	case 1520: case 1350: return "medium";
	case 1521: return "error";
	default: return NULL;
	}
}

static void
sound_vib(SystemSoundID_ s)
{
	const char *p = sound_preset(s);
	if (p != NULL && depth == 0) {
		vib(p);
	}
}

static void my_ass(SystemSoundID_ s) { sound_vib(s); o_ass(s); }
static void my_asa(SystemSoundID_ s) { sound_vib(s); o_asa(s); }
static void my_assc(SystemSoundID_ s, id c) { sound_vib(s); o_assc(s, c); }
static void my_asac(SystemSoundID_ s, id c) { sound_vib(s); o_asac(s, c); }

static void
hook_audio_services(void)
{
	if (access("/var/jb/var/sapphire-haptics-noaudiohook", F_OK) == 0) {
		return;
	}
	typedef void (*MSHookFunction_t)(void *, void *, void **);
	MSHookFunction_t mshook = (MSHookFunction_t)dlsym(RTLD_DEFAULT, "MSHookFunction");
	const char *libs[] = { "/var/jb/usr/lib/libellekit.dylib", "/var/jb/usr/lib/libsubstrate.dylib" };
	for (int i = 0; mshook == NULL && i < 2; i++) {
		void *h = dlopen(libs[i], RTLD_LAZY | RTLD_NOLOAD);
		if (h == NULL) {
			h = dlopen(libs[i], RTLD_LAZY);
		}
		if (h != NULL) {
			mshook = (MSHookFunction_t)dlsym(h, "MSHookFunction");
		}
	}
	if (mshook == NULL) {
		NSLog(@TAG "no MSHookFunction: AudioServices not hooked");
		return;
	}
	struct { const char *name; void *rep; void **orig; } f[] = {
		{ "AudioServicesPlaySystemSound", (void *)my_ass, (void **)&o_ass },
		{ "AudioServicesPlayAlertSound", (void *)my_asa, (void **)&o_asa },
		{ "AudioServicesPlaySystemSoundWithCompletion", (void *)my_assc, (void **)&o_assc },
		{ "AudioServicesPlayAlertSoundWithCompletion", (void *)my_asac, (void **)&o_asac },
	};
	for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
		void *sym = dlsym(RTLD_DEFAULT, f[i].name);
		if (sym != NULL) {
			mshook(sym, f[i].rep, f[i].orig);
			if (*f[i].orig == NULL) {
				NSLog(@TAG "%s: hook failed", f[i].name);
			}
		}
	}
}

/* ------------------------------------------------------------------ alerts (ToneLibrary) and calls */

static BOOL ringing;

static BOOL
alerts_should_vibrate(void)
{
	if (!p_alerts) {
		return NO;
	}
	id vm = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("TLVibrationManager"), sel_registerName("sharedVibrationManager"));
	SEL s = sel_registerName("shouldVibrateForCurrentRingerSwitchState");
	if (vm != nil && [vm respondsToSelector:s]) {
		return ((BOOL (*)(id, SEL))objc_msgSend)(vm, s);
	}
	return YES;
}

static void
ring(BOOL on, const char *why)
{
	if (on == ringing) {
		return;
	}
	ringing = on;
	NSLog(@TAG "call ring vibration %s (%s)", on ? "on" : "off", why);
	vib_raw(on ? "com.sapphire.vib.ring_start" : "com.sapphire.vib.ring_stop");
}

/* the alert's vibration pattern (VibePattern: on?, ms, on?, ms, ...) packed in 8 steps of 10 ms units */
static uint64_t
alert_pattern(id alert, long long type)
{
	id vm = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("TLVibrationManager"), sel_registerName("sharedVibrationManager"));
	if (vm == nil) {
		return 0;
	}
	id ident = [alert respondsToSelector:@selector(vibrationIdentifier)] ? [alert valueForKey:@"vibrationIdentifier"] : nil;
	NSDictionary *pat = nil;
	SEL byId = sel_registerName("patternForVibrationWithIdentifier:"), byType = sel_registerName("currentVibrationPatternForAlertType:");
	if ([ident isKindOfClass:[NSString class]] && [vm respondsToSelector:byId]) {
		pat = ((id (*)(id, SEL, id))objc_msgSend)(vm, byId, ident);
	} else if ([vm respondsToSelector:byType]) {
		pat = ((id (*)(id, SEL, long long))objc_msgSend)(vm, byType, type);
	}
	NSArray *v = [pat isKindOfClass:[NSDictionary class]] ? pat[@"VibePattern"] : nil;
	if (![v isKindOfClass:[NSArray class]] || v.count < 2) {
		return 0;
	}
	unsigned steps[8], n = 0;
	BOOL want_on = YES;
	for (NSUInteger i = 0; i + 1 < v.count && n < 8; i += 2) {
		BOOL is_on = [v[i] boolValue];
		double ms = [v[i + 1] doubleValue];
		if (ms <= 0) {
			continue;
		}
		if (n == 0 && !is_on) {
			continue;                       /* a leading pause */
		}
		if (n > 0 && is_on != want_on) {        /* the same kind twice: one longer step */
			steps[n - 1] = MIN(255u, steps[n - 1] + (unsigned)(ms / 10));
			continue;
		}
		steps[n++] = MAX(1u, MIN(255u, (unsigned)(ms / 10)));
		want_on = !is_on;
	}
	uint64_t packed = 0;
	for (unsigned i = 0; i < n; i++) {
		packed |= (uint64_t)steps[i] << (8 * i);
	}
	return packed;
}

static void
alert_started(id alert)
{
	long long type = [alert respondsToSelector:@selector(type)] ? ((long long (*)(id, SEL))objc_msgSend)(alert, @selector(type)) : 0;
	if (!alerts_should_vibrate()) {
		return;
	}
	id ident = [alert respondsToSelector:@selector(vibrationIdentifier)] ? [alert valueForKey:@"vibrationIdentifier"] : nil;
	if ([ident isKindOfClass:[NSString class]] && [(NSString *)ident rangeOfString:@"none" options:NSCaseInsensitiveSearch].location != NSNotFound) {
		return;
	}
	if (type == 1) {
		objc_setAssociatedObject(alert, &ringing, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
		ring(YES, "incoming call alert");
		return;
	}
	uint64_t p = alert_pattern(alert, type);
	if (p != 0) {
		vib_packed(p);
	} else {
		vib("notify");
	}
}

static void
alert_stopped(id alert)
{
	if (objc_getAssociatedObject(alert, &ringing) != nil) {
		ring(NO, "incoming call alert stopped");
	}
}

static IMP o_tl_play, o_tl_playc, o_tl_stop, o_tl_stopo;
static void my_tl_play(id self, SEL sel) { alert_started(self); ((void (*)(id, SEL))o_tl_play)(self, sel); }
static void my_tl_playc(id self, SEL sel, id c) { alert_started(self); ((void (*)(id, SEL, id))o_tl_playc)(self, sel, c); }
static void my_tl_stop(id self, SEL sel) { alert_stopped(self); ((void (*)(id, SEL))o_tl_stop)(self, sel); }
static void my_tl_stopo(id self, SEL sel, id o) { alert_stopped(self); ((void (*)(id, SEL, id))o_tl_stopo)(self, sel, o); }

static void
call_status_changed(void)
{
	id cc = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("TUCallCenter"), sel_registerName("sharedInstance"));
	SEL s = sel_registerName("incomingCall");
	id incoming = (cc != nil && [cc respondsToSelector:s]) ? ((id (*)(id, SEL))objc_msgSend)(cc, s) : nil;
	if (incoming != nil && !ringing && alerts_should_vibrate()) {
		ring(YES, "incoming call");
	} else if (incoming == nil && ringing) {
		ring(NO, "call answered or ended");
	}
}

/* ------------------------------------------------------------------ prefs */

static void
load_prefs(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:PREFS_PATH];
		BOOL ui = YES, kb = NO, al = YES;
		if ([d isKindOfClass:[NSDictionary class]]) {
			id v;
			if ([(v = d[@"uiHaptics"]) respondsToSelector:@selector(boolValue)]) ui = [v boolValue];
			if ([(v = d[@"keyboardHaptics"]) respondsToSelector:@selector(boolValue)]) kb = [v boolValue];
			if ([(v = d[@"alertVibration"]) respondsToSelector:@selector(boolValue)]) al = [v boolValue];
		}
		BOOL ios = NO;
		CFPropertyListRef k = CFPreferencesCopyAppValue(CFSTR("KeyboardHaptics"), CFSTR("com.apple.keyboard.preferences"));
		if (k != NULL) {
			ios = CFGetTypeID(k) == CFBooleanGetTypeID() && CFBooleanGetValue(k);
			CFRelease(k);
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			p_ui = ui;
			p_kb_ours = kb;
			p_kb_ios = ios;
			p_alerts = al;
		});
	});
}

__attribute__((constructor)) static void
haptics_init(void)
{
	mach_timebase_info(&tb);
	AXSVibrationDisabled = (int (*)(void))dlsym(RTLD_DEFAULT, "_AXSVibrationDisabled");
	notify_register_check("com.sapphire.vib.ms", &tok_ms);
	load_prefs();
	int t;
	notify_register_dispatch("com.sapphire.fp.prefs", &t, dispatch_get_main_queue(), ^(int x) {
		(void)x;
		load_prefs();
	});

	int n = 0;
	n += hook(objc_getClass("UIFeedbackGenerator"), "_playFeedback:", (IMP)my_play, &o_play);
	n += hook(objc_getClass("UIImpactFeedbackGenerator"), "impactOccurred", (IMP)my_impact, &o_impact);
	n += hook(objc_getClass("UIImpactFeedbackGenerator"), "impactOccurredWithIntensity:", (IMP)my_impact_i, &o_impact_i);
	n += hook(objc_getClass("UISelectionFeedbackGenerator"), "selectionChanged", (IMP)my_selection, &o_selection);
	n += hook(objc_getClass("UINotificationFeedbackGenerator"), "notificationOccurred:", (IMP)my_notification, &o_notification);
	n += hook(objc_getClass("_UIKBFeedbackGenerator"), "actionOccurred:", (IMP)my_kb, &o_kb);
	n += hook(objc_getClass("_UIKBFeedbackGenerator"), "actionOccurred:atLocation:", (IMP)my_kb_loc, &o_kb_loc);
	Class tl = objc_getClass("TLAlert");
	if (tl != Nil) {
		n += hook(tl, "play", (IMP)my_tl_play, &o_tl_play);
		n += hook(tl, "playWithCompletionHandler:", (IMP)my_tl_playc, &o_tl_playc);
		n += hook(tl, "stop", (IMP)my_tl_stop, &o_tl_stop);
		n += hook(tl, "stopWithOptions:", (IMP)my_tl_stopo, &o_tl_stopo);
	}
	hook_audio_services();
	if (objc_getClass("SpringBoard") != Nil) {
		[[NSNotificationCenter defaultCenter] addObserverForName:@"TUCallCenterCallStatusChangedNotification" object:nil
		    queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *note) {
			(void)note;
			call_status_changed();
		}];
	}
	NSLog(@TAG "loaded in %@ (%d methods hooked%s)", [[NSProcessInfo processInfo] processName], n, tl != Nil ? ", ToneLibrary" : "");
}
