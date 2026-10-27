/*
 * libredwood-glload.dylib: brings the EAGL bridge (libredwood-gl.dylib) into apps that draw with
 * OpenGL ES -- games -- so that their EAGLContext is ours (eagl.c, "an app's own OpenGL ES").
 * iOS's own OpenGL ES has no driver for this GPU: without the bridge, -initWithAPI: answers nil.
 *
 * The kernel puts it into every app's DYLD_INSERT_LIBRARIES (xnu 0238, kern.sweet_glload=1), so
 * it must cost apps nothing: anything that is not an app (an executable inside an .app bundle) or
 * has not linked OpenGLES is left at once. backboardd gets the bridge from the Metal bundle as
 * before (metal/SapphireMetal). GLLOAD_DEBUG=1 in the environment: what it decided, on stderr.
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((constructor)) static void
load_bridge(void)
{
	/* (not _dyld_get_image_name(0): while inserted libraries start, that is this library) */
	char path[1024];
	uint32_t size = sizeof(path);
	const char *program = _NSGetExecutablePath(path, &size) == 0 ? path : NULL;
	int debug = getenv("GLLOAD_DEBUG") != NULL;
	void *gl = NULL, *bridge = NULL;

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
	if (debug) {
		fprintf(stderr, "glload: %s: OpenGLES %s, bridge %s\n", program, gl ? "linked" : "not linked",
		    bridge ? "loaded" : gl ? dlerror() : "-");
	}
}
