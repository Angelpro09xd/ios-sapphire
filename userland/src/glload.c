/*
 * libredwood-glload.dylib: what a jailbreak's process-launch hook would put into apps, put there by
 * the kernel instead (xnu 0238, kern.sweet_glload=1: DYLD_INSERT_LIBRARIES of every executable inside
 * an .app bundle, SpringBoard's among them; com.sapphire.glload.plist turns it on at boot).
 *
 * - The EAGL bridge (libredwood-gl.dylib) into apps that draw with OpenGL ES -- games -- so that
 *   their EAGLContext is ours (eagl.c, "an app's own OpenGL ES"): iOS's own OpenGL ES has no driver
 *   for this GPU, and without the bridge -initWithAPI: answers nil.
 * - Tweaks: ElleKit's TweakLoader (/var/jb/usr/lib/TweakLoader.dylib), which loads the tweaks of
 *   /var/jb/usr/lib/TweakInject whose filter matches the process; into SpringBoard ElleKit's
 *   MobileSafety first, its safe mode for a tweak that crashes SpringBoard -- as ElleKit's pspawn
 *   hook does on a jailbreak. The file /var/jb/var/sapphire-notweaks: no tweaks anywhere.
 *
 * It must cost apps nothing it need not: anything that is not an app is left at once. backboardd
 * gets the bridge from the Metal bundle as before (metal/SapphireMetal). GLLOAD_DEBUG=1 in the
 * environment: what it decided, on stderr.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TWEAK_LOADER    "/var/jb/usr/lib/TweakLoader.dylib"
#define MOBILE_SAFETY   "/var/jb/usr/lib/ellekit/MobileSafety.dylib"
#define NO_TWEAKS       "/var/jb/var/sapphire-notweaks"

/*
 * Tweak filters, fast. ElleKit's TweakLoader checks each "Bundles" filter entry with
 * CFBundleGetBundleWithIdentifier(); for a bundle not loaded in this process (another app's identifier,
 * com.apple.springboard in Settings, or a framework the app does not link) CoreFoundation then walks every
 * framework and app on the system reading their Info.plists: about 1.4-1.8 s of CPU in every app launch
 * (measured: Settings 2.8 s -> 0.9 s of CPU without it), once a single such filter exists. While the loader
 * runs, the answer comes from what is loaded here instead: the app's own bundle, or a loaded image whose
 * bundle directory is named like the identifier's last component (then CoreFoundation's own answer, cheap
 * for a loaded bundle); anything else is not loaded here, which is all the filter asks.
 */
static int in_tweak_loader;

static int
loaded_bundle_named(const char *name)
{
	size_t n = strlen(name);
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *p = _dyld_get_image_name(i);
		for (const char *q = p; p != NULL && (q = strchr(q, '/')) != NULL; q++) {
			if (strncasecmp(q + 1, name, n) == 0 && q[1 + n] == '.' &&
			    (strncmp(q + 2 + n, "framework/", 10) == 0 || strncmp(q + 2 + n, "bundle/", 7) == 0 ||
			    strncmp(q + 2 + n, "appex/", 6) == 0 || strncmp(q + 2 + n, "app/", 4) == 0)) {
				return 1;
			}
		}
	}
	return 0;
}

static CFBundleRef
fast_bundle_with_identifier(CFStringRef ident)
{
	if (!in_tweak_loader || ident == NULL) {
		return CFBundleGetBundleWithIdentifier(ident);
	}
	CFBundleRef main = CFBundleGetMainBundle();
	CFStringRef mid = main != NULL ? CFBundleGetIdentifier(main) : NULL;
	if (mid != NULL && CFEqual(mid, ident)) {
		return main;
	}
	char id[256];
	if (!CFStringGetCString(ident, id, sizeof(id), kCFStringEncodingUTF8)) {
		return CFBundleGetBundleWithIdentifier(ident);
	}
	const char *last = strrchr(id, '.');
	last = last != NULL ? last + 1 : id;
	return loaded_bundle_named(last) ? CFBundleGetBundleWithIdentifier(ident) : NULL;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposers[] = {
	{ (const void *)fast_bundle_with_identifier, (const void *)CFBundleGetBundleWithIdentifier },
};

__attribute__((constructor)) static void
load_bridge(void)
{
	/* (not _dyld_get_image_name(0): while inserted libraries start, that is this library) */
	char path[1024];
	uint32_t size = sizeof(path);
	const char *program = _NSGetExecutablePath(path, &size) == 0 ? path : NULL;
	int debug = getenv("GLLOAD_DEBUG") != NULL;
	void *gl = NULL, *bridge = NULL, *safety = NULL, *tweaks = NULL;

	/* (apps, and app extensions: the kernel puts this library into both, 0316) */
	if (program == NULL || (strstr(program, ".app/") == NULL && strstr(program, ".appex/") == NULL) ||
	    strstr(program, "/backboardd") != NULL) {
		if (debug) {
			fprintf(stderr, "glload: %s: not an app\n", program ? program : "?");
		}
		return;
	}
	gl = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOLOAD | RTLD_LAZY);
	if (gl != NULL) {
		bridge = dlopen("/redwood/lib/libredwood-gl.dylib", RTLD_NOW);
	}
	if (access(NO_TWEAKS, F_OK) != 0 && access(TWEAK_LOADER, F_OK) == 0) {
		if (strstr(program, "/SpringBoard.app/SpringBoard") != NULL && access(MOBILE_SAFETY, F_OK) == 0) {
			safety = dlopen(MOBILE_SAFETY, RTLD_NOW);
		}
		in_tweak_loader = 1;
		tweaks = dlopen(TWEAK_LOADER, RTLD_NOW);
		in_tweak_loader = 0;
	}
	if (debug) {
		fprintf(stderr, "glload: %s: OpenGLES %s, bridge %s; safe mode %s, tweaks %s\n", program,
		    gl ? "linked" : "not linked", bridge ? "loaded" : gl ? dlerror() : "-", safety ? "loaded" : "-",
		    tweaks ? "loaded" : "-");
	}
}
