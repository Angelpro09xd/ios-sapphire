/*
 * libredwood-glload.dylib: brings the EAGL bridge (libredwood-gl.dylib) into apps that draw with
 * OpenGL ES -- games -- so that their EAGLContext is ours (eagl.c, "an app's own OpenGL ES").
 * iOS's own OpenGL ES has no driver for this GPU: without the bridge, -initWithAPI: answers nil.
 *
 * It is put into processes with DYLD_INSERT_LIBRARIES (launchctl setenv: until the next boot),
 * so it runs in every process started after that and must cost them nothing: anything that is
 * not an app (an executable inside an .app bundle) or has not linked OpenGLES is left at once.
 * backboardd gets the bridge from the Metal bundle as before (metal/SapphireMetal).
 */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <string.h>

__attribute__((constructor)) static void
load_bridge(void)
{
	const char *program = _dyld_get_image_name(0);

	if (program == NULL || strstr(program, ".app/") == NULL || strstr(program, "/backboardd") != NULL) {
		return;
	}
	if (dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_NOLOAD | RTLD_LAZY) == NULL) {
		return;
	}
	(void)dlopen("/redwood/lib/libredwood-gl.dylib", RTLD_NOW);
}
