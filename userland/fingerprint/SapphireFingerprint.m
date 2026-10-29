/*
 * SapphireFingerprint (v2): the lock screen side of fingerprint unlock (fpd does the sensor and the keybag).
 *
 * Glyph: a fingerprint mark over the sensor -- panel pixel (540, 2109) of 1080 x 2400, i.e. x = 1/2 and
 * y = 2109/2400 of the screen whatever the scale (iOS draws 375 x 812 points here) -- while fingerprint
 * unlock is available (com.sapphire.fp.state = 1 from fpd: passcode-locked, unlocked once since boot,
 * a finger enrolled, no lockout), unlocking is on in the prefs and the UI is locked:
 *   - on the cover sheet: one UIView added to CSCoverSheetViewController's view;
 *   - with the passcode keypad up (-[SBLockScreenManager _isPasscodeVisible]): a second one added to the
 *     keypad's own view (the first SBUIPasscodeLockView* found in the cover sheet / SpringBoard's windows;
 *     not over a full keyboard), in the free space under the "0" key. The sensor works there too.
 * Nothing is hooked. While a glyph may show, a 0.3 s timer follows the keypad; it stops when the screen
 * blanks or the UI unlocks.
 * Look: prefs in /var/mobile/Library/Preferences/com.sapphire.fingerprint.plist (written by the Huella app,
 * re-read on com.sapphire.fp.prefs): glyphStyle touchid|symbol|ring|dot, glyphColor white|blue|red|green|
 * purple|orange|pink|teal, glyphSize (points, 0 = the sensor's size), glyphOpacity, pressAnimation
 * pulse|ripple|none, showOnKeypad, fadeIn, successAnimation, unlockEnabled, screenOffWake.
 *
 * com.sapphire.fp.unlocked (fpd: the kernel's keybag is already unlocked): the lock screen goes away the
 * way Touch ID's does: -[SBLockScreenManager _attemptUnlockWithPasscode:nil mesa:YES finishUIUnlock:YES]
 * (what -biometricAuthenticationCoordinator:requestsUnlockWithIntent: calls): a biometric
 * SBFAuthenticationRequest that SBFUserAuthenticationController grants when -isAuthenticated (the keybag's
 * lock state, read afresh once -_invalidateCachedPasscodeLockState drops its per-runloop cache) says
 * unlocked; the handler then runs -_finishUIUnlockFromSource:22 (no keypad). Retried once 0.6 s later.
 * com.sapphire.fp.press: pulse / ripple. com.sapphire.fp.nomatch: shake and "No reconocida (n/5)".
 * com.sapphire.fp.locked_out: glyphs hidden, "Introduce el código". com.sapphire.fp.wake (a press with the
 * screen off, fpd's screenOffWake): the screen lit the tap-to-wake way, -[SBLockScreenManager
 * _wakeScreenForTapToWake].
 *
 * Everything on the main queue from notify_register_dispatch (the prefs file is read on a background
 * queue); no sysctl, no IPC of its own beyond notifyd, no haptics or HUD.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <notify.h>

#define TAG "SapphireFingerprint: "
#define PREFS_PATH      @"/var/mobile/Library/Preferences/com.sapphire.fingerprint.plist"
#define SENSOR_X        (540.0 / 1080.0)
#define SENSOR_Y        (2109.0 / 2400.0)
#define SENSOR_R        (105.0 / 1080.0)    /* radius, fraction of the width */
#define MAX_FAILS       5

/* prefs */
static BOOL p_unlock = YES, p_keypad = YES, p_fade = YES, p_success = YES, p_screen_off;
static NSString *p_style = @"touchid", *p_color = @"white", *p_anim = @"pulse";
static CGFloat p_size, p_opacity = 0.9;

static UIView *g_ls, *g_kp;             /* glyph on the cover sheet / on the keypad */
static __weak UIView *kp_host;
static CFAbsoluteTime kp_searched;
static UILabel *g_msg;
static BOOL ready, locked_out, blanked;
static int tok_state = -1, tok_blank = -1, tok_fails = -1;
static NSTimer *follow;

static id
msg(id o, const char *sel)
{
	SEL s = sel_registerName(sel);
	return (o != nil && [o respondsToSelector:s]) ? ((id (*)(id, SEL))objc_msgSend)(o, s) : nil;
}

static BOOL
msg_bool(id o, const char *sel, BOOL dflt)
{
	SEL s = sel_registerName(sel);
	return (o != nil && [o respondsToSelector:s]) ? ((BOOL (*)(id, SEL))objc_msgSend)(o, s) : dflt;
}

static id
lock_manager(void)
{
	Class c = objc_getClass("SBLockScreenManager");
	id m = msg((id)c, "sharedInstanceIfExists");
	return m != nil ? m : msg((id)c, "sharedInstance");
}

static UIView *
cover_host(void)
{
	id vc = msg(lock_manager(), "coverSheetViewController");
	if (![vc isKindOfClass:[UIViewController class]] || ![(UIViewController *)vc isViewLoaded]) {
		return nil;
	}
	return [(UIViewController *)vc view];
}

/* ------------------------------------------------------------------ the keypad's view */

static UIView *
find_passcode_view(UIView *v, int depth, int *budget)
{
	if (v == nil || depth > 28 || (*budget)-- <= 0 || v.hidden) {
		return nil;
	}
	NSString *n = NSStringFromClass([v class]);
	if ([n hasPrefix:@"SBUIPasscodeLockView"]) {
		return v;
	}
	for (UIView *s in v.subviews) {
		UIView *r = find_passcode_view(s, depth + 1, budget);
		if (r != nil) {
			return r;
		}
	}
	return nil;
}

static UIView *
keypad_host(void)
{
	UIView *h = kp_host;
	if (h != nil && h.window != nil && !h.hidden) {
		return h;
	}
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (now - kp_searched < 1.0) {          /* one walk a second at most */
		return nil;
	}
	kp_searched = now;
	int budget = 4000;
	h = find_passcode_view(cover_host(), 0, &budget);
	if (h == nil) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		NSArray *wins = [UIApplication sharedApplication].windows;
#pragma clang diagnostic pop
		for (UIWindow *w in [wins reverseObjectEnumerator]) {
			if (w.hidden || w.alpha < 0.01) {
				continue;
			}
			h = find_passcode_view(w, 0, &budget);
			if (h != nil) {
				break;
			}
		}
	}
	if (h != nil) {
		NSString *n = NSStringFromClass([h class]);
		if ([n rangeOfString:@"Keyboard"].location != NSNotFound || [n rangeOfString:@"Alphanumeric"].location != NSNotFound) {
			NSLog(@TAG "keypad view %@ is a keyboard: no glyph on it", n);
			h = nil;
		} else {
			NSLog(@TAG "keypad view: %@", n);
		}
	}
	kp_host = h;
	return h;
}

/* ------------------------------------------------------------------ the glyph */

static UIColor *
glyph_color(void)
{
	NSDictionary *m = @{
		@"white": [UIColor whiteColor], @"blue": [UIColor systemBlueColor], @"red": [UIColor systemRedColor],
		@"green": [UIColor systemGreenColor], @"purple": [UIColor systemPurpleColor],
		@"orange": [UIColor systemOrangeColor], @"pink": [UIColor systemPinkColor], @"teal": [UIColor systemTealColor],
	};
	UIColor *c = m[p_color];
	return c != nil ? c : [UIColor whiteColor];
}

static CGFloat
glyph_diameter(void)
{
	CGFloat w = [UIScreen mainScreen].bounds.size.width;
	CGFloat sensor = 2.0 * SENSOR_R * (w > 0 ? w : 375);
	if (p_size >= 30 && p_size <= 140) {
		return p_size;
	}
	return MAX(70.0, sensor);
}

static UIView *
make_glyph(void)
{
	CGFloat d = glyph_diameter();
	UIColor *c = glyph_color();
	UIView *v = [[UIView alloc] initWithFrame:CGRectMake(0, 0, d, d)];
	v.userInteractionEnabled = NO;
	v.hidden = YES;
	v.layer.cornerRadius = d / 2.0;
	BOOL symbol = YES;
	if ([p_style isEqualToString:@"ring"]) {
		v.layer.borderColor = c.CGColor;
		v.layer.borderWidth = 2.0;
		symbol = NO;
	} else if ([p_style isEqualToString:@"dot"]) {
		UIView *dot = [[UIView alloc] initWithFrame:CGRectInset(v.bounds, d * 0.3, d * 0.3)];
		dot.backgroundColor = c;
		dot.layer.cornerRadius = dot.bounds.size.width / 2.0;
		dot.autoresizingMask = UIViewAutoresizingFlexibleLeftMargin | UIViewAutoresizingFlexibleRightMargin |
		    UIViewAutoresizingFlexibleTopMargin | UIViewAutoresizingFlexibleBottomMargin;
		[v addSubview:dot];
		symbol = NO;
	} else if (![p_style isEqualToString:@"symbol"]) {      /* touchid: the symbol in a faint circle */
		v.backgroundColor = [c colorWithAlphaComponent:0.10];
		v.layer.borderColor = [c colorWithAlphaComponent:0.35].CGColor;
		v.layer.borderWidth = 1.0;
	}
	if (symbol) {
		UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:d * 0.6
		    weight:UIImageSymbolWeightLight];
		UIImage *img = [UIImage systemImageNamed:@"touchid" withConfiguration:cfg];
		if (img != nil) {
			UIImageView *iv = [[UIImageView alloc] initWithImage:img];
			iv.tintColor = c;
			iv.contentMode = UIViewContentModeCenter;
			iv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
			iv.frame = v.bounds;
			iv.tag = 1;
			[v addSubview:iv];
		} else {
			v.layer.borderColor = c.CGColor;
			v.layer.borderWidth = 2.0;
		}
	}
	return v;
}

/* the sensor's centre in the host's coordinates (from the screen's: any scale or offset is taken in) */
static CGPoint
sensor_in(UIView *host)
{
	UIScreen *scr = [UIScreen mainScreen];
	CGRect sb = scr.bounds;
	CGPoint c = CGPointMake(sb.origin.x + SENSOR_X * sb.size.width, sb.origin.y + SENSOR_Y * sb.size.height);
	if (host.window != nil) {
		c = [host convertPoint:c fromCoordinateSpace:scr.coordinateSpace];
	}
	return c;
}

static void
show_glyph(UIView * __strong *gp, UIView *host, BOOL show)
{
	UIView *g = *gp;
	if (!show || host == nil) {
		if (g != nil) {
			g.hidden = YES;
		}
		return;
	}
	if (g == nil) {
		g = *gp = make_glyph();
	}
	if (g.superview != host) {
		[g removeFromSuperview];
		[host addSubview:g];
		g.hidden = YES;
		NSLog(@TAG "glyph added to %@", NSStringFromClass([host class]));
	}
	g.center = sensor_in(host);
	if (host.subviews.lastObject != g) {
		[host bringSubviewToFront:g];
	}
	if (g.hidden) {
		g.hidden = NO;
		[g.layer removeAllAnimations];
		g.transform = CGAffineTransformIdentity;
		if (p_fade) {
			g.alpha = 0;
			[UIView animateWithDuration:0.25 animations:^{ g.alpha = p_opacity; }];
		} else {
			g.alpha = p_opacity;
		}
	}
}

static UIView *
visible_glyph(void)
{
	if (g_kp != nil && !g_kp.hidden && g_kp.superview != nil) {
		return g_kp;
	}
	if (g_ls != nil && !g_ls.hidden && g_ls.superview != nil) {
		return g_ls;
	}
	return nil;
}

static void update(void);

static void
set_follow(BOOL on)
{
	if (on && follow == nil) {
		follow = [NSTimer scheduledTimerWithTimeInterval:0.3 repeats:YES block:^(NSTimer *t) {
			(void)t;
			update();
		}];
		follow.tolerance = 0.1;
	} else if (!on && follow != nil) {
		[follow invalidate];
		follow = nil;
	}
}

static void
update(void)
{
	id m = lock_manager();
	BOOL ui_locked = msg_bool(m, "isUILocked", NO);
	BOOL keypad = msg_bool(m, "_isPasscodeVisible", NO);
	BOOL candidate = p_unlock && ready && ui_locked && !blanked;
	set_follow(candidate);
	BOOL ok = candidate && !locked_out;
	show_glyph(&g_ls, cover_host(), ok && !keypad);
	UIView *kh = ok && keypad && p_keypad ? keypad_host() : nil;
	if (kh != nil && !CGRectContainsPoint(kh.bounds, sensor_in(kh)) && kh.window != nil) {
		kh = kh.window;                 /* the keypad view does not reach the sensor: its window does */
	}
	show_glyph(&g_kp, kh, kh != nil);
	if (!keypad) {
		kp_searched = 0;
	}
	if (!ui_locked && g_msg != nil) {
		g_msg.hidden = YES;
	}
}

/* ------------------------------------------------------------------ feedback */

static void
message(NSString *text, NSTimeInterval secs)
{
	UIView *g = visible_glyph();
	UIView *host = g != nil ? g.superview : cover_host();
	if (host == nil) {
		return;
	}
	if (g_msg == nil) {
		g_msg = [[UILabel alloc] initWithFrame:CGRectZero];
		g_msg.userInteractionEnabled = NO;
		g_msg.textColor = [UIColor whiteColor];
		g_msg.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
		g_msg.textAlignment = NSTextAlignmentCenter;
		g_msg.layer.shadowColor = [UIColor blackColor].CGColor;
		g_msg.layer.shadowOpacity = 0.6;
		g_msg.layer.shadowRadius = 3;
		g_msg.layer.shadowOffset = CGSizeZero;
	}
	if (g_msg.superview != host) {
		[g_msg removeFromSuperview];
		[host addSubview:g_msg];
	}
	g_msg.text = text;
	[g_msg sizeToFit];
	CGPoint c = sensor_in(host);
	g_msg.center = CGPointMake(c.x, c.y - glyph_diameter() / 2.0 - 20);
	[host bringSubviewToFront:g_msg];
	g_msg.hidden = NO;
	g_msg.alpha = 1;
	static unsigned gen;
	unsigned my = ++gen;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		if (my == gen && g_msg != nil) {
			[UIView animateWithDuration:0.3 animations:^{ g_msg.alpha = 0; } completion:^(BOOL f) {
				(void)f;
				if (my == gen) {
					g_msg.hidden = YES;
				}
			}];
		}
	});
}

static void
press_animation(void)
{
	UIView *g = visible_glyph();
	if (g == nil || [p_anim isEqualToString:@"none"]) {
		return;
	}
	if ([p_anim isEqualToString:@"ripple"]) {
		CGFloat d = g.bounds.size.width;
		CAShapeLayer *ring = [CAShapeLayer layer];
		ring.path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(-d / 2, -d / 2, d, d)].CGPath;
		ring.position = g.center;
		ring.fillColor = [UIColor clearColor].CGColor;
		ring.strokeColor = glyph_color().CGColor;
		ring.lineWidth = 2;
		[g.superview.layer addSublayer:ring];
		CABasicAnimation *s = [CABasicAnimation animationWithKeyPath:@"transform.scale"];
		s.fromValue = @1.0;
		s.toValue = @1.8;
		CABasicAnimation *o = [CABasicAnimation animationWithKeyPath:@"opacity"];
		o.fromValue = @0.9;
		o.toValue = @0.0;
		CAAnimationGroup *grp = [CAAnimationGroup animation];
		grp.animations = @[ s, o ];
		grp.duration = 0.6;
		grp.removedOnCompletion = NO;
		grp.fillMode = kCAFillModeForwards;
		[ring addAnimation:grp forKey:@"ripple"];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 700 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
			[ring removeFromSuperlayer];
		});
	} else {
		CAKeyframeAnimation *a = [CAKeyframeAnimation animationWithKeyPath:@"transform.scale"];
		a.values = @[ @1.0, @1.15, @1.0 ];
		a.duration = 0.3;
		[g.layer addAnimation:a forKey:@"sapphire-pulse"];
	}
}

static void
shake(void)
{
	UIView *g = visible_glyph();
	if (g == nil) {
		return;
	}
	CAKeyframeAnimation *a = [CAKeyframeAnimation animationWithKeyPath:@"transform.translation.x"];
	a.values = @[ @0, @(-12), @12, @(-9), @9, @(-5), @5, @0 ];
	a.duration = 0.45;
	a.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
	[g.layer addAnimation:a forKey:@"sapphire-shake"];
}

static void
success_animation(void)
{
	UIView *g = visible_glyph();
	if (g == nil || !p_success) {
		return;
	}
	UIView *iv = [g viewWithTag:1];
	[UIView animateWithDuration:0.3 animations:^{
		iv.tintColor = [UIColor systemGreenColor];
		g.transform = CGAffineTransformMakeScale(1.2, 1.2);
		g.alpha = 0;
	} completion:^(BOOL f) {
		(void)f;
		g.hidden = YES;
		g.transform = CGAffineTransformIdentity;
		iv.tintColor = glyph_color();
	}];
}

/* ------------------------------------------------------------------ unlock / wake */

static void
unlock_ui(int attempt)
{
	id m = lock_manager();
	if (m == nil || !msg_bool(m, "isUILocked", NO)) {
		NSLog(@TAG "unlock: UI not locked (attempt %d), nothing to do", attempt);
		return;
	}
	/* drop the auth controller's per-runloop cache so it reads the keybag fpd just unlocked */
	id ac = msg((id)[UIApplication sharedApplication], "authenticationController");
	SEL inval = sel_registerName("_invalidateCachedPasscodeLockState");
	if (ac != nil && [ac respondsToSelector:inval]) {
		((void (*)(id, SEL))objc_msgSend)(ac, inval);
	}
	SEL s1 = sel_registerName("_attemptUnlockWithPasscode:mesa:finishUIUnlock:");
	SEL s2 = sel_registerName("attemptUnlockWithPasscode:finishUIUnlock:completion:");
	SEL s3 = sel_registerName("attemptUnlockWithMesa");
	const char *how;
	BOOL r = NO;
	if ([m respondsToSelector:s1]) {
		r = ((BOOL (*)(id, SEL, id, BOOL, BOOL))objc_msgSend)(m, s1, nil, YES, YES);
		how = "_attemptUnlockWithPasscode:nil mesa:YES finishUIUnlock:YES";
	} else if ([m respondsToSelector:s2]) {
		r = ((BOOL (*)(id, SEL, id, BOOL, id))objc_msgSend)(m, s2, nil, YES, nil);
		how = "attemptUnlockWithPasscode:nil finishUIUnlock:YES completion:nil";
	} else if ([m respondsToSelector:s3]) {
		r = ((BOOL (*)(id, SEL))objc_msgSend)(m, s3);
		how = "attemptUnlockWithMesa";
	} else {
		NSLog(@TAG "unlock: no unlock method on SBLockScreenManager");
		return;
	}
	NSLog(@TAG "unlock attempt %d via %s -> %d", attempt, how, r);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
		BOOL still = msg_bool(lock_manager(), "isUILocked", NO);
		if (still && attempt == 0) {
			unlock_ui(1);
			return;
		}
		NSLog(@TAG "after attempt %d the UI is %s", attempt, still ? "STILL LOCKED" : "unlocked");
		update();
	});
}

static void
wake_screen(void)
{
	if (!blanked) {
		return;
	}
	id m = lock_manager();
	SEL tap = sel_registerName("_wakeScreenForTapToWake");
	if (m != nil && [m respondsToSelector:tap]) {
		NSLog(@TAG "press on the sensor with the screen off: waking (tap-to-wake path)");
		((void (*)(id, SEL))objc_msgSend)(m, tap);
	} else {
		NSLog(@TAG "no -[SBLockScreenManager _wakeScreenForTapToWake]: screen left off");
	}
}

/* ------------------------------------------------------------------ state and prefs */

static void
read_state(void)
{
	uint64_t v = 0;
	if (tok_state >= 0 && notify_get_state(tok_state, &v) == NOTIFY_STATUS_OK) {
		ready = v != 0;
	}
	if (ready) {
		locked_out = NO;        /* fpd says available again: the lockout is over */
	}
	v = 0;
	if (tok_blank >= 0 && notify_get_state(tok_blank, &v) == NOTIFY_STATUS_OK) {
		blanked = v != 0;
	}
}

static int
current_fails(void)
{
	uint64_t v = 0;
	if (tok_fails >= 0) {
		notify_get_state(tok_fails, &v);
	}
	return (int)v;
}

static BOOL
pbool(NSDictionary *d, NSString *k, BOOL dflt)
{
	id v = d[k];
	return [v respondsToSelector:@selector(boolValue)] ? [v boolValue] : dflt;
}

static void
apply_prefs(NSDictionary *d)
{
	p_unlock = pbool(d, @"unlockEnabled", YES);
	p_keypad = pbool(d, @"showOnKeypad", YES);
	p_fade = pbool(d, @"fadeIn", YES);
	p_success = pbool(d, @"successAnimation", YES);
	p_screen_off = pbool(d, @"screenOffWake", NO);
	NSString *s = d[@"glyphStyle"], *c = d[@"glyphColor"], *a = d[@"pressAnimation"];
	p_style = [s isKindOfClass:[NSString class]] ? s : @"touchid";
	p_color = [c isKindOfClass:[NSString class]] ? c : @"white";
	p_anim = [a isKindOfClass:[NSString class]] ? a : @"pulse";
	id size = d[@"glyphSize"], op = d[@"glyphOpacity"];
	p_size = [size respondsToSelector:@selector(doubleValue)] ? [size doubleValue] : 0;
	p_opacity = [op respondsToSelector:@selector(doubleValue)] ? MIN(1.0, MAX(0.15, [op doubleValue])) : 0.9;
	/* rebuilt with the new look on the next update */
	[g_ls removeFromSuperview];
	[g_kp removeFromSuperview];
	g_ls = g_kp = nil;
	NSLog(@TAG "prefs: style %@, color %@, size %.0f, opacity %.2f, animation %@, keypad %d, unlock %d", p_style, p_color,
	    p_size, p_opacity, p_anim, p_keypad, p_unlock);
	update();
}

static void
load_prefs(void)
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:PREFS_PATH];
		if (![d isKindOfClass:[NSDictionary class]]) {
			d = @{};
		}
		dispatch_async(dispatch_get_main_queue(), ^{ apply_prefs(d); });
	});
}

static void
watch_note(const char *name, int *tok, void (^h)(void))
{
	int t = -1;
	if (notify_register_dispatch(name, &t, dispatch_get_main_queue(), ^(int token) {
		(void)token;
		h();
	}) != NOTIFY_STATUS_OK) {
		NSLog(@TAG "cannot listen to %s", name);
		t = -1;
	}
	if (tok != NULL) {
		*tok = t;
	}
}

__attribute__((constructor)) static void
sfp_init(void)
{
	if (objc_getClass("SpringBoard") == Nil || objc_getClass("SBLockScreenManager") == Nil) {
		return;
	}
	if (notify_register_check("com.sapphire.fp.fails", &tok_fails) != NOTIFY_STATUS_OK) {
		tok_fails = -1;
	}
	watch_note("com.sapphire.fp.state", &tok_state, ^{
		read_state();
		update();
	});
	watch_note("com.apple.springboard.hasBlankedScreen", &tok_blank, ^{
		read_state();
		update();
	});
	watch_note("com.apple.springboard.lockstate", NULL, ^{
		update();
	});
	watch_note("com.sapphire.fp.prefs", NULL, ^{
		load_prefs();
	});
	watch_note("com.sapphire.fp.press", NULL, ^{
		press_animation();
	});
	watch_note("com.sapphire.fp.unlocked", NULL, ^{
		NSLog(@TAG "fingerprint matched, keybag unlocked");
		success_animation();
		unlock_ui(0);
	});
	watch_note("com.sapphire.fp.nomatch", NULL, ^{
		int n = current_fails();
		NSLog(@TAG "no match (%d)", n);
		shake();
		message(n > 0 ? [NSString stringWithFormat:@"Huella no reconocida (%d/%d)", MIN(n, MAX_FAILS), MAX_FAILS]
		              : @"Huella no reconocida", 1.6);
	});
	watch_note("com.sapphire.fp.locked_out", NULL, ^{
		NSLog(@TAG "locked out until the passcode is entered");
		message(@"Demasiados intentos. Introduce el código.", 3.0);
		locked_out = YES;
		update();
	});
	watch_note("com.sapphire.fp.wake", NULL, ^{
		read_state();
		wake_screen();
	});
	/* fpd may have published its state before SpringBoard started */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		read_state();
		load_prefs();
	});
	NSLog(@TAG "loaded (v2)");
}
