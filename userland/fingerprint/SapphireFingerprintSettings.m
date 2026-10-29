/*
 * SapphireFingerprintSettings: a "Huella dactilar" row in Settings, right after "Face ID / Touch ID y código",
 * that opens the Huella app (sapphirefp://).
 *
 * Settings' root list is PSUIPrefsListController (PreferencesUI). Its -specifiers gets an override on that
 * class only (class_addMethod: PSListController's own method, used by every other pane, is not replaced)
 * that calls the inherited one and, if the returned list does not hold the row yet, inserts it after the
 * first specifier whose identifier contains PASSCODE (TOUCHID_PASSCODE / PASSCODE / ...), or before
 * PRIVACY, or at the end. The row is a PSButtonCell with a red fingerprint icon; its action opens the URL.
 * Needs nothing from PreferenceLoader.
 */
#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#import <objc/message.h>

#define TAG "SapphireFingerprintSettings: "
#define ROW_ID @"SAPPHIRE_FINGERPRINT"
#define PS_BUTTON_CELL 13

static IMP orig_specifiers;

@interface SapphireFPSettingsTarget : NSObject
@end

@implementation SapphireFPSettingsTarget
- (void)sapphireOpenFingerprint:(id)spec
{
	(void)spec;
	NSURL *u = [NSURL URLWithString:@"sapphirefp://"];
	[[UIApplication sharedApplication] openURL:u options:@{} completionHandler:^(BOOL ok) {
		if (!ok) {
			NSLog(@TAG "sapphirefp:// did not open (Huella app not installed?)");
		}
	}];
}
@end

static SapphireFPSettingsTarget *target;

static UIImage *
row_icon(void)
{
	CGFloat s = 29;
	UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(s, s)];
	return [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
		(void)ctx;
		[[UIColor systemRedColor] setFill];
		[[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, s, s) cornerRadius:6.5] fill];
		UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:19
		    weight:UIImageSymbolWeightRegular];
		UIImage *fp = [[UIImage systemImageNamed:@"touchid" withConfiguration:cfg]
		    imageWithTintColor:[UIColor whiteColor] renderingMode:UIImageRenderingModeAlwaysOriginal];
		if (fp != nil) {
			CGSize z = fp.size;
			[fp drawInRect:CGRectMake((s - z.width) / 2, (s - z.height) / 2, z.width, z.height)];
		}
	}];
}

static id
make_row(void)
{
	Class ps = objc_getClass("PSSpecifier");
	SEL mk = sel_registerName("preferenceSpecifierNamed:target:set:get:detail:cell:edit:");
	if (ps == Nil || ![ps respondsToSelector:mk]) {
		return nil;
	}
	if (target == nil) {
		target = [SapphireFPSettingsTarget new];
	}
	id spec = ((id (*)(id, SEL, NSString *, id, SEL, SEL, Class, long long, Class))objc_msgSend)(ps, mk,
	    @"Huella dactilar", target, NULL, NULL, Nil, PS_BUTTON_CELL, Nil);
	if (spec == nil) {
		return nil;
	}
	SEL setid = sel_registerName("setIdentifier:"), setprop = sel_registerName("setProperty:forKey:");
	SEL setact = sel_registerName("setButtonAction:");
	if ([spec respondsToSelector:setid]) {
		((void (*)(id, SEL, NSString *))objc_msgSend)(spec, setid, ROW_ID);
	}
	if ([spec respondsToSelector:setact]) {
		((void (*)(id, SEL, SEL))objc_msgSend)(spec, setact, @selector(sapphireOpenFingerprint:));
	}
	if ([spec respondsToSelector:setprop]) {
		UIImage *icon = row_icon();
		if (icon != nil) {
			((void (*)(id, SEL, id, NSString *))objc_msgSend)(spec, setprop, icon, @"iconImage");
		}
		((void (*)(id, SEL, id, NSString *))objc_msgSend)(spec, setprop, @YES, @"enabled");
		((void (*)(id, SEL, id, NSString *))objc_msgSend)(spec, setprop, ROW_ID, @"id");
	}
	return spec;
}

static NSString *
spec_id(id spec)
{
	id i = [spec respondsToSelector:@selector(identifier)] ? ((id (*)(id, SEL))objc_msgSend)(spec, @selector(identifier)) : nil;
	return [i isKindOfClass:[NSString class]] ? i : nil;
}

static id
my_specifiers(id self, SEL sel)
{
	id list = ((id (*)(id, SEL))orig_specifiers)(self, sel);
	if (![list isKindOfClass:[NSMutableArray class]]) {
		return list;
	}
	NSMutableArray *a = list;
	NSUInteger after = NSNotFound, privacy = NSNotFound;
	for (NSUInteger i = 0; i < a.count; i++) {
		NSString *ident = spec_id(a[i]);
		if (ident == nil) {
			continue;
		}
		if ([ident isEqualToString:ROW_ID]) {
			return list;                    /* already there */
		}
		if (after == NSNotFound && [ident rangeOfString:@"PASSCODE"].location != NSNotFound) {
			after = i;
		}
		if (privacy == NSNotFound && [ident rangeOfString:@"PRIVACY"].location != NSNotFound) {
			privacy = i;
		}
	}
	id row = make_row();
	if (row == nil) {
		return list;
	}
	NSUInteger at = after != NSNotFound ? after + 1 : privacy != NSNotFound ? privacy : a.count;
	[a insertObject:row atIndex:at];
	NSLog(@TAG "row inserted at %lu", (unsigned long)at);
	return list;
}

static BOOL
install(void)
{
	Class c = objc_getClass("PSUIPrefsListController");
	SEL s = sel_registerName("specifiers");
	Method m = c != Nil ? class_getInstanceMethod(c, s) : NULL;
	if (m == NULL) {
		return NO;
	}
	IMP inherited = method_getImplementation(m);
	if (class_addMethod(c, s, (IMP)my_specifiers, method_getTypeEncoding(m))) {
		orig_specifiers = inherited;    /* the method was PSListController's: the override is on this class only */
	} else {
		orig_specifiers = method_setImplementation(m, (IMP)my_specifiers);
	}
	NSLog(@TAG "loaded");
	return YES;
}

__attribute__((constructor)) static void
sfps_init(void)
{
	if (install()) {
		return;
	}
	/* PreferencesUI not loaded yet: once the app has launched (before the root list is built) */
	__block id obs = [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidFinishLaunchingNotification
	    object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *n) {
		(void)n;
		if (!install()) {
			NSLog(@TAG "no -[PSUIPrefsListController specifiers]: no row");
		}
		[[NSNotificationCenter defaultCenter] removeObserver:obs];
		obs = nil;
	}];
}
