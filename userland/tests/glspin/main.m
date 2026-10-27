/*
 * GLSpin: an app that draws the way an OpenGL ES game does -- a full-screen CAEAGLLayer, an ES 3
 * context, a CADisplayLink, a renderbuffer from the layer, presentRenderbuffer: -- with a
 * game-like load (a few hundred textured, blended quads a frame, some of them screen-sized), and
 * writes its frame rate to /var/tmp/glspin.log every 2 s. It is the test of whether apps can draw
 * with OpenGL ES on this phone, and how fast.
 *
 * File /var/tmp/glspin.cfg, if there: "DRAWS FULL" (quads a frame, of which screen-sized).
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <OpenGLES/ES3/gl.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <unistd.h>

static FILE *logf_;

static void
say(const char *format, ...)
{
	va_list ap;
	if (logf_ == NULL) {
		logf_ = fopen("/var/tmp/glspin.log", "a");
	}
	if (logf_ == NULL) {
		return;
	}
	va_start(ap, format);
	vfprintf(logf_, format, ap);
	va_end(ap);
	fflush(logf_);
}

static double
now_ms(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

@interface SpinView : UIView
@end

@implementation SpinView {
	EAGLContext *_context;
	GLuint _fb, _rb, _prog, _tex[4];
	GLint _w, _h, _tint, _offset;
	CADisplayLink *_link;
	int _draws, _full, _frames;
	double _t0, _cpu;
}

+ (Class)layerClass
{
	return [CAEAGLLayer class];
}

static GLuint
compile(GLenum type, const char *text)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	char log[512];
	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		say("shader: %s\n", log);
	}
	return s;
}

- (instancetype)initWithFrame:(CGRect)frame
{
	self = [super initWithFrame:frame];
	CAEAGLLayer *layer = (CAEAGLLayer *)self.layer;
	layer.opaque = YES;
	layer.contentsScale = [UIScreen mainScreen].scale;
	_draws = 300;
	_full = 3;
	FILE *cfg = fopen("/var/tmp/glspin.cfg", "r");
	if (cfg != NULL) {
		fscanf(cfg, "%d %d", &_draws, &_full);
		fclose(cfg);
	}
	_context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
	say("context %p (scale %.1f, %d draws a frame, %d of them screen-sized)\n", _context, layer.contentsScale, _draws, _full);
	if (_context == nil || ![EAGLContext setCurrentContext:_context]) {
		say("no context\n");
		return self;
	}
	say("GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	glGenFramebuffers(1, &_fb);
	glGenRenderbuffers(1, &_rb);
	glBindFramebuffer(GL_FRAMEBUFFER, _fb);
	glBindRenderbuffer(GL_RENDERBUFFER, _rb);
	BOOL stored = [_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, _rb);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &_w);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &_h);
	say("renderbufferStorage:fromDrawable: %d, %d x %d, framebuffer status %#x\n", stored, _w, _h,
	    glCheckFramebufferStatus(GL_FRAMEBUFFER));

	const char *vs = "#version 300 es\nin vec2 p; uniform vec4 off; out vec2 t;\n"
	    "void main() { t = p * 0.5 + 0.5; gl_Position = vec4(p * off.zw + off.xy, 0.0, 1.0); }\n";
	const char *fs = "#version 300 es\nprecision mediump float; uniform sampler2D s; uniform vec4 tint; in vec2 t; out vec4 c;\n"
	    "void main() { c = texture(s, t) * tint; }\n";
	_prog = glCreateProgram();
	glAttachShader(_prog, compile(GL_VERTEX_SHADER, vs));
	glAttachShader(_prog, compile(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(_prog, 0, "p");
	glLinkProgram(_prog);
	glUseProgram(_prog);
	_tint = glGetUniformLocation(_prog, "tint");
	_offset = glGetUniformLocation(_prog, "off");
	glUniform1i(glGetUniformLocation(_prog, "s"), 0);
	static uint32_t pixels[128 * 128];
	glGenTextures(4, _tex);
	for (int t = 0; t < 4; t++) {
		for (int i = 0; i < 128 * 128; i++) {
			pixels[i] = 0xff000000u | (uint32_t)((i * (t + 3) * 2654435761u) >> 8);
		}
		glBindTexture(GL_TEXTURE_2D, _tex[t]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	static const GLfloat quad[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint vbo;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	_link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
	if (@available(iOS 15.0, *)) {
		_link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
	}
	[_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
	_t0 = now_ms();
	return self;
}

- (void)frame:(CADisplayLink *)link
{
	double began = now_ms();
	float phase = (float)(began / 1000.0);

	glBindFramebuffer(GL_FRAMEBUFFER, _fb);
	glViewport(0, 0, _w, _h);
	glClearColor(0.1f, 0.1f, 0.2f + 0.1f * sinf(phase), 1);
	glClear(GL_COLOR_BUFFER_BIT);
	for (int i = 0; i < _draws; i++) {
		bool full = i < _full;
		float size = full ? 1.0f : 0.06f + 0.04f * (float)(i % 5);
		float x = full ? 0 : sinf(phase * (0.3f + 0.01f * (float)i) + (float)i) * 0.9f;
		float y = full ? 0 : cosf(phase * (0.2f + 0.013f * (float)i) + (float)i * 1.7f) * 0.9f;
		glBindTexture(GL_TEXTURE_2D, _tex[i % 4]);
		glUniform4f(_offset, x, y, size, size * (float)_w / (float)_h);
		glUniform4f(_tint, 1, 1, 1, full ? 0.6f : 1.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glBindRenderbuffer(GL_RENDERBUFFER, _rb);
	[_context presentRenderbuffer:GL_RENDERBUFFER];
	_cpu += now_ms() - began;
	_frames++;
	double t = now_ms();
	if (t - _t0 >= 2000) {
		say("%.1f fps, %.2f ms of CPU a frame (draws and present)\n", _frames * 1000.0 / (t - _t0), _cpu / _frames);
		_frames = 0;
		_cpu = 0;
		_t0 = t;
	}
}
@end

@interface SpinDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation SpinDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	say("--- GLSpin started\n");
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	UIViewController *vc = [[UIViewController alloc] init];
	vc.view = [[SpinView alloc] initWithFrame:self.window.bounds];
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];
	return YES;
}
@end

int
main(int argc, char *argv[])
{
	/*
	 * The EAGL bridge, loaded here before the first EAGLContext (file /var/tmp/glspin.nobridge:
	 * not -- to see whether libredwood-glload brought it, or what iOS's own OpenGL ES does).
	 * Its log lines go to the same file.
	 */
	freopen("/var/tmp/glspin.log", "a", stderr);
	if (access("/var/tmp/glspin.nobridge", F_OK) != 0) {
		void *bridge = dlopen("/redwood/lib/libredwood-gl.dylib", RTLD_NOW);
		say("bridge: %s\n", bridge != NULL ? "loaded" : dlerror());
	} else {
		say("bridge: %s\n", dlopen("/redwood/lib/libredwood-gl.dylib", RTLD_NOLOAD | RTLD_LAZY) != NULL ?
		    "already here (libredwood-glload)" : "not loaded");
	}
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([SpinDelegate class]));
	}
}
