/*
 * memdump: reading code and data out of the shared cache, for crash analysis on the phone (the cache's slide
 * is the same in every process of a boot, so addresses found here are valid for the crashed app too).
 *
 *   memdump imp LIB CLASS SELECTOR       dlopen LIB; the method's IMP, its image and offset
 *   memdump hex ADDR LEN                 LEN bytes at ADDR (hex) as hex words
 *   memdump str ADDR                     the C string at ADDR, or the CFString/selector it points to
 *   memdump base LIB                     the load address of LIB (dlopen)
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc >= 5 && !strcmp(argv[1], "imp")) {
			if (!dlopen(argv[2], RTLD_NOW)) {
				printf("dlopen: %s\n", dlerror());
				return 1;
			}
			Class c = objc_getClass(argv[3]);
			if (!c) {
				printf("no class %s\n", argv[3]);
				return 1;
			}
			Method m = class_getInstanceMethod(c, sel_registerName(argv[4]));
			if (!m) {
				printf("no method\n");
				return 1;
			}
			IMP imp = method_getImplementation(m);
			Dl_info di;
			dladdr((void *)imp, &di);
			printf("imp %p image %s base %p off 0x%lx\n", (void *)imp, di.dli_fname, di.dli_fbase,
			    (unsigned long)((uintptr_t)imp - (uintptr_t)di.dli_fbase));
			return 0;
		}
		if (argc >= 3 && !strcmp(argv[1], "base")) {
			void *h = dlopen(argv[2], RTLD_NOW);
			for (uint32_t i = 0; i < _dyld_image_count(); i++) {
				if (strstr(_dyld_get_image_name(i), argv[2]) || (h && strcmp(_dyld_get_image_name(i), argv[2]) == 0)) {
					printf("%s %p\n", _dyld_get_image_name(i), (void *)_dyld_get_image_header(i));
				}
			}
			return 0;
		}
		if (argc >= 4 && !strcmp(argv[1], "hex")) {
			const uint32_t *p = (const uint32_t *)strtoull(argv[2], NULL, 16);
			unsigned long n = strtoul(argv[3], NULL, 0) / 4;
			for (unsigned long i = 0; i < n; i++) {
				printf("%08x%c", p[i], (i % 8 == 7) ? '\n' : ' ');
			}
			printf("\n");
			return 0;
		}
		if (argc >= 3 && !strcmp(argv[1], "str")) {
			const char *p = (const char *)strtoull(argv[2], NULL, 16);
			Dl_info di;
			if (dladdr(p, &di)) {
				printf("[in %s +0x%lx sym %s] ", di.dli_fname, (unsigned long)(p - (const char *)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
			}
			int printable = 1;
			for (int i = 0; i < 8 && p[i]; i++) {
				if (p[i] < 0x20 || p[i] > 0x7e) {
					printable = 0;
				}
			}
			if (printable && p[0]) {
				printf("cstr \"%.200s\"\n", p);
			} else {
				const void *const *q = (const void *const *)p;
				const char *inner = (const char *)q[0];
				Dl_info d2;
				if (dladdr(inner, &d2) && inner[0] >= 0x20 && inner[0] <= 0x7e) {
					printf("ptr -> \"%.200s\"\n", inner);
				} else {
					/* a CFString literal: isa, flags, char *, length */
					const char *s = (const char *)q[2];
					if (dladdr(s, &d2)) {
						printf("cfstring \"%.200s\"\n", s);
					} else {
						printf("raw %p %p %p %p\n", q[0], q[1], q[2], q[3]);
					}
				}
			}
			return 0;
		}
		if (argc >= 3 && !strcmp(argv[1], "lsdc")) {
			dlopen("/System/Library/PrivateFrameworks/MobileSafari.framework/MobileSafari", RTLD_NOW);
			Class P = objc_getClass("LSApplicationProxy");
			id proxy = [P performSelector:sel_registerName("applicationProxyForIdentifier:") withObject:[NSString stringWithUTF8String:argv[2]]];
			NSLog(@"proxy %@ installed %@ dataContainerURL %@", proxy, [proxy valueForKey:@"installed"], [proxy valueForKey:@"dataContainerURL"]);
			NSFileManager *fm = NSFileManager.defaultManager;
			SEL s = sel_registerName("safari_mobileSafariContainerDirectoryURL");
			if ([fm respondsToSelector:s]) {
				NSLog(@"safari_mobileSafariContainerDirectoryURL = %@", [fm performSelector:s]);
			}
			return 0;
		}
		if (argc >= 3 && !strcmp(argv[1], "ext")) {
			Class E = objc_getClass("NSExtension");
			NSError *err = nil;
			id e = ((id (*)(Class, SEL, id, NSError **))objc_msgSend)(E, sel_registerName("extensionWithIdentifier:error:"), [NSString stringWithUTF8String:argv[2]], &err);
			NSLog(@"extension %@ error %@", e, err);
			Class R = objc_getClass("LSApplicationExtensionRecord");
			id r = ((id (*)(id, SEL, id, NSError **))objc_msgSend)([R alloc], sel_registerName("initWithBundleIdentifier:error:"), [NSString stringWithUTF8String:argv[2]], &err);
			NSLog(@"LS record %@ url %@ error %@", r, r ? [r valueForKey:@"URL"] : nil, err);
			return 0;
		}
		fprintf(stderr, "usage: memdump imp LIB CLASS SEL | base LIB | hex ADDR LEN | str ADDR\n");
		return 2;
	}
}
