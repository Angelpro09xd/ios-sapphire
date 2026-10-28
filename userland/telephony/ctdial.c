/*
 * ctdial: a call placed straight through CommCenter (CoreTelephony's CTCallDial), without the Phone app,
 * InCallService or callservicesd, to see whether CommCenter's own dial path reaches the modem (QMI VOICE).
 *   ctdial NUMBER [SECONDS]     dial, print the call status changes for SECONDS (default 30), hang up
 *   ctdial -l [SECONDS]         only list call status changes
 * Signed with ctdial.entitlements (CommCenter's fine-grained "spi" and "phone").
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef const void *CTCallRef;
typedef CFNotificationCenterRef (*center_fn)(void);
typedef void (*observe_fn)(CFNotificationCenterRef, const void *, CFNotificationCallback, CFStringRef, const void *,
    CFNotificationSuspensionBehavior);

static CTCallRef (*CTCallDial_)(CFStringRef);
static int (*CTCallGetStatus_)(CTCallRef);
static void (*CTCallDisconnect_)(CTCallRef);
static CFArrayRef (*CTCopyCurrentCalls_)(void *);
static CTCallRef call;

static void
on_call(CFNotificationCenterRef center, void *observer, CFStringRef name, const void *object, CFDictionaryRef info)
{
	(void)center; (void)observer; (void)object;
	char n[128] = "";
	CFStringGetCString(name, n, sizeof(n), kCFStringEncodingUTF8);
	long status = -1, reason = -1;
	if (info) {
		CFNumberRef s = CFDictionaryGetValue(info, CFSTR("kCTCallStatus"));
		CFNumberRef r = CFDictionaryGetValue(info, CFSTR("kCTCallStatusChangeReason"));
		if (s) CFNumberGetValue(s, kCFNumberLongType, &status);
		if (r) CFNumberGetValue(r, kCFNumberLongType, &reason);
	}
	printf("event %s: status %ld reason %ld\n", n, status, reason);
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: ctdial NUMBER [SECONDS] | -l [SECONDS]\n");
		return 2;
	}
	void *ct = dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony", RTLD_NOW);
	if (!ct) {
		fprintf(stderr, "no CoreTelephony: %s\n", dlerror());
		return 1;
	}
	CTCallDial_ = dlsym(ct, "CTCallDial");
	CTCallGetStatus_ = dlsym(ct, "CTCallGetStatus");
	CTCallDisconnect_ = dlsym(ct, "CTCallDisconnect");
	CTCopyCurrentCalls_ = dlsym(ct, "_CTCopyCurrentCalls");
	center_fn center = (center_fn)dlsym(ct, "CTTelephonyCenterGetDefault");
	observe_fn observe = (observe_fn)dlsym(ct, "CTTelephonyCenterAddObserver");
	printf("CTCallDial %p, CTCallGetStatus %p, CTCallDisconnect %p, center %p\n", (void *)CTCallDial_,
	    (void *)CTCallGetStatus_, (void *)CTCallDisconnect_, (void *)center);
	if (center && observe) {
		observe(center(), NULL, on_call, NULL, NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
	}
	int secs = argc > 2 ? atoi(argv[2]) : 30;
	if (strcmp(argv[1], "-l") != 0) {
		if (!CTCallDial_) return 1;
		CFStringRef num = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
		call = CTCallDial_(num);
		printf("CTCallDial -> %p\n", call);
		fflush(stdout);
	}
	for (int i = 0; i < secs; i++) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
		if (call && CTCallGetStatus_) {
			printf("t=%d status %d\n", i + 1, CTCallGetStatus_(call));
			fflush(stdout);
		}
	}
	if (call && CTCallDisconnect_) {
		CTCallDisconnect_(call);
		printf("disconnected\n");
	}
	return 0;
}
