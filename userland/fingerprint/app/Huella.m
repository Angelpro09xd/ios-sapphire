/*
 * Huella: manage fingerprint unlock (fpd) -- enrolled fingers, enrollment, a test screen, the lock-screen
 * glyph's look, the log. Spanish UI. Installed with TrollStore (no sandbox, platform-application) so it
 * can reach fpd's control socket /var/run/fpd.sock (protocol: fpd.c) and write its prefs file
 * /var/mobile/Library/Preferences/com.sapphire.fingerprint.plist directly (not through cfprefsd: fpd and
 * the SpringBoard tweak read the file), announced with the darwin notification com.sapphire.fp.prefs.
 * URL scheme sapphirefp:// (the "Huella dactilar" row in Settings opens it).
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <notify.h>

#define SOCK_PATH   "/var/run/fpd.sock"
#define PREFS_PATH  @"/var/mobile/Library/Preferences/com.sapphire.fingerprint.plist"
#define MAX_FINGERS 5
#define MAX_FAILS   5
#define SENSOR_X    (540.0 / 1080.0)
#define SENSOR_Y    (2109.0 / 2400.0)
#define SENSOR_R    (105.0 / 1080.0)

/* ================================================================== fpd connection */

static int
fp_connect(int timeout_s)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	snprintf(a.sun_path, sizeof a.sun_path, "%s", SOCK_PATH);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		close(fd);
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	if (timeout_s > 0) {
		struct timeval tv = { timeout_s, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	}
	return fd;
}

static BOOL
fp_write(int fd, NSString *cmd)
{
	NSData *d = [[cmd stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
	const char *p = d.bytes;
	size_t n = d.length;
	while (n > 0) {
		ssize_t w = write(fd, p, n);
		if (w <= 0) {
			return NO;
		}
		p += w;
		n -= (size_t)w;
	}
	return YES;
}

/* one command, its answer (lines up to OK.../ERR.../END), on a background queue; done on the main queue */
static void
fp_request(NSString *cmd, void (^done)(NSArray<NSString *> *lines, NSString *error))
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		NSMutableArray *lines = [NSMutableArray array];
		NSString *err = nil;
		int fd = fp_connect(25);
		if (fd < 0) {
			err = @"El servicio de huella (fpd) no responde.";
		} else if (!fp_write(fd, cmd)) {
			err = @"No se pudo enviar la orden.";
		} else {
			NSMutableData *buf = [NSMutableData data];
			BOOL end = NO;
			while (!end) {
				char b[2048];
				ssize_t r = read(fd, b, sizeof b);
				if (r <= 0) {
					err = @"Sin respuesta del servicio.";
					break;
				}
				[buf appendBytes:b length:(NSUInteger)r];
				for (;;) {
					const char *s = buf.bytes;
					const char *nl = memchr(s, '\n', buf.length);
					if (nl == NULL) {
						break;
					}
					NSString *line = [[NSString alloc] initWithBytes:s length:(NSUInteger)(nl - s) encoding:NSUTF8StringEncoding];
					[buf replaceBytesInRange:NSMakeRange(0, (NSUInteger)(nl - s) + 1) withBytes:NULL length:0];
					if (line == nil) {
						continue;
					}
					[lines addObject:line];
					if ([line isEqualToString:@"END"] || [line hasPrefix:@"OK"] || [line hasPrefix:@"ERR"]) {
						end = YES;
						break;
					}
				}
			}
		}
		if (fd >= 0) {
			close(fd);
		}
		dispatch_async(dispatch_get_main_queue(), ^{ done(lines, err); });
	});
}

/* a long-lived connection (enrollment, test): every line to the handler on the main queue */
@interface FPStream : NSObject
@property (nonatomic, copy) void (^onLine)(NSString *line);
@property (nonatomic, copy) void (^onClose)(void);
- (BOOL)openWith:(NSString *)cmd;
- (void)send:(NSString *)cmd;
- (void)close;
@end

@implementation FPStream {
	int _fd;
	BOOL _closed;
}

- (BOOL)openWith:(NSString *)cmd
{
	_fd = fp_connect(0);
	if (_fd < 0) {
		return NO;
	}
	if (!fp_write(_fd, cmd)) {
		close(_fd);
		_fd = -1;
		return NO;
	}
	int fd = _fd;
	__weak FPStream *weak = self;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		NSMutableData *buf = [NSMutableData data];
		for (;;) {
			char b[1024];
			ssize_t r = read(fd, b, sizeof b);
			if (r <= 0) {
				break;
			}
			[buf appendBytes:b length:(NSUInteger)r];
			for (;;) {
				const char *s = buf.bytes;
				const char *nl = memchr(s, '\n', buf.length);
				if (nl == NULL) {
					break;
				}
				NSString *line = [[NSString alloc] initWithBytes:s length:(NSUInteger)(nl - s) encoding:NSUTF8StringEncoding];
				[buf replaceBytesInRange:NSMakeRange(0, (NSUInteger)(nl - s) + 1) withBytes:NULL length:0];
				if (line != nil) {
					dispatch_async(dispatch_get_main_queue(), ^{
						FPStream *st = weak;
						if (st != nil && st.onLine != nil) {
							st.onLine(line);
						}
					});
				}
			}
		}
		close(fd);
		dispatch_async(dispatch_get_main_queue(), ^{
			FPStream *st = weak;
			if (st != nil && st.onClose != nil) {
				st.onClose();
			}
		});
	});
	return YES;
}

- (void)send:(NSString *)cmd
{
	if (_fd >= 0 && !_closed) {
		fp_write(_fd, cmd);
	}
}

- (void)close
{
	if (_fd >= 0 && !_closed) {
		_closed = YES;
		shutdown(_fd, SHUT_RDWR);       /* the reader closes the descriptor */
	}
}

- (void)dealloc
{
	[self close];
}
@end

static NSDictionary *
parse_status(NSString *line)
{
	NSMutableDictionary *d = [NSMutableDictionary dictionary];
	for (NSString *kv in [line componentsSeparatedByString:@" "]) {
		NSRange r = [kv rangeOfString:@"="];
		if (r.location != NSNotFound) {
			d[[kv substringToIndex:r.location]] = [kv substringFromIndex:r.location + 1];
		}
	}
	return d;
}

/* ================================================================== prefs */

static NSMutableDictionary *
prefs(void)
{
	static NSMutableDictionary *p;
	if (p == nil) {
		NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:PREFS_PATH];
		p = [d isKindOfClass:[NSDictionary class]] ? [d mutableCopy] : [NSMutableDictionary dictionary];
	}
	return p;
}

static void
prefs_save(void)
{
	[prefs() writeToFile:PREFS_PATH atomically:YES];
	notify_post("com.sapphire.fp.prefs");
}

static BOOL
pref_bool(NSString *k, BOOL dflt)
{
	id v = prefs()[k];
	return [v respondsToSelector:@selector(boolValue)] ? [v boolValue] : dflt;
}

static double
pref_double(NSString *k, double dflt)
{
	id v = prefs()[k];
	return [v respondsToSelector:@selector(doubleValue)] ? [v doubleValue] : dflt;
}

static NSString *
pref_string(NSString *k, NSString *dflt)
{
	id v = prefs()[k];
	return [v isKindOfClass:[NSString class]] ? v : dflt;
}

static void
pref_set(NSString *k, id v)
{
	prefs()[k] = v;
	prefs_save();
}

static NSArray *
style_options(void)
{
	return @[ @[ @"touchid", @"Huella en un círculo" ], @[ @"symbol", @"Solo la huella" ], @[ @"ring", @"Solo un anillo" ],
		  @[ @"dot", @"Un punto" ] ];
}

static NSArray *
color_options(void)
{
	return @[ @[ @"white", @"Blanco" ], @[ @"blue", @"Azul" ], @[ @"red", @"Rojo" ], @[ @"green", @"Verde" ],
		  @[ @"purple", @"Morado" ], @[ @"orange", @"Naranja" ], @[ @"pink", @"Rosa" ], @[ @"teal", @"Turquesa" ] ];
}

static NSArray *
anim_options(void)
{
	return @[ @[ @"pulse", @"Latido" ], @[ @"ripple", @"Onda" ], @[ @"none", @"Ninguna" ] ];
}

static NSString *
option_label(NSArray *opts, NSString *key)
{
	for (NSArray *o in opts) {
		if ([o[0] isEqualToString:key]) {
			return o[1];
		}
	}
	return key;
}

static UIAlertController *
simple_alert(NSString *title, NSString *text)
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:text
	    preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
	return a;
}

static NSString *
fpd_error_text(NSString *line)
{
	if ([line hasPrefix:@"ERR locked"]) {
		return @"Desbloquea el iPhone primero.";
	}
	if ([line hasPrefix:@"ERR full"]) {
		return @"Ya hay 5 huellas registradas. Elimina una para añadir otra.";
	}
	if ([line hasPrefix:@"ERR busy"]) {
		return @"El sensor está ocupado. Inténtalo de nuevo en unos segundos.";
	}
	if ([line hasPrefix:@"ERR sensor"]) {
		return @"El sensor no está disponible (mira el registro).";
	}
	if ([line hasPrefix:@"ERR nofingers"]) {
		return @"No hay ninguna huella registrada.";
	}
	if ([line hasPrefix:@"ERR noslot"]) {
		return @"Esa huella ya no existe.";
	}
	return line;
}

/* ================================================================== the sensor marker */

/* a ring where the sensor is, in a view that covers the whole screen */
static CAShapeLayer *
sensor_ring(UIView *v)
{
	CGRect sb = [UIScreen mainScreen].bounds;
	CGPoint c = [v convertPoint:CGPointMake(SENSOR_X * sb.size.width, SENSOR_Y * sb.size.height)
	    fromCoordinateSpace:[UIScreen mainScreen].coordinateSpace];
	CGFloat r = SENSOR_R * sb.size.width;
	CAShapeLayer *l = [CAShapeLayer layer];
	l.path = [UIBezierPath bezierPathWithArcCenter:c radius:r startAngle:0 endAngle:2 * M_PI clockwise:YES].CGPath;
	l.fillColor = [[UIColor systemRedColor] colorWithAlphaComponent:0.08].CGColor;
	l.strokeColor = [UIColor systemRedColor].CGColor;
	l.lineWidth = 2;
	CABasicAnimation *a = [CABasicAnimation animationWithKeyPath:@"opacity"];
	a.fromValue = @1.0;
	a.toValue = @0.35;
	a.duration = 0.9;
	a.autoreverses = YES;
	a.repeatCount = HUGE_VALF;
	[l addAnimation:a forKey:@"blink"];
	return l;
}

/* ================================================================== fingerprint progress view */

@interface FPProgressView : UIView
@property (nonatomic) CGFloat progress;
- (void)setProgress:(CGFloat)p animated:(BOOL)animated;
- (void)shake;
- (void)setDone;
@end

@implementation FPProgressView {
	UIImageView *_base, *_fill;
	CALayer *_mask;
	CAShapeLayer *_ring, *_track;
}

- (instancetype)initWithFrame:(CGRect)f
{
	if ((self = [super initWithFrame:f])) {
		UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:f.size.width * 0.55
		    weight:UIImageSymbolWeightThin];
		UIImage *img = [UIImage systemImageNamed:@"touchid" withConfiguration:cfg];
		_base = [[UIImageView alloc] initWithImage:img];
		_base.tintColor = [UIColor systemGray3Color];
		_fill = [[UIImageView alloc] initWithImage:img];
		_fill.tintColor = [UIColor systemRedColor];
		for (UIImageView *iv in @[ _base, _fill ]) {
			iv.contentMode = UIViewContentModeCenter;
			iv.frame = self.bounds;
			[self addSubview:iv];
		}
		_mask = [CALayer layer];
		_mask.backgroundColor = [UIColor blackColor].CGColor;
		_mask.anchorPoint = CGPointMake(0.5, 1.0);
		_mask.frame = CGRectMake(0, f.size.height, f.size.width, 0);
		_fill.layer.mask = _mask;
		CGPoint c = CGPointMake(f.size.width / 2, f.size.height / 2);
		UIBezierPath *p = [UIBezierPath bezierPathWithArcCenter:c radius:f.size.width / 2 - 3 startAngle:-M_PI_2
		    endAngle:3 * M_PI_2 clockwise:YES];
		_track = [CAShapeLayer layer];
		_track.path = p.CGPath;
		_track.fillColor = [UIColor clearColor].CGColor;
		_track.strokeColor = [UIColor systemGray5Color].CGColor;
		_track.lineWidth = 4;
		[self.layer addSublayer:_track];
		_ring = [CAShapeLayer layer];
		_ring.path = p.CGPath;
		_ring.fillColor = [UIColor clearColor].CGColor;
		_ring.strokeColor = [UIColor systemRedColor].CGColor;
		_ring.lineWidth = 4;
		_ring.lineCap = kCALineCapRound;
		_ring.strokeEnd = 0;
		[self.layer addSublayer:_ring];
	}
	return self;
}

- (void)setProgress:(CGFloat)p animated:(BOOL)animated
{
	p = MAX(0, MIN(1, p));
	_progress = p;
	CGFloat h = self.bounds.size.height;
	[CATransaction begin];
	[CATransaction setAnimationDuration:animated ? 0.45 : 0];
	_mask.frame = CGRectMake(0, h * (1 - p), self.bounds.size.width, h * p);
	_ring.strokeEnd = p;
	[CATransaction commit];
}

- (void)shake
{
	CAKeyframeAnimation *a = [CAKeyframeAnimation animationWithKeyPath:@"transform.translation.x"];
	a.values = @[ @0, @(-10), @10, @(-7), @7, @(-3), @3, @0 ];
	a.duration = 0.45;
	[self.layer addAnimation:a forKey:@"shake"];
}

- (void)setDone
{
	[self setProgress:1 animated:YES];
	_ring.strokeColor = [UIColor systemGreenColor].CGColor;
	[UIView animateWithDuration:0.3 animations:^{ self->_fill.tintColor = [UIColor systemGreenColor]; }];
	CAKeyframeAnimation *a = [CAKeyframeAnimation animationWithKeyPath:@"transform.scale"];
	a.values = @[ @1.0, @1.12, @1.0 ];
	a.duration = 0.4;
	[self.layer addAnimation:a forKey:@"done"];
}
@end

/* ================================================================== enrollment */

@interface EnrollVC : UIViewController
@property (nonatomic, copy) void (^finished)(void);
@end

@implementation EnrollVC {
	FPStream *_s;
	FPProgressView *_fp;
	UILabel *_title, *_text, *_hint;
	int _total, _touches, _slot;
	BOOL _done, _failed;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor systemBackgroundColor];
	self.title = @"Añadir huella";
	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Cancelar"
	    style:UIBarButtonItemStylePlain target:self action:@selector(cancel)];
	_total = 15;
	_slot = -1;
	_title = [UILabel new];
	_title.font = [UIFont systemFontOfSize:28 weight:UIFontWeightBold];
	_title.textAlignment = NSTextAlignmentCenter;
	_title.text = @"Huella dactilar";
	_text = [UILabel new];
	_text.font = [UIFont systemFontOfSize:17];
	_text.numberOfLines = 0;
	_text.textAlignment = NSTextAlignmentCenter;
	_text.text = @"Conectando con el sensor…";
	_hint = [UILabel new];
	_hint.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
	_hint.textColor = [UIColor systemRedColor];
	_hint.numberOfLines = 0;
	_hint.textAlignment = NSTextAlignmentCenter;
	_fp = [[FPProgressView alloc] initWithFrame:CGRectMake(0, 0, 170, 170)];
	for (UIView *v in @[ _title, _text, _hint, _fp ]) {
		v.translatesAutoresizingMaskIntoConstraints = NO;
		[self.view addSubview:v];
	}
	UILayoutGuide *g = self.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[_title.topAnchor constraintEqualToAnchor:g.topAnchor constant:16],
		[_title.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
		[_title.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-20],
		[_text.topAnchor constraintEqualToAnchor:_title.bottomAnchor constant:10],
		[_text.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:28],
		[_text.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-28],
		[_fp.topAnchor constraintEqualToAnchor:_text.bottomAnchor constant:28],
		[_fp.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
		[_fp.widthAnchor constraintEqualToConstant:170],
		[_fp.heightAnchor constraintEqualToConstant:170],
		[_hint.topAnchor constraintEqualToAnchor:_fp.bottomAnchor constant:20],
		[_hint.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:28],
		[_hint.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-28],
	]];
}

- (void)viewDidAppear:(BOOL)animated
{
	[super viewDidAppear:animated];
	if (_s != nil) {
		return;
	}
	[self.view.layer addSublayer:sensor_ring(self.view)];
	__weak EnrollVC *weak = self;
	_s = [FPStream new];
	_s.onLine = ^(NSString *line) { [weak line:line]; };
	_s.onClose = ^{
		EnrollVC *me = weak;
		if (me != nil && !me->_done && !me->_failed) {
			[me fail:@"Se perdió la conexión con el servicio de huella."];
		}
	};
	if (![_s openWith:@"ENROLL_BEGIN"]) {
		[self fail:@"El servicio de huella (fpd) no responde."];
	}
}

- (void)setInstruction:(NSString *)t
{
	if (![_text.text isEqualToString:t]) {
		[UIView transitionWithView:_text duration:0.2 options:UIViewAnimationOptionTransitionCrossDissolve
		    animations:^{ self->_text.text = t; } completion:nil];
	}
}

- (void)line:(NSString *)line
{
	NSArray *w = [line componentsSeparatedByString:@" "];
	if ([line hasPrefix:@"OK"]) {
		[self setInstruction:@"Apoya el dedo sobre el círculo rojo de la pantalla y levántalo. Repítelo varias veces."];
		_hint.text = @"";
	} else if ([line hasPrefix:@"ERR"]) {
		[self fail:fpd_error_text(line)];
	} else if ([line isEqualToString:@"PRESS"]) {
		_hint.textColor = [UIColor secondaryLabelColor];
		_hint.text = @"Leyendo…";
	} else if ([line hasPrefix:@"PROGRESS"] && w.count >= 3) {
		int rem = [w[1] intValue], code = [w[2] intValue], first = w.count >= 4 ? [w[3] intValue] : -1;
		if (first > 0) {
			_total = MAX(first, 1);
		}
		if (rem >= 0) {
			_touches++;
			_total = MAX(_total, rem + 1);
			CGFloat p = 1.0 - (CGFloat)rem / (CGFloat)_total;
			[_fp setProgress:p animated:YES];
			_hint.text = @"";
			if (rem == 0) {
				[self setInstruction:@"Guardando la huella…"];
			} else if (p < 0.5) {
				[self setInstruction:_touches < 3 ? @"Apoya y levanta el dedo." : @"Mueve un poco el dedo entre un toque y otro."];
			} else {
				[self setInstruction:@"Ajusta los bordes: apoya ahora los bordes de la yema del dedo."];
			}
		} else {
			[_fp shake];
			_hint.textColor = [UIColor systemRedColor];
			_hint.text = [self errorText:code];
		}
	} else if ([line hasPrefix:@"DONE"]) {
		_done = YES;
		_slot = w.count >= 2 ? [w[1] intValue] : -1;
		[_fp setDone];
		_title.text = @"Completado";
		_hint.text = @"";
		[self setInstruction:@"La huella está lista para desbloquear el iPhone."];
		self.navigationItem.leftBarButtonItem = nil;
		self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Continuar"
		    style:UIBarButtonItemStyleDone target:self action:@selector(finish)];
		[_s close];
	} else if ([line hasPrefix:@"FAILED"]) {
		NSString *why = w.count >= 2 ? w[1] : @"";
		[self fail:[why isEqualToString:@"timeout"] ? @"Tiempo agotado: no se tocó el sensor durante un rato."
		          : [NSString stringWithFormat:@"No se pudo guardar la huella (%@).", why]];
	}
}

- (NSString *)errorText:(int)code
{
	switch (code) {
	case 1047: return @"Mueve el dedo un poco: esa zona ya está registrada.";
	case 1015: return @"Esta huella ya está registrada.";
	case 1013: return @"Apoya el dedo entero sobre el círculo.";
	case 1014: return @"Limpia la pantalla y el dedo.";
	case 1096: return @"Mantén el dedo apoyado un momento más.";
	case 1007: return @"Se alcanzó el límite de huellas.";
	default: return [NSString stringWithFormat:@"No se pudo leer (código %d). Vuelve a intentarlo.", code];
	}
}

- (void)fail:(NSString *)why
{
	if (_failed || _done) {
		return;
	}
	_failed = YES;
	[_s close];
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"No se pudo registrar la huella" message:why
	    preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		(void)x;
		[self dismissViewControllerAnimated:YES completion:self.finished];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

- (void)cancel
{
	_failed = YES;
	[_s send:@"ENROLL_CANCEL"];
	FPStream *s = _s;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [s close]; });
	[self dismissViewControllerAnimated:YES completion:self.finished];
}

- (void)finish
{
	int slot = _slot;
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Nombre de la huella"
	    message:@"Por ejemplo: «Pulgar derecho»." preferredStyle:UIAlertControllerStyleAlert];
	[a addTextFieldWithConfigurationHandler:^(UITextField *t) {
		t.placeholder = [NSString stringWithFormat:@"Huella %d", slot + 1];
		t.autocapitalizationType = UITextAutocapitalizationTypeSentences;
	}];
	void (^close)(void) = ^{ [self dismissViewControllerAnimated:YES completion:self.finished]; };
	[a addAction:[UIAlertAction actionWithTitle:@"Omitir" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) {
		(void)x;
		close();
	}]];
	[a addAction:[UIAlertAction actionWithTitle:@"Guardar" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		(void)x;
		NSString *n = [a.textFields.firstObject.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
		if (n.length > 0 && slot >= 0) {
			n = [[n componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]] componentsJoinedByString:@" "];
			fp_request([NSString stringWithFormat:@"RENAME %d %@", slot, n], ^(NSArray *l, NSString *e) {
				(void)l;
				(void)e;
				close();
			});
		} else {
			close();
		}
	}]];
	[self presentViewController:a animated:YES completion:nil];
}
@end

/* ================================================================== test */

@interface TestVC : UIViewController
@end

@implementation TestVC {
	FPStream *_s;
	UIImageView *_icon;
	UILabel *_text;
	UIButton *_again;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor systemBackgroundColor];
	self.title = @"Probar huella";
	UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:90 weight:UIImageSymbolWeightThin];
	_icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"touchid" withConfiguration:cfg]];
	_icon.tintColor = [UIColor systemGray2Color];
	_text = [UILabel new];
	_text.numberOfLines = 0;
	_text.textAlignment = NSTextAlignmentCenter;
	_text.font = [UIFont systemFontOfSize:19 weight:UIFontWeightMedium];
	_again = [UIButton buttonWithType:UIButtonTypeSystem];
	[_again setTitle:@"Probar otra vez" forState:UIControlStateNormal];
	_again.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
	[_again addTarget:self action:@selector(start) forControlEvents:UIControlEventTouchUpInside];
	UILabel *note = [UILabel new];
	note.numberOfLines = 0;
	note.textAlignment = NSTextAlignmentCenter;
	note.font = [UIFont systemFontOfSize:13];
	note.textColor = [UIColor secondaryLabelColor];
	note.text = @"Comprueba si el sensor reconoce tu dedo. No desbloquea nada y no cuenta como intento fallido.";
	for (UIView *v in @[ _icon, _text, _again, note ]) {
		v.translatesAutoresizingMaskIntoConstraints = NO;
		[self.view addSubview:v];
	}
	UILayoutGuide *g = self.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[_icon.topAnchor constraintEqualToAnchor:g.topAnchor constant:40],
		[_icon.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
		[_text.topAnchor constraintEqualToAnchor:_icon.bottomAnchor constant:24],
		[_text.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:24],
		[_text.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-24],
		[_again.topAnchor constraintEqualToAnchor:_text.bottomAnchor constant:20],
		[_again.centerXAnchor constraintEqualToAnchor:g.centerXAnchor],
		[note.topAnchor constraintEqualToAnchor:_again.bottomAnchor constant:20],
		[note.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:30],
		[note.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-30],
	]];
}

- (void)viewDidAppear:(BOOL)animated
{
	[super viewDidAppear:animated];
	static NSString *const key = @"ring";
	if ([self.view.layer valueForKey:key] == nil) {
		CAShapeLayer *r = sensor_ring(self.view);
		[self.view.layer addSublayer:r];
		[self.view.layer setValue:r forKey:key];
	}
	if (_s == nil) {
		[self start];
	}
}

- (void)viewWillDisappear:(BOOL)animated
{
	[super viewWillDisappear:animated];
	[_s send:@"TEST_CANCEL"];
	[_s close];
	_s = nil;
}

- (void)result:(NSString *)t color:(UIColor *)c
{
	_text.text = t;
	_icon.tintColor = c;
	_again.hidden = NO;
	[_s close];
	_s = nil;
}

- (void)start
{
	[_s close];
	_again.hidden = YES;
	_icon.tintColor = [UIColor systemGray2Color];
	_text.text = @"Apoya el dedo sobre el círculo rojo.";
	__weak TestVC *weak = self;
	FPStream *s = [FPStream new];
	_s = s;
	s.onLine = ^(NSString *line) {
		TestVC *me = weak;
		if (me == nil || me->_s != s) {
			return;
		}
		NSArray *w = [line componentsSeparatedByString:@" "];
		if ([line isEqualToString:@"PRESS"]) {
			me->_text.text = @"Comprobando…";
		} else if ([line hasPrefix:@"TEST MATCH"]) {
			NSString *name = w.count >= 5 ? [[w subarrayWithRange:NSMakeRange(4, w.count - 4)] componentsJoinedByString:@" "] : @"";
			[me result:name.length ? [NSString stringWithFormat:@"Reconocida: %@", name] : @"Huella reconocida"
			     color:[UIColor systemGreenColor]];
		} else if ([line hasPrefix:@"TEST NOMATCH"]) {
			[me result:@"No reconocida" color:[UIColor systemRedColor]];
		} else if ([line hasPrefix:@"TEST LIFTED"]) {
			[me result:@"Has levantado el dedo demasiado pronto." color:[UIColor systemOrangeColor]];
		} else if ([line hasPrefix:@"TEST TIMEOUT"]) {
			[me result:@"Tiempo agotado." color:[UIColor systemGray2Color]];
		} else if ([line hasPrefix:@"TEST ERR"]) {
			[me result:[NSString stringWithFormat:@"Error del sensor (%@).", w.count >= 3 ? w[2] : @"?"] color:[UIColor systemOrangeColor]];
		} else if ([line hasPrefix:@"ERR"]) {
			[me result:fpd_error_text(line) color:[UIColor systemGray2Color]];
		}
	};
	if (![s openWith:@"TEST"]) {
		[self result:@"El servicio de huella (fpd) no responde." color:[UIColor systemGray2Color]];
	}
}
@end

/* ================================================================== log */

@interface LogVC : UIViewController
@end

@implementation LogVC {
	UITextView *_tv;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Registro";
	self.view.backgroundColor = [UIColor systemBackgroundColor];
	_tv = [[UITextView alloc] initWithFrame:self.view.bounds];
	_tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	_tv.editable = NO;
	_tv.font = [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightRegular];
	[self.view addSubview:_tv];
	self.navigationItem.rightBarButtonItems = @[
		[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(reload)],
		[[UIBarButtonItem alloc] initWithTitle:@"Copiar" style:UIBarButtonItemStylePlain target:self action:@selector(copyAll)],
	];
	[self reload];
}

- (void)reload
{
	_tv.text = @"Cargando…";
	fp_request(@"LOG 250", ^(NSArray<NSString *> *lines, NSString *err) {
		NSMutableString *s = [NSMutableString string];
		for (NSString *l in lines) {
			if ([l hasPrefix:@"L "]) {
				[s appendFormat:@"%@\n", [l substringFromIndex:2]];
			}
		}
		self->_tv.text = err ?: (s.length ? s : @"(vacío)");
		[self->_tv scrollRangeToVisible:NSMakeRange(self->_tv.text.length, 0)];
	});
}

- (void)copyAll
{
	[UIPasteboard generalPasteboard].string = _tv.text;
}
@end

/* ================================================================== option picker */

@interface PickVC : UITableViewController
@property (nonatomic, copy) NSArray *options;
@property (nonatomic, copy) NSString *key, *dflt;
@end

@implementation PickVC
- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s
{
	(void)tv;
	(void)s;
	return (NSInteger)self.options.count;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	(void)tv;
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
	NSArray *o = self.options[(NSUInteger)ip.row];
	c.textLabel.text = o[1];
	c.accessoryType = [o[0] isEqualToString:pref_string(self.key, self.dflt)] ? UITableViewCellAccessoryCheckmark
	                                                                          : UITableViewCellAccessoryNone;
	return c;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	pref_set(self.key, self.options[(NSUInteger)ip.row][0]);
	[tv reloadData];
	[self.navigationController popViewControllerAnimated:YES];
}
@end

/* ================================================================== main list */

enum { S_FINGERS, S_USE, S_LOOK, S_OFF, S_VIB, S_SENSORS, S_STATUS, S_COUNT };

@interface MainVC : UITableViewController
@end

@implementation MainVC {
	NSArray<NSArray *> *_fingers;   /* @[slot, fid, name] */
	NSDictionary *_status;
	NSString *_error;
}

- (instancetype)init
{
	return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Huella dactilar";
	self.navigationController.navigationBar.prefersLargeTitles = YES;
	self.refreshControl = [UIRefreshControl new];
	[self.refreshControl addTarget:self action:@selector(refresh) forControlEvents:UIControlEventValueChanged];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	[self refresh];
}

- (void)refresh
{
	fp_request(@"LIST", ^(NSArray<NSString *> *lines, NSString *err) {
		NSMutableArray *f = [NSMutableArray array];
		for (NSString *l in lines) {
			NSArray *w = [l componentsSeparatedByString:@" "];
			if ([l hasPrefix:@"FINGER "] && w.count >= 5) {
				NSString *name = [[w subarrayWithRange:NSMakeRange(4, w.count - 4)] componentsJoinedByString:@" "];
				[f addObject:@[ @([w[1] intValue]), w[2], name ]];
			}
		}
		self->_fingers = f;
		self->_error = err;
		fp_request(@"STATUS", ^(NSArray<NSString *> *sl, NSString *e2) {
			self->_status = e2 == nil && sl.count ? parse_status(sl.lastObject) : nil;
			[self.refreshControl endRefreshing];
			[self.tableView reloadData];
		});
	});
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv
{
	(void)tv;
	return S_COUNT;
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s
{
	(void)tv;
	switch (s) {
	case S_FINGERS: return (NSInteger)_fingers.count + 1 + (_fingers.count > 0 ? 1 : 0);
	case S_USE: return 2;
	case S_LOOK: return 7;
	case S_OFF: return 2;
	case S_VIB: return 5;
	case S_SENSORS: return 1;
	case S_STATUS: return 6;
	}
	return 0;
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)s
{
	(void)tv;
	NSArray *t = @[ @"Huellas", @"Usar huella para", @"Icono en la pantalla bloqueada", @"Pantalla apagada", @"Vibración",
		        @"Sensores", @"Estado" ];
	return t[(NSUInteger)s];
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)s
{
	(void)tv;
	switch (s) {
	case S_FINGERS:
		return _error ?: @"Hasta 5 huellas. Para registrar o eliminar huellas el iPhone debe estar desbloqueado.";
	case S_USE:
		return @"Tras reiniciar, la primera vez hay que usar el código. Después de 5 intentos fallidos se pide el código.";
	case S_OFF:
		return @"Experimental: funciona si el panel táctil sigue informando con la pantalla apagada (se ve en el "
		       @"Registro). El indicador con la pantalla apagada aún no tiene efecto: el panel está apagado.";
	case S_VIB:
		return @"El motor de este iPhone da pulsos cortos: la respuesta háptica se imita con toques breves. "
		       @"Las llamadas y notificaciones siguen los ajustes de vibrar en modo sonido / silencio.";
	case S_SENSORS:
		return @"Requiere los sensores de movimiento, que aún no funcionan en este iPhone.";
	}
	return nil;
}

- (UITableViewCell *)switchCell:(NSString *)title key:(NSString *)key dflt:(BOOL)dflt enabled:(BOOL)enabled
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
	c.textLabel.text = title;
	c.textLabel.numberOfLines = 0;
	c.selectionStyle = UITableViewCellSelectionStyleNone;
	UISwitch *sw = [UISwitch new];
	sw.on = pref_bool(key, dflt);
	sw.enabled = enabled;
	c.textLabel.enabled = enabled;
	sw.accessibilityIdentifier = key;
	[sw addTarget:self action:@selector(switched:) forControlEvents:UIControlEventValueChanged];
	c.accessoryView = sw;
	return c;
}

- (void)switched:(UISwitch *)sw
{
	if ([sw.accessibilityIdentifier isEqualToString:@"fadeIn"]) {
		prefs()[@"successAnimation"] = @(sw.on);       /* one switch for both of the tweak's animations */
	}
	pref_set(sw.accessibilityIdentifier, @(sw.on));
}

- (UITableViewCell *)sliderCell:(NSString *)title key:(NSString *)key min:(float)mn max:(float)mx dflt:(float)d
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
	c.selectionStyle = UITableViewCellSelectionStyleNone;
	UILabel *l = [UILabel new];
	l.text = title;
	UISlider *s = [UISlider new];
	s.minimumValue = mn;
	s.maximumValue = mx;
	double v = pref_double(key, d);
	s.value = (float)(v > 0 ? v : d);
	s.accessibilityIdentifier = key;
	[s addTarget:self action:@selector(slid:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside];
	for (UIView *v2 in @[ l, s ]) {
		v2.translatesAutoresizingMaskIntoConstraints = NO;
		[c.contentView addSubview:v2];
	}
	UILayoutGuide *m = c.contentView.layoutMarginsGuide;
	[NSLayoutConstraint activateConstraints:@[
		[l.leadingAnchor constraintEqualToAnchor:m.leadingAnchor],
		[l.centerYAnchor constraintEqualToAnchor:c.contentView.centerYAnchor],
		[l.widthAnchor constraintEqualToConstant:90],
		[s.leadingAnchor constraintEqualToAnchor:l.trailingAnchor constant:8],
		[s.trailingAnchor constraintEqualToAnchor:m.trailingAnchor],
		[s.centerYAnchor constraintEqualToAnchor:c.contentView.centerYAnchor],
		[c.contentView.heightAnchor constraintGreaterThanOrEqualToConstant:48],
	]];
	return c;
}

- (void)slid:(UISlider *)s
{
	double v = s.value;
	if ([s.accessibilityIdentifier isEqualToString:@"glyphSize"]) {
		v = round(v);
	} else {
		v = round(v * 100) / 100;
	}
	pref_set(s.accessibilityIdentifier, @(v));
}

- (UITableViewCell *)valueCell:(NSString *)title value:(NSString *)value link:(BOOL)link
{
	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
	c.textLabel.text = title;
	c.detailTextLabel.text = value;
	c.accessoryType = link ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
	c.selectionStyle = link ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
	return c;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	(void)tv;
	NSInteger r = ip.row;
	switch (ip.section) {
	case S_FINGERS: {
		if (r < (NSInteger)_fingers.count) {
			NSArray *f = _fingers[(NSUInteger)r];
			UITableViewCell *c = [self valueCell:f[2] value:@"" link:YES];
			c.imageView.image = [UIImage systemImageNamed:@"touchid"];
			c.imageView.tintColor = [UIColor systemRedColor];
			return c;
		}
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		if (r == (NSInteger)_fingers.count) {
			BOOL can = _fingers.count < MAX_FINGERS && _error == nil;
			c.textLabel.text = can ? @"Añadir una huella…" : @"Añadir una huella (máximo 5)";
			c.textLabel.textColor = can ? self.view.tintColor : [UIColor secondaryLabelColor];
		} else {
			c.textLabel.text = @"Eliminar todas las huellas";
			c.textLabel.textColor = [UIColor systemRedColor];
		}
		return c;
	}
	case S_USE:
		if (r == 0) {
			return [self switchCell:@"Desbloquear el iPhone" key:@"unlockEnabled" dflt:YES enabled:YES];
		}
		return [self switchCell:@"Apps y compras (próximamente)" key:@"appsEnabled" dflt:NO enabled:NO];
	case S_LOOK:
		switch (r) {
		case 0: return [self valueCell:@"Estilo" value:option_label(style_options(), pref_string(@"glyphStyle", @"touchid")) link:YES];
		case 1: return [self valueCell:@"Color" value:option_label(color_options(), pref_string(@"glyphColor", @"white")) link:YES];
		case 2: return [self sliderCell:@"Tamaño" key:@"glyphSize" min:50 max:110 dflt:73];
		case 3: return [self sliderCell:@"Opacidad" key:@"glyphOpacity" min:0.2f max:1.0f dflt:0.9f];
		case 4: return [self valueCell:@"Al tocar" value:option_label(anim_options(), pref_string(@"pressAnimation", @"pulse")) link:YES];
		case 5: return [self switchCell:@"Mostrar también sobre el teclado del código" key:@"showOnKeypad" dflt:YES enabled:YES];
		default: return [self switchCell:@"Animaciones al aparecer y al desbloquear" key:@"fadeIn" dflt:YES enabled:YES];
		}
	case S_OFF:
		if (r == 0) {
			return [self switchCell:@"Desbloquear tocando con la pantalla apagada" key:@"screenOffWake" dflt:NO enabled:YES];
		}
		return [self switchCell:@"Indicador con la pantalla apagada" key:@"alwaysOnIndicator" dflt:NO enabled:YES];
	case S_VIB:
		switch (r) {
		case 0: return [self switchCell:@"Vibrar al reconocer o fallar la huella" key:@"haptics" dflt:YES enabled:YES];
		case 1: return [self switchCell:@"Respuesta háptica del sistema" key:@"uiHaptics" dflt:YES enabled:YES];
		case 2: return [self switchCell:@"Vibración del teclado" key:@"keyboardHaptics" dflt:NO enabled:YES];
		case 3: return [self switchCell:@"Vibrar con llamadas y notificaciones" key:@"alertVibration" dflt:YES enabled:YES];
		default: return [self valueCell:@"Probar vibración" value:@"" link:YES];
		}
	case S_SENSORS:
		return [self switchCell:@"Mostrar solo al mover el iPhone (requiere sensores)" key:@"raiseToShow" dflt:NO enabled:NO];
	case S_STATUS: {
		NSDictionary *st = _status;
		switch (r) {
		case 0:
			return [self valueCell:@"Servicio" value:st == nil ? @"No responde" : [st[@"sensor"] isEqualToString:@"1"] ? @"Activo"
			                                                                    : @"Sensor no disponible" link:NO];
		case 1: {
			NSString *v;
			if (st == nil) {
				v = @"—";
			} else if ([st[@"lockedout"] isEqualToString:@"1"]) {
				v = @"Bloqueado: usa el código";
			} else {
				v = [NSString stringWithFormat:@"%@ de %d", st[@"fails"] ?: @"0", MAX_FAILS];
			}
			return [self valueCell:@"Intentos fallidos" value:v link:NO];
		}
		case 2:
			return [self valueCell:@"Desbloqueos con huella" value:st ? [NSString stringWithFormat:@"%@ (fallos %@)",
			    st[@"unlocks"] ?: @"0", st[@"nomatch"] ?: @"0"] : @"—" link:NO];
		case 3:
			return [self valueCell:@"Toques con la pantalla apagada" value:st[@"offpresses"] ?: @"—" link:NO];
		case 4:
			return [self valueCell:@"Probar huella" value:@"" link:YES];
		default:
			return [self valueCell:@"Registro" value:@"" link:YES];
		}
	}
	}
	return [UITableViewCell new];
}

- (BOOL)tableView:(UITableView *)tv canEditRowAtIndexPath:(NSIndexPath *)ip
{
	(void)tv;
	return ip.section == S_FINGERS && ip.row < (NSInteger)_fingers.count;
}

- (void)tableView:(UITableView *)tv commitEditingStyle:(UITableViewCellEditingStyle)style forRowAtIndexPath:(NSIndexPath *)ip
{
	(void)tv;
	if (style == UITableViewCellEditingStyleDelete && ip.row < (NSInteger)_fingers.count) {
		[self confirmDelete:_fingers[(NSUInteger)ip.row]];
	}
}

- (void)run:(NSString *)cmd
{
	fp_request(cmd, ^(NSArray<NSString *> *lines, NSString *err) {
		NSString *last = lines.lastObject;
		if (err != nil || ![last hasPrefix:@"OK"]) {
			[self presentViewController:simple_alert(@"No se pudo completar", err ?: fpd_error_text(last ?: @"")) animated:YES
			    completion:nil];
		}
		[self refresh];
	});
}

- (void)confirmDelete:(NSArray *)f
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"¿Eliminar «%@»?", f[2]]
	    message:nil preferredStyle:UIAlertControllerStyleActionSheet];
	[a addAction:[UIAlertAction actionWithTitle:@"Eliminar huella" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
		(void)x;
		[self run:[NSString stringWithFormat:@"DELETE %@", f[0]]];
	}]];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:nil]];
	[self presentViewController:a animated:YES completion:nil];
}

- (void)fingerActions:(NSArray *)f
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:f[2] message:nil
	    preferredStyle:UIAlertControllerStyleActionSheet];
	[a addAction:[UIAlertAction actionWithTitle:@"Cambiar nombre" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		(void)x;
		UIAlertController *r = [UIAlertController alertControllerWithTitle:@"Nombre de la huella" message:nil
		    preferredStyle:UIAlertControllerStyleAlert];
		[r addTextFieldWithConfigurationHandler:^(UITextField *t) {
			t.text = f[2];
			t.clearButtonMode = UITextFieldViewModeWhileEditing;
		}];
		[r addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:nil]];
		[r addAction:[UIAlertAction actionWithTitle:@"Guardar" style:UIAlertActionStyleDefault handler:^(UIAlertAction *y) {
			(void)y;
			NSString *n = [r.textFields.firstObject.text
			    stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
			n = [[n componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]] componentsJoinedByString:@" "];
			if (n.length > 0) {
				[self run:[NSString stringWithFormat:@"RENAME %@ %@", f[0], n]];
			}
		}]];
		[self presentViewController:r animated:YES completion:nil];
	}]];
	[a addAction:[UIAlertAction actionWithTitle:@"Eliminar huella" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
		(void)x;
		[self confirmDelete:f];
	}]];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:nil]];
	[self presentViewController:a animated:YES completion:nil];
}

- (void)pick:(NSString *)title options:(NSArray *)o key:(NSString *)k dflt:(NSString *)d
{
	PickVC *p = [[PickVC alloc] initWithStyle:UITableViewStyleInsetGrouped];
	p.title = title;
	p.options = o;
	p.key = k;
	p.dflt = d;
	[self.navigationController pushViewController:p animated:YES];
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	NSInteger r = ip.row;
	if (ip.section == S_FINGERS) {
		if (r < (NSInteger)_fingers.count) {
			[self fingerActions:_fingers[(NSUInteger)r]];
		} else if (r == (NSInteger)_fingers.count) {
			if (_fingers.count >= MAX_FINGERS || _error != nil) {
				return;
			}
			EnrollVC *e = [EnrollVC new];
			__weak MainVC *weak = self;
			e.finished = ^{ [weak refresh]; };
			UINavigationController *n = [[UINavigationController alloc] initWithRootViewController:e];
			n.modalPresentationStyle = UIModalPresentationFullScreen;
			[self presentViewController:n animated:YES completion:nil];
		} else {
			UIAlertController *a = [UIAlertController alertControllerWithTitle:@"¿Eliminar todas las huellas?"
			    message:@"El iPhone solo se podrá desbloquear con el código." preferredStyle:UIAlertControllerStyleActionSheet];
			[a addAction:[UIAlertAction actionWithTitle:@"Eliminar todas" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
				(void)x;
				[self run:@"DELETE_ALL"];
			}]];
			[a addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:nil]];
			[self presentViewController:a animated:YES completion:nil];
		}
	} else if (ip.section == S_LOOK) {
		if (r == 0) {
			[self pick:@"Estilo" options:style_options() key:@"glyphStyle" dflt:@"touchid"];
		} else if (r == 1) {
			[self pick:@"Color" options:color_options() key:@"glyphColor" dflt:@"white"];
		} else if (r == 4) {
			[self pick:@"Al tocar" options:anim_options() key:@"pressAnimation" dflt:@"pulse"];
		}
	} else if (ip.section == S_VIB && r == 4) {
		UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Probar vibración" message:nil
		    preferredStyle:UIAlertControllerStyleActionSheet];
		NSArray *opts = @[ @[ @"selection", @"Selección" ], @[ @"light", @"Toque suave" ], @[ @"medium", @"Toque medio" ],
				   @[ @"heavy", @"Toque fuerte" ], @[ @"success", @"Éxito" ], @[ @"warning", @"Aviso" ],
				   @[ @"error", @"Error" ], @[ @"notify", @"Notificación" ], @[ @"long", @"Larga" ] ];
		for (NSArray *o in opts) {
			NSString *name = [@"com.sapphire.vib." stringByAppendingString:o[0]];
			[a addAction:[UIAlertAction actionWithTitle:o[1] style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
				(void)x;
				notify_post(name.UTF8String);
			}]];
		}
		[a addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:nil]];
		[self presentViewController:a animated:YES completion:nil];
	} else if (ip.section == S_STATUS) {
		if (r == 4) {
			[self.navigationController pushViewController:[TestVC new] animated:YES];
		} else if (r == 5) {
			[self.navigationController pushViewController:[LogVC new] animated:YES];
		}
	}
}
@end

/* ================================================================== app */

@interface HuellaAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation HuellaAppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts
{
	(void)app;
	(void)opts;
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:[MainVC new]];
	[self.window makeKeyAndVisible];
	return YES;
}

- (BOOL)application:(UIApplication *)app openURL:(NSURL *)url options:(NSDictionary *)opts
{
	(void)app;
	(void)opts;
	return [url.scheme isEqualToString:@"sapphirefp"];
}
@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([HuellaAppDelegate class]));
	}
}
