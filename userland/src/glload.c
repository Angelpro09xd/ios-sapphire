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
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TWEAK_LOADER    "/var/jb/usr/lib/TweakLoader.dylib"
#define MOBILE_SAFETY   "/var/jb/usr/lib/ellekit/MobileSafety.dylib"
#define NO_TWEAKS       "/var/jb/var/sapphire-notweaks"

__attribute__((constructor)) static void
load_bridge(void)
{
	/* (not _dyld_get_image_name(0): while inserted libraries start, that is this library) */
	char path[1024];
	uint32_t size = sizeof(path);
	const char *program = _NSGetExecutablePath(path, &size) == 0 ? path : NULL;
	int debug = getenv("GLLOAD_DEBUG") != NULL;
	void *gl = NULL, *bridge = NULL, *safety = NULL, *tweaks = NULL;

	if (program == NULL || strstr(program, ".app/") == NULL || strstr(program, "/backboardd") != NULL) {
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
		tweaks = dlopen(TWEAK_LOADER, RTLD_NOW);
	}
	if (debug) {
		fprintf(stderr, "glload: %s: OpenGLES %s, bridge %s; safe mode %s, tweaks %s\n", program,
		    gl ? "linked" : "not linked", bridge ? "loaded" : gl ? dlerror() : "-", safety ? "loaded" : "-",
		    tweaks ? "loaded" : "-");
	}
}
