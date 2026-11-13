/*
 * SapphireSafari: Safari closed at once at launch.
 *
 * -[Application application:didFinishLaunchingWithOptions:] (MobileSafariUI) builds a file URL from
 * SFSafariIconImageCacheDirectoryPath(), which hangs off -[NSFileManager safari_mobileSafariContainerDirectoryURL]
 * (SafariCore). That asks sandbox_container_path_for_pid(getpid()) first: Sapphire's sandbox is permissive
 * and keeps no app containers, so it fails; then [[LSApplicationRecord alloc] initWithBundleIdentifier:
 * @"com.apple.mobilesafari" ...].dataContainerURL, nil here too. +[NSURL fileURLWithPath:nil] throws.
 *
 * Here, when SafariCore's answer is nil, the container is Safari's own data container as launchd gave it to
 * the process (its HOME, /var/mobile/Containers/Data/Application/<UUID>) when it is one, else the one
 * LaunchServices / containermanagerd would have named: found by its metadata (MCMMetadataIdentifier).
 * Injected wherever SafariCore is loaded (Safari, SafariViewService, WebKit's in-app browsers).
 */
#import <Foundation/Foundation.h>
#include <objc/runtime.h>

typedef void (*getter_t)(id, SEL, NSURL **, BOOL *);
static getter_t orig_get;

/* the data container of BUNDLE: its metadata names it */
static NSURL *
container_of(NSString *bundle)
{
	NSString *home = NSHomeDirectory();
	if ([home containsString:@"/Containers/Data/Application/"]) {
		NSDictionary *m = [NSDictionary dictionaryWithContentsOfFile:
		    [home stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"]];
		if (m == nil || [m[@"MCMMetadataIdentifier"] isEqual:bundle]) {
			return [NSURL fileURLWithPath:home isDirectory:YES];
		}
	}
	NSString *root = @"/var/mobile/Containers/Data/Application";
	for (NSString *d in [NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:nil]) {
		NSString *c = [root stringByAppendingPathComponent:d];
		NSDictionary *m = [NSDictionary dictionaryWithContentsOfFile:
		    [c stringByAppendingPathComponent:@".com.apple.mobile_container_manager.metadata.plist"]];
		if ([m[@"MCMMetadataIdentifier"] isEqual:bundle]) {
			return [NSURL fileURLWithPath:c isDirectory:YES];
		}
	}
	return nil;
}

static void
get_container(id self, SEL sel, NSURL **url, BOOL *containerized)
{
	NSURL *u = nil;
	BOOL c = NO;
	if (orig_get != NULL) {
		orig_get(self, sel, &u, &c);
	}
	if (u == nil) {
		static NSURL *found;
		static dispatch_once_t once;
		dispatch_once(&once, ^{
			found = container_of(@"com.apple.mobilesafari");
		});
		u = found;
		c = u != nil;
	}
	if (url != NULL) {
		*url = u;
	}
	if (containerized != NULL) {
		*containerized = c;
	}
}

__attribute__((constructor)) static void
install(void)
{
	Class fm = objc_getClass("NSFileManager");
	Method m = fm != Nil ? class_getInstanceMethod(fm, sel_registerName("_safari_getMobileSafariContainerDirectoryURL:isContainerized:")) : NULL;
	if (m != NULL) {
		orig_get = (getter_t)method_setImplementation(m, (IMP)get_container);
	}
}
