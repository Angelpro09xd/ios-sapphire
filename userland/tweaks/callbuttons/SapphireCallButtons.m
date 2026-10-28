/*
 * SapphireCallButtons: the call screen's speaker and mute buttons, done here.
 *
 * iOS routes a phone call's sound through the modem's own audio device ("Baseband Voice", VirtualAudio's
 * RoutingHandler_Call); sapphire has none -- the call's sound goes between the modem and the codec in the
 * audio DSP (userland/audio/arplay) -- and the silent one tried in RedwoodAudio.driver kept the call
 * screen from coming up. So the call has one route, the speaker button is grey, and mute reaches nothing.
 * Here, in InCallService's controls (PHAudioCallControlsViewController):
 *   speaker  always enabled; a tap toggles it, posted as "com.sapphire.audio.receiver" (state 1: the
 *            receiver, 0: the speaker), which arplay applies to the codec (top speaker / bottom speaker);
 *   mute     a tap toggles "com.sapphire.call.mute" (state 1: muted), arplay switches the microphones'
 *            ADCs off; iOS's own mute still runs too.
 * Both are shown selected from these states, which a new call ("com.sapphire.call.audio" going to 1)
 * clears. The buttons are told apart by their titles (control type values are logged once).
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <notify.h>

enum { ROLE_OTHER = 1, ROLE_SPEAKER, ROLE_MUTE };

static int roles[64];
static BOOL speaker_on, mute_on;
static IMP orig_enabled, orig_selected, orig_tapped;

static void
post(const char *name, uint64_t state)
{
	int token;
	if (notify_register_check(name, &token) == NOTIFY_STATUS_OK) {
		notify_set_state(token, state);
		notify_post(name);
		notify_cancel(token);
	}
}

static int
role_of(id self, long type)
{
	if (type < 0 || type >= 64) {
		return ROLE_OTHER;
	}
	if (roles[type] == 0) {
		NSString *title = ((id (*)(id, SEL, long))objc_msgSend)(self, sel_registerName("titleForControlType:"), type);
		NSString *t = [title isKindOfClass:[NSString class]] ? title.lowercaseString : @"";
		roles[type] = ([t containsString:@"altavoz"] || [t containsString:@"speaker"] || [t containsString:@"audio"]) ? ROLE_SPEAKER :
		    ([t containsString:@"silenc"] || [t containsString:@"mute"]) ? ROLE_MUTE : ROLE_OTHER;
		NSLog(@"SapphireCallButtons: control type %ld \"%@\" -> %s", type, title,
		    roles[type] == ROLE_SPEAKER ? "speaker" : roles[type] == ROLE_MUTE ? "mute" : "other");
	}
	return roles[type];
}

static void
refresh(id self)
{
	id view = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("controlsView"));
	if ([view respondsToSelector:sel_registerName("reloadButtonViewStates")]) {
		((void (*)(id, SEL))objc_msgSend)(view, sel_registerName("reloadButtonViewStates"));
	}
}

static BOOL
my_enabled(id self, SEL sel, long type)
{
	BOOL r = ((BOOL (*)(id, SEL, long))orig_enabled)(self, sel, type);
	return role_of(self, type) == ROLE_SPEAKER ? YES : r;
}

static BOOL
my_selected(id self, SEL sel, long type)
{
	switch (role_of(self, type)) {
	case ROLE_SPEAKER:
		return speaker_on;
	case ROLE_MUTE:
		return mute_on;
	default:
		return ((BOOL (*)(id, SEL, long))orig_selected)(self, sel, type);
	}
}

static void
my_tapped(id self, SEL sel, long type, id view)
{
	switch (role_of(self, type)) {
	case ROLE_SPEAKER:
		speaker_on = !speaker_on;
		post("com.sapphire.audio.receiver", !speaker_on);
		NSLog(@"SapphireCallButtons: speaker %s", speaker_on ? "on" : "off");
		refresh(self);
		return;                                 /* (iOS's own: "could not find available route") */
	case ROLE_MUTE:
		mute_on = !mute_on;
		post("com.sapphire.call.mute", mute_on);
		NSLog(@"SapphireCallButtons: mute %s", mute_on ? "on" : "off");
		((void (*)(id, SEL, long, id))orig_tapped)(self, sel, type, view);
		refresh(self);
		return;
	default:
		((void (*)(id, SEL, long, id))orig_tapped)(self, sel, type, view);
	}
}

static void
hook(Class c, const char *name, IMP mine, IMP *orig)
{
	Method m = class_getInstanceMethod(c, sel_registerName(name));
	if (m != NULL) {
		*orig = method_setImplementation(m, mine);
	} else {
		NSLog(@"SapphireCallButtons: no %s", name);
	}
}

__attribute__((constructor)) static void
install(void)
{
	Class c = objc_getClass("PHAudioCallControlsViewController");
	if (c == Nil) {
		return;
	}
	hook(c, "controlTypeIsEnabled:", (IMP)my_enabled, &orig_enabled);
	hook(c, "controlTypeIsSelected:", (IMP)my_selected, &orig_selected);
	hook(c, "controlTypeTapped:forView:", (IMP)my_tapped, &orig_tapped);
	int token;
	notify_register_dispatch("com.sapphire.call.audio", &token, dispatch_get_main_queue(), ^(int t) {
		uint64_t st = 0;
		notify_get_state(t, &st);
		if (st) {
			speaker_on = NO;
			mute_on = NO;
			post("com.sapphire.call.mute", 0);
		}
	});
}
