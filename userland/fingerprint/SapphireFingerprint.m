/*
 * SapphireFingerprint: the lock screen side of fingerprint unlock (fpd does the sensor and the keybag).
 *
 * A fingerprint glyph (SF Symbol "touchid" in a faint circle) sits over the sensor -- panel pixel
 * (540, 2109) of 1080 x 2400, i.e. x = 1/2 and y = 2109/2400 of the screen whatever the scale (iOS
 * draws 375 x 812 points here) -- on the cover sheet while fingerprint unlock is available
 * (com.sapphire.fp.state = 1 from fpd: passcode-locked, unlocked once since boot, templates, no
 * lockout), the UI is locked and the passcode keypad is not up. It is one UIView added to
 * CSCoverSheetViewController's view; nothing is hooked. While the cover sheet may show it, a 0.3 s
 * timer follows the keypad (-[SBLockScreenManager _isPasscodeVisible]); it stops when the screen
 * blanks or the UI unlocks.
 *
 * com.sapphire.fp.unlocked (fpd: the kernel's keybag is already unlocked): the lock screen goes away
 * the way Touch ID's does. SpringBoard's own biometric path,
 * -[SBLockScreenManager biometricAuthenticationCoordinator:requestsUnlockWithIntent:], calls
 * -[SBLockScreenManager _attemptUnlockWithPasscode:nil mesa:YES finishUIUnlock:YES]: an
 * SBFAuthenticationRequest for biometrics goes to SBFUserAuthenticationController, whose
 * -_evaluateBiometricAttempt: succeeds when -isAuthenticated (MobileKeyBag's lock state, read afresh
 * once its per-runloop cache is dropped: -_invalidateCachedPasscodeLockState) says unlocked; the
 * request's handler then runs -_finishUIUnlockFromSource:22 (no keypad, passcode overlay closed). If the
 * UI is still locked 0.6 s later it is tried once more.
 * com.sapphire.fp.nomatch: the glyph shakes. com.sapphire.fp.locked_out: the glyph is hidden.
 *
 * Everything on the main queue, from notify_register_dispatch; no sysctl, no IPC of its own beyond
 * notifyd, no haptics or HUD.
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <notify.h>

#define TAG "SapphireFingerprint: "
#define SENSOR_X        (540.0 / 1080.0)
#define SENSOR_Y        (2109.0 / 2400.0)
#define SENSOR_R        (105.0 / 1080.0)    /* radius, fraction of the width */

static UIView *glyph;
static BOOL ready, locked_out, blanked;
static int tok_state = -1, tok_blank = -1;
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
host_view(void)
{
	id vc = msg(lock_manager(), "coverSheetViewController");
	if (![vc isKindOfClass:[UIViewController class]] || ![(UIViewController *)vc isViewLoaded]) {
		return nil;
	}
	return [(UIViewController *)vc view];
}

static UIView *
make_glyph(void)
{
	UIView *v = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 74, 74)];
	v.userInteractionEnabled = NO;
	v.backgroundColor = [UIColor colorWithWhite:1.0 alpha:0.10];
	v.layer.borderColor = [UIColor colorWithWhite:1.0 alpha:0.35].CGColor;
	v.layer.borderWidth = 1.0;
	v.hidden = YES;
	UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:44
	    weight:UIImageSymbolWeightLight];
	UIImage *img = [UIImage systemImageNamed:@"touchid" withConfiguration:cfg];
	if (img != nil) {
		UIImageView *iv = [[UIImageView alloc] initWithImage:img];
		iv.tintColor = [UIColor whiteColor];
		iv.contentMode = UIViewContentModeCenter;
		iv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
		iv.frame = v.bounds;
		[v addSubview:iv];
	} else {
		NSLog(@TAG "no touchid symbol, circle only");
	}
	return v;
}

/* the sensor's centre, in the host's coordinates (from the screen's, so any scale or offset is taken in) */
static void
place(UIView *host)
{
	UIScreen *scr = [UIScreen mainScreen];
	CGRect sb = scr.bounds;
	if (sb.size.width <= 0 || sb.size.height <= 0) {
		return;
	}
	CGFloat d = MAX(70.0, 2.0 * SENSOR_R * sb.size.width);
	CGPoint c = CGPointMake(sb.origin.x + SENSOR_X * sb.size.width, sb.origin.y + SENSOR_Y * sb.size.height);
	if (host.window != nil) {
		c = [host convertPoint:c fromCoordinateSpace:scr.coordinateSpace];
	}
	glyph.bounds = CGRectMake(0, 0, d, d);
	glyph.layer.cornerRadius = d / 2.0;
	glyph.center = c;
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
	BOOL candidate = ready && !locked_out && ui_locked && !blanked;
	set_follow(candidate);
	BOOL show = candidate && !keypad;
	UIView *host = host_view();
	if (host == nil) {
		if (glyph != nil) {
			glyph.hidden = YES;
		}
		return;
	}
	if (glyph == nil) {
		if (!show) {
			return;
		}
		glyph = make_glyph();
	}
	if (glyph.superview != host) {
		[glyph removeFromSuperview];
		[host addSubview:glyph];
		NSLog(@TAG "glyph added to %@", NSStringFromClass([host class]));
	}
	if (show) {
		place(host);
		if (glyph.hidden || host.subviews.lastObject != glyph) {
			[host bringSubviewToFront:glyph];
		}
	}
	glyph.hidden = !show;
}

static void
shake(void)
{
	if (glyph == nil || glyph.hidden) {
		return;
	}
	CAKeyframeAnimation *a = [CAKeyframeAnimation animationWithKeyPath:@"transform.translation.x"];
	a.values = @[ @0, @(-12), @12, @(-9), @9, @(-5), @5, @0 ];
	a.duration = 0.45;
	a.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
	[glyph.layer addAnimation:a forKey:@"sapphire-shake"];
}

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
	if (glyph != nil) {
		glyph.hidden = YES;
	}
	if (attempt == 0) {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
			if (msg_bool(lock_manager(), "isUILocked", NO)) {
				unlock_ui(1);
			} else {
				NSLog(@TAG "UI unlocked");
			}
		});
	} else {
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
			NSLog(@TAG "after the retry the UI is %s", msg_bool(lock_manager(), "isUILocked", NO) ? "STILL LOCKED" : "unlocked");
			update();
		});
	}
}

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
	watch_note("com.sapphire.fp.unlocked", NULL, ^{
		NSLog(@TAG "fingerprint matched, keybag unlocked");
		unlock_ui(0);
	});
	watch_note("com.sapphire.fp.nomatch", NULL, ^{
		NSLog(@TAG "no match");
		shake();
	});
	watch_note("com.sapphire.fp.locked_out", NULL, ^{
		NSLog(@TAG "locked out until the passcode is entered");
		locked_out = YES;
		update();
	});
	/* fpd may have published its state before SpringBoard started */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		read_state();
		update();
	});
	NSLog(@TAG "loaded");
}
