/*
 * glbench: what the GPU and the driver cost here, without a window server in the way.
 *
 *   glbench [W H]      (default 1125 x 2436, the display's size)
 *
 * 1. fill: 20 full-target quads, textured and blended (what a layer over the whole screen costs
 *    the GPU), into a texture of the GPU's own memory and into an IOSurface made a texture
 *    (redwood_gl_surface_texture: CoreAnimation's surfaces are such), timed by GL_TIME_ELAPSED
 *    and by the clock around a glFinish.
 * 2. draws: 2000 small quads with a texture bind and a uniform between them (CoreAnimation's
 *    pattern), the CPU's time a draw until the flush, and the GPU's.
 * Links src/gl_osmesa.c and libOSMesa (userland/build-gl.sh's); run as root.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/redwood_gl.h"

typedef unsigned GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;

#define GL_TEXTURE_2D           0x0de1
#define GL_RGBA                 0x1908
#define GL_BGRA                 0x80e1
#define GL_UNSIGNED_BYTE        0x1401
#define GL_FLOAT                0x1406
#define GL_TRIANGLE_STRIP       0x0005
#define GL_FRAMEBUFFER          0x8d40
#define GL_COLOR_ATTACHMENT0    0x8ce0
#define GL_FRAMEBUFFER_COMPLETE 0x8cd5
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_LINEAR               0x2601
#define GL_BLEND                0x0be2
#define GL_ONE                  1
#define GL_ONE_MINUS_SRC_ALPHA  0x0303
#define GL_VERTEX_SHADER        0x8b31
#define GL_FRAGMENT_SHADER      0x8b30
#define GL_COMPILE_STATUS       0x8b81
#define GL_LINK_STATUS          0x8b82
#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_TIME_ELAPSED         0x88bf
#define GL_QUERY_RESULT         0x8866
#define GL_RENDERER             0x1f01

extern void glGenTextures(GLsizei, GLuint *);
extern void glBindTexture(GLenum, GLuint);
extern void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
extern void glTexParameteri(GLenum, GLenum, GLint);
extern void glGenFramebuffers(GLsizei, GLuint *);
extern void glBindFramebuffer(GLenum, GLuint);
extern void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
extern GLenum glCheckFramebufferStatus(GLenum);
extern void glViewport(GLint, GLint, GLsizei, GLsizei);
extern void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
extern void glClear(GLbitfield);
extern void glEnable(GLenum);
extern void glBlendFunc(GLenum, GLenum);
extern GLuint glCreateShader(GLenum);
extern void glShaderSource(GLuint, GLsizei, const GLchar *const *, const GLint *);
extern void glCompileShader(GLuint);
extern void glGetShaderiv(GLuint, GLenum, GLint *);
extern void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, GLchar *);
extern GLuint glCreateProgram(void);
extern void glAttachShader(GLuint, GLuint);
extern void glBindAttribLocation(GLuint, GLuint, const GLchar *);
extern void glLinkProgram(GLuint);
extern void glUseProgram(GLuint);
extern GLint glGetUniformLocation(GLuint, const GLchar *);
extern void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
extern void glUniform1i(GLint, GLint);
extern void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
extern void glEnableVertexAttribArray(GLuint);
extern void glDrawArrays(GLenum, GLint, GLsizei);
extern void glFlush(void);
extern void glFinish(void);
extern unsigned glGetError(void);
extern void glGenQueries(GLsizei, GLuint *);
extern void glBeginQuery(GLenum, GLuint);
extern void glEndQuery(GLenum);
extern void glGetQueryObjectuiv(GLuint, GLenum, GLuint *);
extern const unsigned char *glGetString(GLenum);
extern void glActiveTexture(GLenum);

static double
now_ms(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static GLuint
shader(GLenum type, const char *text)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[512];

	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "glbench: shader: %s\n", log);
		exit(1);
	}
	return s;
}

static GLuint query;

/* n quads at (x, y, w, h) in [-1, 1] coordinates, a texture bind and a uniform before each */
static void
quads(GLint tint, const GLuint *textures, int ntextures, int n, float size)
{
	for (int i = 0; i < n; i++) {
		float x = size >= 2 ? -1 : -1 + (float)((i * 37) % 97) / 50.0f;
		float y = size >= 2 ? -1 : -1 + (float)((i * 53) % 89) / 46.0f;
		GLfloat corners[8] = { x, y, x + size, y, x, y + size, x + size, y + size };
		glBindTexture(GL_TEXTURE_2D, textures[i % ntextures]);
		glUniform4f(tint, 1, 1, 1, 0.5f + 0.4f * (float)(i % 2));
		glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, corners);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
}

/* the CPU's ms until the flush returned, the GPU's ms (query), the ms until glFinish returned */
static void
timed(const char *what, GLint tint, const GLuint *textures, int ntextures, int n, float size)
{
	GLuint gpu_ns = 0;
	double t0, t1, t2;

	glFinish();
	t0 = now_ms();
	glBeginQuery(GL_TIME_ELAPSED, query);
	quads(tint, textures, ntextures, n, size);
	glEndQuery(GL_TIME_ELAPSED);
	glFlush();
	t1 = now_ms();
	glFinish();
	t2 = now_ms();
	glGetQueryObjectuiv(query, GL_QUERY_RESULT, &gpu_ns);
	printf("%-34s cpu %7.2f ms (%5.1f us a draw)  gpu %7.2f ms  until done %7.2f ms\n", what, t1 - t0,
	    (t1 - t0) * 1000 / n, gpu_ns / 1e6, t2 - t0);
}

static GLuint
target(GLuint texture, int w, int h)
{
	GLuint fb;

	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "glbench: framebuffer incomplete\n");
		exit(1);
	}
	glViewport(0, 0, w, h);
	glClearColor(0.1f, 0.2f, 0.3f, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	return fb;
}

static IOSurfaceRef
surface(int w, int h)
{
	int bpe = 4, fmt = 'BGRA';
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
	CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe), nf = CFNumberCreate(NULL, kCFNumberIntType, &fmt);

	CFDictionarySetValue(d, kIOSurfaceWidth, nw);
	CFDictionarySetValue(d, kIOSurfaceHeight, nh);
	CFDictionarySetValue(d, kIOSurfaceBytesPerElement, nb);
	CFDictionarySetValue(d, kIOSurfacePixelFormat, nf);
	return IOSurfaceCreate(d);
}

int
main(int argc, char **argv)
{
	int w = argc > 2 ? atoi(argv[1]) : 1125, h = argc > 2 ? atoi(argv[2]) : 2436;
	struct redwood_gl *gl = redwood_gl_create(3);
	GLuint textures[8], own, fb_own, fb_surf, surf_tex, prog;
	static uint32_t pixels[256 * 256];
	const char *vs = "#version 300 es\nin vec2 p; out vec2 t; void main() { t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
	const char *fs = "#version 300 es\nprecision mediump float; uniform sampler2D s; uniform vec4 tint; in vec2 t; out vec4 c;\n"
	    "void main() { c = texture(s, t) * tint; }\n";

	if (gl == NULL || !redwood_gl_make_current(gl)) {
		fprintf(stderr, "glbench: no context\n");
		return 1;
	}
	printf("glbench: %s, target %d x %d\n", glGetString(GL_RENDERER), w, h);
	glGenQueries(1, &query);

	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "s"), 0);
	GLint tint = glGetUniformLocation(prog, "tint");
	glEnableVertexAttribArray(0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glGenTextures(8, textures);
	for (int t = 0; t < 8; t++) {
		for (int i = 0; i < 256 * 256; i++) {
			pixels[i] = 0x80000000u | (uint32_t)(i * (t + 1) * 2654435761u >> 8);
		}
		glBindTexture(GL_TEXTURE_2D, textures[t]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}

	glGenTextures(1, &own);
	glBindTexture(GL_TEXTURE_2D, own);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	fb_own = target(own, w, h);

	IOSurfaceRef s = surface(w, h);
	glGenTextures(1, &surf_tex);
	glBindTexture(GL_TEXTURE_2D, surf_tex);
	if (s == NULL || !redwood_gl_surface_texture(gl, IOSurfaceGetID(s))) {
		fprintf(stderr, "glbench: no IOSurface texture\n");
		return 1;
	}
	fb_surf = target(surf_tex, w, h);

	for (int round = 0; round < 3; round++) {
		printf("-- round %d\n", round + 1);
		glBindFramebuffer(GL_FRAMEBUFFER, fb_own);
		timed("fill 20 x full, GPU memory", tint, textures, 8, 20, 2);
		glBindFramebuffer(GL_FRAMEBUFFER, fb_surf);
		timed("fill 20 x full, IOSurface", tint, textures, 8, 20, 2);
		glBindFramebuffer(GL_FRAMEBUFFER, fb_own);
		timed("2000 small draws, GPU memory", tint, textures, 8, 2000, 0.05f);
		glBindFramebuffer(GL_FRAMEBUFFER, fb_surf);
		timed("2000 small draws, IOSurface", tint, textures, 8, 2000, 0.05f);
	}
	printf("errors: %#x\n", glGetError());
	return 0;
}
