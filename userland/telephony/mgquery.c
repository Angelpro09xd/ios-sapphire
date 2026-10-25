/* mgquery KEY...: MobileGestalt answers (MGCopyAnswer), to see what iOS thinks the device can do */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
	void *h = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
	CFTypeRef (*copy)(CFStringRef, CFDictionaryRef) = h ? dlsym(h, "MGCopyAnswer") : NULL;
	if (copy == NULL) {
		fprintf(stderr, "no MGCopyAnswer\n");
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		CFStringRef k = CFStringCreateWithCString(NULL, argv[i], kCFStringEncodingUTF8);
		CFTypeRef v = copy(k, NULL);
		char buf[256] = "(null)";
		if (v != NULL) {
			CFStringRef d = CFCopyDescription(v);
			CFStringGetCString(d, buf, sizeof(buf), kCFStringEncodingUTF8);
			CFRelease(d);
			CFRelease(v);
		}
		printf("%-34s %s\n", argv[i], buf);
		CFRelease(k);
	}
	return 0;
}
