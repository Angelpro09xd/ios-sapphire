/*
 * btpower [on|off|state]: what Settings' Bluetooth switch does, from the command line:
 * BluetoothManager.framework (private) -> bluetoothd. Prints available / powered / enabled before and after.
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>

@interface BluetoothManagerX : NSObject
+ (id)sharedInstance;
- (BOOL)available;
- (BOOL)powered;
- (BOOL)enabled;
- (int)powerState;
- (BOOL)setPowered:(BOOL)p;
- (BOOL)setEnabled:(BOOL)e;
- (id)localAddress;
@end

static void
show(id m, const char *when)
{
	NSLog(@"btpower %s: available %d powered %d enabled %d powerState %d address %@", when,
	    [m available], [m powered], [m enabled], [m respondsToSelector:@selector(powerState)] ? [m powerState] : -1,
	    [m respondsToSelector:@selector(localAddress)] ? [m localAddress] : @"?");
	printf("%s: available %d powered %d enabled %d\n", when, [m available], [m powered], [m enabled]);
	fflush(stdout);
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (!dlopen("/System/Library/PrivateFrameworks/BluetoothManager.framework/BluetoothManager", RTLD_NOW)) {
			printf("no BluetoothManager: %s\n", dlerror());
			return 1;
		}
		Class c = NSClassFromString(@"BluetoothManager");
		id m = [c sharedInstance];
		[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:3]];
		show(m, "start");
		const char *what = argc > 1 ? argv[1] : "state";
		if (!strcmp(what, "on") || !strcmp(what, "off")) {
			BOOL on = !strcmp(what, "on");
			BOOL r1 = [m setEnabled:on];
			BOOL r2 = [m setPowered:on];
			printf("setEnabled %d -> %d, setPowered %d -> %d\n", on, r1, on, r2);
			[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:8]];
			show(m, "after");
		}
	}
	return 0;
}
