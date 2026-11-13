/*
 * Android: iOS's side of the dual boot.
 *   "Restart into Android": the A/B slot Android lives in made active (xnu 0305, kern.sweet_boot_android = 1:
 *   the GPT switch Qualcomm's bootloader does itself) and the phone restarted into Android, which then stays
 *   the system booted until iOS is chosen again from Android's Sapphire Dual Boot app.
 *   "Update Sapphire": the same slot switch, but the restart goes to that slot's recovery (OrangeFox; xnu 0311,
 *   kern.sweet_boot_android = 3), where the PC installer's update mode takes over (and makes iOS's slot
 *   active again when it is done).
 * Spanish or English, as the phone's language.
 */
#import <UIKit/UIKit.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <string.h>

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@interface BootController : UIViewController
@property (strong, nonatomic) UILabel *status;
@end

static BOOL
spanish(void)
{
	NSString *l = NSLocale.preferredLanguages.firstObject ?: @"en";
	return [l hasPrefix:@"es"];
}

static NSString *
T(NSString *es, NSString *en)
{
	return spanish() ? es : en;
}

static int
active_slot(void)
{
	int v = 0;
	size_t len = sizeof(v);
	if (sysctlbyname("kern.sweet_boot_android", &v, &len, NULL, 0) != 0) {
		return -1;
	}
	return v;
}

static NSString *
sapphire_version(void)
{
	NSString *v = [NSString stringWithContentsOfFile:@"/redwood/version" encoding:NSUTF8StringEncoding error:nil];
	v = [v stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
	return v.length ? v : @"0.1";
}

@implementation BootController

- (UIButton *)button:(NSString *)title color:(UIColor *)c action:(SEL)sel
{
	UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
	[b setTitle:title forState:UIControlStateNormal];
	b.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightSemibold];
	b.backgroundColor = c;
	[b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
	b.layer.cornerRadius = 28;
	b.translatesAutoresizingMaskIntoConstraints = NO;
	[b addTarget:self action:sel forControlEvents:UIControlEventTouchUpInside];
	[self.view addSubview:b];
	return b;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.systemBackgroundColor;

	UILabel *title = [UILabel new];
	title.text = @"Dual boot";
	title.font = [UIFont systemFontOfSize:34 weight:UIFontWeightBold];
	title.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:title];

	self.status = [UILabel new];
	self.status.numberOfLines = 0;
	self.status.textColor = UIColor.secondaryLabelColor;
	self.status.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:self.status];

	UIButton *go = [self button:T(@"Reiniciar en Android", @"Restart into Android")
	    color:[UIColor colorWithRed:0.20 green:0.66 blue:0.33 alpha:1] action:@selector(ask)];
	UIButton *upd = [self button:T(@"Actualizar Sapphire", @"Update Sapphire")
	    color:[UIColor colorWithRed:0.06 green:0.32 blue:0.73 alpha:1] action:@selector(askUpdate)];

	UILabel *note = [UILabel new];
	note.numberOfLines = 0;
	note.font = [UIFont systemFontOfSize:14];
	note.textColor = UIColor.secondaryLabelColor;
	note.text = [NSString stringWithFormat:T(@"Sapphire %@. Para actualizar: pulsa «Actualizar Sapphire», conecta el teléfono al PC y abre el instalador de Sapphire en modo «Actualizar». iOS, tus apps y tus datos se conservan.",
	    @"Sapphire %@. To update: tap \"Update Sapphire\", connect the phone to the PC and open the Sapphire installer in \"Update\" mode. iOS, your apps and your data are kept."), sapphire_version()];
	note.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:note];

	UILayoutGuide *g = self.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[title.topAnchor constraintEqualToAnchor:g.topAnchor constant:32],
		[title.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
		[self.status.topAnchor constraintEqualToAnchor:title.bottomAnchor constant:16],
		[self.status.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
		[self.status.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-20],
		[go.topAnchor constraintEqualToAnchor:self.status.bottomAnchor constant:32],
		[go.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
		[go.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-20],
		[go.heightAnchor constraintEqualToConstant:56],
		[upd.topAnchor constraintEqualToAnchor:go.bottomAnchor constant:14],
		[upd.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
		[upd.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-20],
		[upd.heightAnchor constraintEqualToConstant:56],
		[note.topAnchor constraintEqualToAnchor:upd.bottomAnchor constant:16],
		[note.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
		[note.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-20],
	]];
	[self refresh];
}

- (void)refresh
{
	int s = active_slot();
	if (s == 'a' || s == 'b') {
		self.status.text = [NSString stringWithFormat:T(@"iOS arranca desde la ranura %c. Android está en la ranura %c.\n\n"
		    "Al reiniciar en Android, el teléfono se queda en Android hasta que vuelvas a iOS desde la app Sapphire Dual Boot.",
		    @"iOS starts from slot %c. Android is in slot %c.\n\n"
		    "After restarting into Android, the phone stays in Android until you go back to iOS from the Sapphire Dual Boot app."),
		    (char)s, s == 'a' ? 'b' : 'a'];
	} else {
		self.status.text = T(@"No se pudo leer la ranura activa: este kernel no tiene el cambio de ranura (xnu 0305).",
		    @"The active slot could not be read: this kernel has no slot switch (xnu 0305).");
	}
}

- (void)confirm:(NSString *)title message:(NSString *)msg value:(int)v
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:msg
	    preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:T(@"Cancelar", @"Cancel") style:UIAlertActionStyleCancel handler:nil]];
	[a addAction:[UIAlertAction actionWithTitle:T(@"Reiniciar", @"Restart") style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
		(void)x;
		[self go:v];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

- (void)ask
{
	[self confirm:T(@"Reiniciar en Android", @"Restart into Android")
	    message:T(@"El teléfono se reiniciará en Android y seguirá en Android en cada reinicio. ¿Seguir?",
	        @"The phone restarts into Android and stays in Android at every restart. Continue?") value:1];
}

- (void)askUpdate
{
	[self confirm:T(@"Actualizar Sapphire", @"Update Sapphire")
	    message:T(@"El teléfono se reiniciará en OrangeFox. Conéctalo al PC y abre el instalador de Sapphire en modo «Actualizar»: al terminar vuelve solo a iOS. ¿Seguir?",
	        @"The phone restarts into OrangeFox. Connect it to the PC and open the Sapphire installer in \"Update\" mode: when it is done it goes back to iOS by itself. Continue?") value:3];
}

- (void)go:(int)v
{
	self.status.text = T(@"Cambiando de ranura…", @"Switching slots…");
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		int val = v;
		int rc = sysctlbyname("kern.sweet_boot_android", NULL, NULL, &val, sizeof(val));
		int e = errno;
		dispatch_async(dispatch_get_main_queue(), ^{
			/* on success the kernel restarts the phone and this is never seen */
			self.status.text = rc == 0 ? T(@"Reiniciando…", @"Restarting…") :
			    (v == 3 && e == EINVAL ? T(@"Este kernel aún no sabe ir a OrangeFox (hace falta el kernel 0311 o posterior). No se ha modificado nada.",
			        @"This kernel cannot go to OrangeFox yet (kernel 0311 or later is needed). Nothing was changed.") :
			    [NSString stringWithFormat:T(@"No se pudo cambiar de ranura: %s. No se ha modificado nada.",
			        @"The slot could not be switched: %s. Nothing was changed."), strerror(e)]);
		});
	});
}

@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	(void)application, (void)options;
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = [BootController new];
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
	}
}
