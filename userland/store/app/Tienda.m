// Tienda: an app store for this phone. It reads AltStore-format sources (JSON lists of IPAs), shows their apps,
// downloads the IPA and hands it to TrollStore's trollstorehelper (spawned as root, the way TrollStore does it),
// so apps install, update and uninstall with one tap, without an Apple ID.
#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <spawn.h>
#include <sys/wait.h>
#include <glob.h>

#define POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE 1
extern int posix_spawnattr_set_persona_np(const posix_spawnattr_t *, uid_t, uint32_t);
extern int posix_spawnattr_set_persona_uid_np(const posix_spawnattr_t *, uid_t);
extern int posix_spawnattr_set_persona_gid_np(const posix_spawnattr_t *, uid_t);

@interface LSApplicationProxy : NSObject
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
@property (nonatomic, readonly) NSString *shortVersionString;
@property (nonatomic, readonly) NSString *bundleVersion;
@property (nonatomic, readonly) NSURL *bundleURL;
@property (nonatomic, readonly, getter=isInstalled) BOOL installed;
@end
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)openApplicationWithBundleID:(NSString *)bundleID;
@end

static NSString *const kSourcesKey = @"sources";
static NSArray<NSString *> *defaultSources(void)
{
	return @[@"https://community-apps.sidestore.io/sidecommunity.json", @"https://alt.getutm.app",
		 @"https://provenance-emu.com/apps.json", @"https://apps.altstore.io"];
}

#pragma mark - model

@interface StoreApp : NSObject
@property (copy) NSString *name, *bundleID, *developer, *subtitle, *desc, *version, *source, *minOS;
@property (strong) NSURL *iconURL, *downloadURL;
@property long long size;
@end
@implementation StoreApp
@end

typedef NS_ENUM(int, JobState) { JobDownloading, JobInstalling };
@interface Job : NSObject
@property (strong) NSURLSessionDownloadTask *task;
@property JobState state;
@end
@implementation Job
@end

static NSString *str(id v) { return [v isKindOfClass:NSString.class] ? v : nil; }
static NSURL *url(id v) { NSString *s = str(v); return s.length ? [NSURL URLWithString:s] : nil; }

static NSString *installedVersion(NSString *bid)
{
	Class c = NSClassFromString(@"LSApplicationProxy");
	LSApplicationProxy *p = [c applicationProxyForIdentifier:bid];
	if (!p || !p.installed || !p.bundleURL) return nil;
	return p.shortVersionString ?: p.bundleVersion ?: @"?";
}

static BOOL newer(NSString *a, NSString *b)   // a > b
{
	return a && b && [a compare:b options:NSNumericSearch] == NSOrderedDescending;
}

static NSString *helperPath(void)
{
	Class c = NSClassFromString(@"LSApplicationProxy");
	LSApplicationProxy *p = [c applicationProxyForIdentifier:@"com.opa334.TrollStore"];
	NSString *h = p.bundleURL ? [p.bundleURL.path stringByAppendingPathComponent:@"trollstorehelper"] : nil;
	if (h && [NSFileManager.defaultManager isExecutableFileAtPath:h]) return h;
	glob_t g;
	NSString *r = nil;
	if (glob("/var/containers/Bundle/Application/*/TrollStore.app/trollstorehelper", 0, NULL, &g) == 0 && g.gl_pathc)
		r = [NSString stringWithUTF8String:g.gl_pathv[0]];
	globfree(&g);
	return r;
}

// trollstorehelper ARGS as root; returns the exit status, *out gets its output
static int runHelper(NSArray<NSString *> *args, NSString **out)
{
	NSString *h = helperPath();
	if (!h) { if (out) *out = @"TrollStore no está instalado"; return -1; }
	int fds[2];
	pipe(fds);
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	posix_spawnattr_set_persona_np(&at, 99, POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE);
	posix_spawnattr_set_persona_uid_np(&at, 0);
	posix_spawnattr_set_persona_gid_np(&at, 0);
	NSUInteger n = args.count + 2;
	char **argv = calloc(n, sizeof(char *));
	argv[0] = (char *)h.fileSystemRepresentation;
	for (NSUInteger i = 0; i < args.count; i++) argv[i + 1] = (char *)args[i].UTF8String;
	pid_t pid;
	int e = posix_spawn(&pid, argv[0], &fa, &at, argv, NULL);
	free(argv);
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	NSMutableData *d = [NSMutableData data];
	char buf[4096];
	ssize_t k;
	while (e == 0 && (k = read(fds[0], buf, sizeof buf)) > 0) [d appendBytes:buf length:k];
	close(fds[0]);
	if (e) { if (out) *out = [NSString stringWithFormat:@"no se pudo lanzar el instalador (%s)", strerror(e)]; return -1; }
	int st = 0;
	waitpid(pid, &st, 0);
	if (out) *out = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"";
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static NSString *sizeText(long long b)
{
	return b > 0 ? [NSByteCountFormatter stringFromByteCount:b countStyle:NSByteCountFormatterCountStyleFile] : @"";
}

#pragma mark - store (sources, apps, downloads)

@interface Store : NSObject
@property (strong) NSArray<StoreApp *> *apps;
@property (strong) NSMutableDictionary<NSString *, NSString *> *sourceNames;  // url -> name or error
@property (strong) NSMutableDictionary<NSString *, Job *> *jobs;             // bundle id -> job
@property (strong) NSCache *icons;
@property BOOL loading;
@property int busyFiles;
+ (instancetype)shared;
- (void)installFile:(NSString *)path name:(NSString *)name from:(UIViewController *)vc done:(void (^)(int))done;
- (NSArray<NSString *> *)sources;
- (void)setSources:(NSArray<NSString *> *)s;
- (void)reload:(void (^)(void))done;
- (void)icon:(NSURL *)u done:(void (^)(UIImage *))done;
- (void)install:(StoreApp *)a from:(UIViewController *)vc;
- (void)uninstall:(StoreApp *)a from:(UIViewController *)vc;
@end

static NSString *const kChanged = @"TiendaChanged";

static void alert(UIViewController *vc, NSString *t, NSString *m)
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:t message:m preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
	UIViewController *top = vc;
	while (top.presentedViewController) top = top.presentedViewController;
	[top presentViewController:a animated:YES completion:nil];
}

static void changed(void)
{
	dispatch_async(dispatch_get_main_queue(), ^{ [NSNotificationCenter.defaultCenter postNotificationName:kChanged object:nil]; });
}

// trollstorehelper's install codes: 0 ok; 182 (needs developer mode) and 184 (extra encrypted binaries) are
// installed with a warning; 180 is a FairPlay-encrypted main binary (an App Store copy, not a decrypted one)
static void reportInstall(UIViewController *vc, NSString *name, int rc, NSString *out)
{
	if (rc == 0) return;
	if (rc == 182) {
		alert(vc, [NSString stringWithFormat:@"%@ instalada", name],
		      @"Esta app está hecha para desarrolladores (pide el «Modo desarrollador»). Si no abre, activa el Modo desarrollador en Ajustes → Privacidad y seguridad, o en los ajustes de TrollStore, y reinicia.");
		return;
	}
	if (rc == 184) {
		alert(vc, [NSString stringWithFormat:@"%@ instalada", name],
		      @"Aviso: algunas partes de la app (extensiones) siguen cifradas y puede que no funcionen.");
		return;
	}
	if (rc == 180) {
		alert(vc, name, @"Esta IPA está cifrada con FairPlay (es una copia sacada de la App Store tal cual). Hace falta una versión descifrada.");
		return;
	}
	NSString *o = out.length > 600 ? [out substringFromIndex:out.length - 600] : out;
	alert(vc, name, [NSString stringWithFormat:@"TrollStore no pudo instalarla (código %d).\n%@", rc, o]);
}

@implementation Store
+ (instancetype)shared
{
	static Store *s;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		s = [Store new];
		s.apps = @[];
		s.sourceNames = [NSMutableDictionary new];
		s.jobs = [NSMutableDictionary new];
		s.icons = [NSCache new];
	});
	return s;
}

- (NSArray<NSString *> *)sources
{
	NSArray *a = [NSUserDefaults.standardUserDefaults arrayForKey:kSourcesKey];
	return a ?: defaultSources();
}

- (void)setSources:(NSArray<NSString *> *)s
{
	[NSUserDefaults.standardUserDefaults setObject:s forKey:kSourcesKey];
}

- (NSArray<StoreApp *> *)parse:(NSDictionary *)d source:(NSString *)src
{
	NSMutableArray *r = [NSMutableArray new];
	NSArray *apps = [d[@"apps"] isKindOfClass:NSArray.class] ? d[@"apps"] : @[];
	for (NSDictionary *x in apps) {
		if (![x isKindOfClass:NSDictionary.class]) continue;
		StoreApp *a = [StoreApp new];
		a.name = str(x[@"name"]);
		a.bundleID = str(x[@"bundleIdentifier"]);
		a.developer = str(x[@"developerName"]) ?: @"";
		a.subtitle = str(x[@"subtitle"]) ?: @"";
		a.desc = str(x[@"localizedDescription"]) ?: @"";
		a.iconURL = url(x[@"iconURL"]);
		a.source = src;
		NSDictionary *v = nil;
		if ([x[@"versions"] isKindOfClass:NSArray.class] && [x[@"versions"] count] &&
		    [x[@"versions"][0] isKindOfClass:NSDictionary.class])
			v = x[@"versions"][0];
		a.version = str(v[@"version"]) ?: str(x[@"version"]) ?: @"";
		a.downloadURL = url(v[@"downloadURL"]) ?: url(x[@"downloadURL"]);
		id size = v[@"size"] ?: x[@"size"];
		a.size = [size respondsToSelector:@selector(longLongValue)] ? [size longLongValue] : 0;
		a.minOS = str(v[@"minOSVersion"]) ?: str(x[@"minOSVersion"]);
		if (a.name.length && a.bundleID.length && a.downloadURL) [r addObject:a];
	}
	return r;
}

- (void)reload:(void (^)(void))done
{
	if (self.loading) { if (done) done(); return; }
	self.loading = YES;
	NSArray *srcs = self.sources;
	NSMutableArray *all = [NSMutableArray new];
	dispatch_group_t g = dispatch_group_create();
	NSURLSessionConfiguration *cfg = NSURLSessionConfiguration.ephemeralSessionConfiguration;
	cfg.timeoutIntervalForRequest = 25;
	cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
	NSURLSession *ses = [NSURLSession sessionWithConfiguration:cfg];
	for (NSString *s in srcs) {
		NSURL *u = [NSURL URLWithString:s];
		if (!u) continue;
		dispatch_group_enter(g);
		[[ses dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
			NSDictionary *d = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
			@synchronized(all) {
				if ([d isKindOfClass:NSDictionary.class]) {
					NSArray *p = [self parse:d source:s];
					self.sourceNames[s] = [NSString stringWithFormat:@"%@ · %lu apps", str(d[@"name"]) ?: u.host, (unsigned long)p.count];
					[all addObjectsFromArray:p];
				} else {
					self.sourceNames[s] = err ? [NSString stringWithFormat:@"Error: %@", err.localizedDescription] : @"Error: no es una fuente válida";
				}
			}
			dispatch_group_leave(g);
		}] resume];
	}
	dispatch_group_notify(g, dispatch_get_main_queue(), ^{
		NSMutableDictionary<NSString *, StoreApp *> *best = [NSMutableDictionary new];
		for (StoreApp *a in all) {
			StoreApp *o = best[a.bundleID];
			if (!o || newer(a.version, o.version)) best[a.bundleID] = a;
		}
		self.apps = [best.allValues sortedArrayUsingComparator:^NSComparisonResult(StoreApp *x, StoreApp *y) {
			return [x.name localizedCaseInsensitiveCompare:y.name];
		}];
		self.loading = NO;
		[ses finishTasksAndInvalidate];
		[NSNotificationCenter.defaultCenter postNotificationName:kChanged object:nil];
		if (done) done();
	});
}

- (void)icon:(NSURL *)u done:(void (^)(UIImage *))done
{
	if (!u) return;
	UIImage *c = [self.icons objectForKey:u];
	if (c) { done(c); return; }
	[[NSURLSession.sharedSession dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *r, NSError *e) {
		UIImage *img = data ? [UIImage imageWithData:data] : nil;
		if (!img) return;
		CGFloat side = 120;
		UIGraphicsImageRenderer *rr = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(side, side)];
		UIImage *out = [rr imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
			[[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, side, side) cornerRadius:side * 0.225] addClip];
			[img drawInRect:CGRectMake(0, 0, side, side)];
		}];
		[self.icons setObject:out forKey:u];
		dispatch_async(dispatch_get_main_queue(), ^{ done(out); });
	}] resume];
}

- (void)install:(StoreApp *)a from:(UIViewController *)vc
{
	if (self.jobs[a.bundleID]) return;
	if (!helperPath()) { alert(vc, @"Falta TrollStore", @"La Tienda instala las apps con TrollStore, y no lo encuentro."); return; }
	Job *j = [Job new];
	j.state = JobDownloading;
	NSString *dir = @"/var/mobile/Documents/Tienda";
	[NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	NSString *dst = [dir stringByAppendingPathComponent:[a.bundleID stringByAppendingString:@".ipa"]];
	j.task = [NSURLSession.sharedSession downloadTaskWithURL:a.downloadURL completionHandler:^(NSURL *tmp, NSURLResponse *resp, NSError *err) {
		NSInteger code = [resp isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)resp).statusCode : 200;
		if (err || !tmp || code >= 400) {
			dispatch_async(dispatch_get_main_queue(), ^{
				[self.jobs removeObjectForKey:a.bundleID];
				changed();
				if (err.code != NSURLErrorCancelled)
					alert(vc, a.name, err ? [NSString stringWithFormat:@"La descarga falló: %@", err.localizedDescription]
							      : [NSString stringWithFormat:@"La descarga falló (HTTP %ld)", (long)code]);
			});
			return;
		}
		[NSFileManager.defaultManager removeItemAtPath:dst error:nil];
		[NSFileManager.defaultManager moveItemAtPath:tmp.path toPath:dst error:nil];
		j.state = JobInstalling;
		changed();
		NSString *out = nil;
		int rc = runHelper(@[@"install", dst], &out);
		[NSFileManager.defaultManager removeItemAtPath:dst error:nil];
		dispatch_async(dispatch_get_main_queue(), ^{
			[self.jobs removeObjectForKey:a.bundleID];
			changed();
			reportInstall(vc, a.name, rc, out);
		});
	}];
	self.jobs[a.bundleID] = j;
	[j.task resume];
	changed();
}

// an IPA already on disk (downloaded by the browser, or opened from Files/Safari); the file is removed afterwards
- (void)installFile:(NSString *)path name:(NSString *)name from:(UIViewController *)vc done:(void (^)(int))done
{
	if (!helperPath()) { alert(vc, @"Falta TrollStore", @"La Tienda instala las apps con TrollStore, y no lo encuentro."); return; }
	self.busyFiles++;
	changed();
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		NSString *out = nil;
		int rc = runHelper(@[@"install", path], &out);
		[NSFileManager.defaultManager removeItemAtPath:path error:nil];
		dispatch_async(dispatch_get_main_queue(), ^{
			self.busyFiles--;
			changed();
			reportInstall(vc, name, rc, out);
			if (done) done(rc);
		});
	});
}

- (void)uninstall:(StoreApp *)a from:(UIViewController *)vc
{
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		NSString *out = nil;
		int rc = runHelper(@[@"uninstall", a.bundleID], &out);
		dispatch_async(dispatch_get_main_queue(), ^{
			changed();
			if (rc != 0)
				alert(vc, a.name, [NSString stringWithFormat:@"No se pudo desinstalar (código %d). Solo se pueden quitar desde aquí las apps instaladas con TrollStore.\n%@", rc, out]);
		});
	});
}
@end

#pragma mark - the get/open/update button

static NSString *buttonTitle(StoreApp *a, BOOL *busy)
{
	Store *s = Store.shared;
	Job *j = s.jobs[a.bundleID];
	*busy = j != nil;
	if (j) {
		if (j.state == JobInstalling) return @"Instalando…";
		double f = j.task.countOfBytesExpectedToReceive > 0
			? (double)j.task.countOfBytesReceived / j.task.countOfBytesExpectedToReceive : 0;
		return [NSString stringWithFormat:@"%d %%", (int)(f * 100)];
	}
	NSString *iv = installedVersion(a.bundleID);
	if (!iv) return @"Obtener";
	return newer(a.version, iv) ? @"Actualizar" : @"Abrir";
}

static UIButton *pill(void)
{
	UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
	b.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightBold];
	b.backgroundColor = [UIColor tertiarySystemFillColor];
	b.layer.cornerRadius = 15;
	b.contentEdgeInsets = UIEdgeInsetsMake(5, 14, 5, 14);
	return b;
}

static void pressed(StoreApp *a, UIViewController *vc)
{
	Store *s = Store.shared;
	Job *j = s.jobs[a.bundleID];
	if (j) {
		if (j.state == JobDownloading) [j.task cancel];
		return;
	}
	NSString *iv = installedVersion(a.bundleID);
	if (iv && !newer(a.version, iv)) {
		[[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] openApplicationWithBundleID:a.bundleID];
		return;
	}
	[s install:a from:vc];
}

#pragma mark - app detail

@interface DetailVC : UIViewController
@property (strong) StoreApp *app;
@property (strong) UIButton *button, *remove;
@property (strong) UILabel *info;
@end

@implementation DetailVC
- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.systemBackgroundColor;
	self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
	UIScrollView *sv = [UIScrollView new];
	sv.translatesAutoresizingMaskIntoConstraints = NO;
	sv.alwaysBounceVertical = YES;
	[self.view addSubview:sv];
	UIStackView *st = [UIStackView new];
	st.axis = UILayoutConstraintAxisVertical;
	st.spacing = 14;
	st.translatesAutoresizingMaskIntoConstraints = NO;
	[sv addSubview:st];
	[NSLayoutConstraint activateConstraints:@[
		[sv.topAnchor constraintEqualToAnchor:self.view.topAnchor], [sv.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
		[sv.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor], [sv.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[st.topAnchor constraintEqualToAnchor:sv.contentLayoutGuide.topAnchor constant:16],
		[st.bottomAnchor constraintEqualToAnchor:sv.contentLayoutGuide.bottomAnchor constant:-24],
		[st.leadingAnchor constraintEqualToAnchor:sv.frameLayoutGuide.leadingAnchor constant:20],
		[st.trailingAnchor constraintEqualToAnchor:sv.frameLayoutGuide.trailingAnchor constant:-20],
	]];

	UIStackView *head = [UIStackView new];
	head.spacing = 16;
	head.alignment = UIStackViewAlignmentCenter;
	UIImageView *iv = [UIImageView new];
	iv.backgroundColor = UIColor.tertiarySystemFillColor;
	iv.layer.cornerRadius = 22;
	iv.clipsToBounds = YES;
	[iv.widthAnchor constraintEqualToConstant:100].active = YES;
	[iv.heightAnchor constraintEqualToConstant:100].active = YES;
	[Store.shared icon:self.app.iconURL done:^(UIImage *img) { iv.image = img; }];
	UIStackView *names = [UIStackView new];
	names.axis = UILayoutConstraintAxisVertical;
	names.spacing = 4;
	names.alignment = UIStackViewAlignmentLeading;
	UILabel *n = [UILabel new];
	n.text = self.app.name;
	n.font = [UIFont systemFontOfSize:22 weight:UIFontWeightBold];
	n.numberOfLines = 2;
	UILabel *d = [UILabel new];
	d.text = self.app.subtitle.length ? self.app.subtitle : self.app.developer;
	d.textColor = UIColor.secondaryLabelColor;
	d.numberOfLines = 2;
	self.button = pill();
	[self.button addTarget:self action:@selector(tap) forControlEvents:UIControlEventTouchUpInside];
	[names addArrangedSubview:n];
	[names addArrangedSubview:d];
	[names setCustomSpacing:10 afterView:d];
	[names addArrangedSubview:self.button];
	[head addArrangedSubview:iv];
	[head addArrangedSubview:names];
	[st addArrangedSubview:head];

	self.info = [UILabel new];
	self.info.numberOfLines = 0;
	self.info.font = [UIFont systemFontOfSize:14];
	self.info.textColor = UIColor.secondaryLabelColor;
	[st addArrangedSubview:self.info];

	UILabel *desc = [UILabel new];
	desc.numberOfLines = 0;
	desc.text = self.app.desc;
	desc.font = [UIFont systemFontOfSize:16];
	[st addArrangedSubview:desc];

	self.remove = [UIButton buttonWithType:UIButtonTypeSystem];
	[self.remove setTitle:@"Desinstalar" forState:UIControlStateNormal];
	[self.remove setTitleColor:UIColor.systemRedColor forState:UIControlStateNormal];
	self.remove.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeading;
	[self.remove addTarget:self action:@selector(uninstall) forControlEvents:UIControlEventTouchUpInside];
	[st addArrangedSubview:self.remove];

	[NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refresh) name:kChanged object:nil];
	[self refresh];
}

- (void)refresh
{
	BOOL busy;
	[self.button setTitle:buttonTitle(self.app, &busy) forState:UIControlStateNormal];
	NSString *iv = installedVersion(self.app.bundleID);
	NSMutableArray *l = [NSMutableArray new];
	[l addObject:[NSString stringWithFormat:@"Versión %@%@", self.app.version, self.app.size ? [@" · " stringByAppendingString:sizeText(self.app.size)] : @""]];
	if (iv) [l addObject:[NSString stringWithFormat:@"Instalada: %@", iv]];
	if (self.app.developer.length) [l addObject:[NSString stringWithFormat:@"Desarrollador: %@", self.app.developer]];
	if (self.app.minOS.length && newer(self.app.minOS, UIDevice.currentDevice.systemVersion))
		[l addObject:[NSString stringWithFormat:@"⚠️ Necesita iOS %@ (este teléfono tiene %@)", self.app.minOS, UIDevice.currentDevice.systemVersion]];
	[l addObject:[NSString stringWithFormat:@"%@\n%@", self.app.bundleID, [NSURL URLWithString:self.app.source].host ?: @""]];
	self.info.text = [l componentsJoinedByString:@"\n"];
	self.remove.hidden = iv == nil || busy;
}

- (void)tap { pressed(self.app, self); }

- (void)uninstall
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"¿Desinstalar %@?", self.app.name]
								 message:@"Se borran la app y sus datos." preferredStyle:UIAlertControllerStyleAlert];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:nil]];
	[a addAction:[UIAlertAction actionWithTitle:@"Desinstalar" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *x) {
		[Store.shared uninstall:self.app from:self];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}
@end

#pragma mark - app list

static UIImage *placeholder(void);

@interface AppsVC : UITableViewController <UISearchResultsUpdating>
@property (strong) NSArray<StoreApp *> *shown;
@property (strong) UISearchController *search;
@property BOOL onlyInstalled;
@property (strong) NSTimer *timer;
@end

@implementation AppsVC
- (void)viewDidLoad
{
	[super viewDidLoad];
	self.tableView.rowHeight = 76;
	self.search = [[UISearchController alloc] initWithSearchResultsController:nil];
	self.search.searchResultsUpdater = self;
	self.search.obscuresBackgroundDuringPresentation = NO;
	self.search.searchBar.placeholder = @"Buscar apps";
	self.navigationItem.searchController = self.search;
	self.navigationItem.hidesSearchBarWhenScrolling = NO;
	self.refreshControl = [UIRefreshControl new];
	[self.refreshControl addTarget:self action:@selector(pull) forControlEvents:UIControlEventValueChanged];
	[NSNotificationCenter.defaultCenter addObserver:self selector:@selector(filter) name:kChanged object:nil];
	[self filter];
}

- (void)viewWillAppear:(BOOL)animated
{
	[super viewWillAppear:animated];
	[self filter];
	self.timer = [NSTimer scheduledTimerWithTimeInterval:0.4 repeats:YES block:^(NSTimer *t) {
		if (Store.shared.jobs.count) [self updateButtons];
	}];
}

- (void)viewWillDisappear:(BOOL)animated
{
	[super viewWillDisappear:animated];
	[self.timer invalidate];
}

- (void)pull
{
	[Store.shared reload:^{ [self.refreshControl endRefreshing]; }];
}

- (void)updateSearchResultsForSearchController:(UISearchController *)sc { [self filter]; }

- (void)filter
{
	NSString *q = self.search.searchBar.text;
	NSMutableArray *r = [NSMutableArray new];
	for (StoreApp *a in Store.shared.apps) {
		if (self.onlyInstalled && !installedVersion(a.bundleID)) continue;
		if (q.length && [a.name rangeOfString:q options:NSCaseInsensitiveSearch | NSDiacriticInsensitiveSearch].location == NSNotFound &&
		    [a.developer rangeOfString:q options:NSCaseInsensitiveSearch].location == NSNotFound)
			continue;
		[r addObject:a];
	}
	self.shown = r;
	[self.tableView reloadData];
	if (!Store.shared.apps.count && Store.shared.loading) {
		UIActivityIndicatorView *sp = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
		[sp startAnimating];
		self.tableView.backgroundView = sp;
	} else if (!r.count) {
		UILabel *l = [UILabel new];
		l.text = self.onlyInstalled ? @"Ninguna app de tus fuentes está instalada" : @"No hay apps.\nAñade fuentes en la pestaña Fuentes.";
		l.numberOfLines = 0;
		l.textAlignment = NSTextAlignmentCenter;
		l.textColor = UIColor.secondaryLabelColor;
		self.tableView.backgroundView = l;
	} else {
		self.tableView.backgroundView = nil;
	}
}

- (void)updateButtons
{
	for (NSIndexPath *ip in self.tableView.indexPathsForVisibleRows) {
		UITableViewCell *c = [self.tableView cellForRowAtIndexPath:ip];
		UIButton *b = (UIButton *)c.accessoryView;
		BOOL busy;
		[b setTitle:buttonTitle(self.shown[ip.row], &busy) forState:UIControlStateNormal];
		[b sizeToFit];
	}
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s { return self.shown.count; }

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [tv dequeueReusableCellWithIdentifier:@"a"];
	if (!c) {
		c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"a"];
		c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
		c.textLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
		UIButton *b = pill();
		[b addTarget:self action:@selector(buttonTap:) forControlEvents:UIControlEventTouchUpInside];
		c.accessoryView = b;
	}
	StoreApp *a = self.shown[ip.row];
	c.textLabel.text = a.name;
	c.detailTextLabel.text = a.subtitle.length ? a.subtitle : a.developer;
	c.imageView.image = [Store.shared.icons objectForKey:a.iconURL] ?: placeholder();
	if (![Store.shared.icons objectForKey:a.iconURL]) {
		[Store.shared icon:a.iconURL done:^(UIImage *img) {
			UITableViewCell *cc = [tv cellForRowAtIndexPath:ip];
			if (cc && ip.row < (NSInteger)self.shown.count && self.shown[ip.row] == a) { cc.imageView.image = img; [cc setNeedsLayout]; }
		}];
	}
	UIButton *b = (UIButton *)c.accessoryView;
	b.tag = ip.row;
	BOOL busy;
	[b setTitle:buttonTitle(a, &busy) forState:UIControlStateNormal];
	[b sizeToFit];
	return c;
}

static UIImage *placeholder(void)
{
	static UIImage *p;
	if (!p) {
		UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(120, 120)];
		p = [r imageWithActions:^(UIGraphicsImageRendererContext *ctx) {
			[UIColor.tertiarySystemFillColor setFill];
			[[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, 120, 120) cornerRadius:27] fill];
		}];
	}
	return p;
}

- (void)tableView:(UITableView *)tv willDisplayCell:(UITableViewCell *)c forRowAtIndexPath:(NSIndexPath *)ip
{
	c.imageView.transform = CGAffineTransformIdentity;
}

- (void)buttonTap:(UIButton *)b
{
	if (b.tag < (NSInteger)self.shown.count) pressed(self.shown[b.tag], self);
	[self updateButtons];
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	DetailVC *d = [DetailVC new];
	d.app = self.shown[ip.row];
	[self.navigationController pushViewController:d animated:YES];
}
@end

#pragma mark - sources

@interface SourcesVC : UITableViewController
@end

@implementation SourcesVC
- (void)viewDidLoad
{
	[super viewDidLoad];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(add)];
	self.navigationItem.leftBarButtonItem = self.editButtonItem;
	self.refreshControl = [UIRefreshControl new];
	[self.refreshControl addTarget:self action:@selector(pull) forControlEvents:UIControlEventValueChanged];
	[NSNotificationCenter.defaultCenter addObserver:self.tableView selector:@selector(reloadData) name:kChanged object:nil];
}

- (void)pull { [Store.shared reload:^{ [self.refreshControl endRefreshing]; }]; }

- (void)add
{
	UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Añadir fuente"
								 message:@"URL de una fuente en formato AltStore / SideStore (JSON)" preferredStyle:UIAlertControllerStyleAlert];
	[a addTextFieldWithConfigurationHandler:^(UITextField *f) {
		f.placeholder = @"https://…";
		f.keyboardType = UIKeyboardTypeURL;
		f.autocapitalizationType = UITextAutocapitalizationTypeNone;
		f.autocorrectionType = UITextAutocorrectionTypeNo;
		f.text = UIPasteboard.generalPasteboard.hasURLs ? UIPasteboard.generalPasteboard.URL.absoluteString : nil;
	}];
	[a addAction:[UIAlertAction actionWithTitle:@"Cancelar" style:UIAlertActionStyleCancel handler:nil]];
	[a addAction:[UIAlertAction actionWithTitle:@"Añadir" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
		NSString *s = [a.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
		if (!s.length) return;
		if (![s containsString:@"://"]) s = [@"https://" stringByAppendingString:s];
		NSMutableArray *l = [Store.shared.sources mutableCopy];
		if (![l containsObject:s]) [l addObject:s];
		Store.shared.sources = l;
		[self.tableView reloadData];
		[Store.shared reload:nil];
	}]];
	[self presentViewController:a animated:YES completion:nil];
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s { return Store.shared.sources.count; }

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)s
{
	return @"Las apps se instalan con TrollStore. Desliza una fuente para quitarla; tira hacia abajo para actualizar.";
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [tv dequeueReusableCellWithIdentifier:@"s"] ?: [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"s"];
	NSString *s = Store.shared.sources[ip.row];
	NSString *n = Store.shared.sourceNames[s];
	c.textLabel.text = n ?: (Store.shared.loading ? @"Cargando…" : s);
	c.textLabel.textColor = [n hasPrefix:@"Error"] ? UIColor.systemRedColor : UIColor.labelColor;
	c.detailTextLabel.text = s;
	c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
	c.selectionStyle = UITableViewCellSelectionStyleNone;
	return c;
}

- (void)tableView:(UITableView *)tv commitEditingStyle:(UITableViewCellEditingStyle)st forRowAtIndexPath:(NSIndexPath *)ip
{
	if (st != UITableViewCellEditingStyleDelete) return;
	NSMutableArray *l = [Store.shared.sources mutableCopy];
	[l removeObjectAtIndex:ip.row];
	Store.shared.sources = l;
	[tv deleteRowsAtIndexPaths:@[ip] withRowAnimation:UITableViewRowAnimationAutomatic];
	[Store.shared reload:nil];
}
@end

#pragma mark - browser (decrypted IPA sites): downloads of IPAs are caught and installed

@interface BrowserVC : UIViewController <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
@property (strong) NSURL *start;
@property (copy) NSString *appName;
@property (strong) WKWebView *web;
@property (strong) UIProgressView *bar;
@property (strong) WKDownload *download;
@property (copy) NSString *dlPath, *dlName;
@property (strong) NSTimer *timer;
@end

@implementation BrowserVC
- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = UIColor.systemBackgroundColor;
	self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
	WKWebViewConfiguration *cfg = [WKWebViewConfiguration new];
	self.web = [[WKWebView alloc] initWithFrame:self.view.bounds configuration:cfg];
	self.web.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	self.web.navigationDelegate = self;
	self.web.UIDelegate = self;
	self.web.allowsBackForwardNavigationGestures = YES;
	self.web.customUserAgent = @"Mozilla/5.0 (iPhone; CPU iPhone OS 16_7 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1";
	[self.view addSubview:self.web];
	self.bar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
	self.bar.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:self.bar];
	[NSLayoutConstraint activateConstraints:@[
		[self.bar.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
		[self.bar.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[self.bar.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
	]];
	UIBarButtonItem *back = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"chevron.backward"] style:UIBarButtonItemStylePlain target:self.web action:@selector(goBack)];
	UIBarButtonItem *reload = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self.web action:@selector(reload)];
	self.navigationItem.rightBarButtonItems = @[reload, back];
	self.title = self.appName ?: @"Navegador";
	[self.web loadRequest:[NSURLRequest requestWithURL:self.start]];
	self.timer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *t) { [self tick]; }];
}

- (void)viewDidDisappear:(BOOL)animated
{
	[super viewDidDisappear:animated];
	if (self.isMovingFromParentViewController) [self.timer invalidate];
}

- (void)tick
{
	if (self.download) {
		double f = self.download.progress.fractionCompleted;
		self.bar.hidden = NO;
		self.bar.progress = f;
		self.navigationItem.prompt = [NSString stringWithFormat:@"Descargando %@ · %d %%", self.dlName ?: @"", (int)(f * 100)];
	} else if (Store.shared.busyFiles) {
		self.bar.hidden = YES;
		self.navigationItem.prompt = @"Instalando…";
	} else {
		self.navigationItem.prompt = nil;
		self.bar.hidden = self.web.estimatedProgress >= 1.0 || !self.web.isLoading;
		self.bar.progress = self.web.estimatedProgress;
	}
}

static BOOL looksLikeIPA(NSURLResponse *r)
{
	NSString *p = r.URL.path.lowercaseString, *m = r.MIMEType.lowercaseString, *name = r.suggestedFilename.lowercaseString;
	if ([p hasSuffix:@".ipa"] || [name hasSuffix:@".ipa"]) return YES;
	if ([m isEqualToString:@"application/octet-stream"] || [m containsString:@"zip"] || [m containsString:@"itunes"]) return YES;
	if ([r isKindOfClass:NSHTTPURLResponse.class]) {
		NSString *cd = [((NSHTTPURLResponse *)r).allHeaderFields[@"Content-Disposition"] lowercaseString];
		if ([cd containsString:@"attachment"]) return YES;
	}
	return NO;
}

- (void)webView:(WKWebView *)wv decidePolicyForNavigationAction:(WKNavigationAction *)a decisionHandler:(void (^)(WKNavigationActionPolicy))h
{
	NSString *s = a.request.URL.scheme.lowercaseString;
	if (s.length && ![@[@"http", @"https", @"about", @"blob", @"data"] containsObject:s]) {
		[UIApplication.sharedApplication openURL:a.request.URL options:@{} completionHandler:nil];
		h(WKNavigationActionPolicyCancel);
		return;
	}
	h(a.shouldPerformDownload ? WKNavigationActionPolicyDownload : WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView *)wv decidePolicyForNavigationResponse:(WKNavigationResponse *)r decisionHandler:(void (^)(WKNavigationResponsePolicy))h
{
	BOOL dl = r.isForMainFrame && (looksLikeIPA(r.response) || !r.canShowMIMEType);
	h(dl ? WKNavigationResponsePolicyDownload : WKNavigationResponsePolicyAllow);
}

- (void)webView:(WKWebView *)wv navigationAction:(WKNavigationAction *)a didBecomeDownload:(WKDownload *)d { d.delegate = self; }
- (void)webView:(WKWebView *)wv navigationResponse:(WKNavigationResponse *)r didBecomeDownload:(WKDownload *)d { d.delegate = self; }

// target=_blank links open here
- (WKWebView *)webView:(WKWebView *)wv createWebViewWithConfiguration:(WKWebViewConfiguration *)c forNavigationAction:(WKNavigationAction *)a windowFeatures:(WKWindowFeatures *)f
{
	if (a.request.URL) [wv loadRequest:a.request];
	return nil;
}

- (void)download:(WKDownload *)d decideDestinationUsingResponse:(NSURLResponse *)r suggestedFilename:(NSString *)name completionHandler:(void (^)(NSURL *))h
{
	if (self.download) { h(nil); return; }   // one at a time
	NSString *dir = @"/var/mobile/Documents/Tienda";
	[NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	self.dlName = name.length ? name : @"app.ipa";
	self.dlPath = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"web-%@.ipa", NSUUID.UUID.UUIDString]];
	self.download = d;
	h([NSURL fileURLWithPath:self.dlPath]);
}

- (void)downloadDidFinish:(WKDownload *)d
{
	self.download = nil;
	NSString *path = self.dlPath, *name = [self.dlName stringByDeletingPathExtension];
	NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
	NSData *magic = [fh readDataOfLength:2];
	[fh closeFile];
	if (magic.length < 2 || memcmp(magic.bytes, "PK", 2) != 0) {
		[NSFileManager.defaultManager removeItemAtPath:path error:nil];
		alert(self, self.dlName, @"Lo descargado no es una IPA (no es un archivo zip).");
		return;
	}
	[Store.shared installFile:path name:self.appName ?: name from:self done:^(int rc) {
		if (rc == 0) alert(self, [NSString stringWithFormat:@"%@ instalada", self.appName ?: name], @"Ya está en la pantalla de inicio.");
	}];
}

- (void)download:(WKDownload *)d didFailWithError:(NSError *)e resumeData:(NSData *)rd
{
	self.download = nil;
	[NSFileManager.defaultManager removeItemAtPath:self.dlPath error:nil];
	alert(self, self.dlName, [NSString stringWithFormat:@"La descarga falló: %@", e.localizedDescription]);
}
@end

#pragma mark - App Store catalog (Apple's public search API; free apps only), installs through the browser

static NSString *const kStoreSite = @"https://armconverter.com/store";
static NSString *const kPopular = @"310633997,389801252,835599320,686449807,544007664,324684580,363590051,447188370,333903271,284882215,454638411,985746746,585027354,422689480,535886823,1094591345,1446075923";

@interface CatalogVC : UITableViewController <UISearchBarDelegate>
@property (strong) NSArray<NSDictionary *> *items;
@property (strong) UISearchController *search;
@property (strong) NSURLSessionDataTask *task;
@property BOOL loading;
@end

@implementation CatalogVC
- (void)viewDidLoad
{
	[super viewDidLoad];
	self.tableView.rowHeight = 76;
	self.items = @[];
	self.search = [[UISearchController alloc] initWithSearchResultsController:nil];
	self.search.searchBar.delegate = self;
	self.search.obscuresBackgroundDuringPresentation = NO;
	self.search.searchBar.placeholder = @"Buscar en la App Store";
	self.navigationItem.searchController = self.search;
	self.navigationItem.hidesSearchBarWhenScrolling = NO;
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"safari"] style:UIBarButtonItemStylePlain target:self action:@selector(browser)];
	[NSNotificationCenter.defaultCenter addObserver:self.tableView selector:@selector(reloadData) name:kChanged object:nil];
	[self query:[NSString stringWithFormat:@"https://itunes.apple.com/lookup?id=%@&country=es&entity=software", kPopular]];
}

- (void)browser
{
	BrowserVC *b = [BrowserVC new];
	b.start = [NSURL URLWithString:kStoreSite];
	[self.navigationController pushViewController:b animated:YES];
}

- (void)searchBarSearchButtonClicked:(UISearchBar *)sb
{
	NSString *q = [sb.text stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLQueryAllowedCharacterSet];
	if (!q.length) return;
	[self query:[NSString stringWithFormat:@"https://itunes.apple.com/search?term=%@&country=es&entity=software&limit=60", q]];
}

- (void)searchBarCancelButtonClicked:(UISearchBar *)sb
{
	[self query:[NSString stringWithFormat:@"https://itunes.apple.com/lookup?id=%@&country=es&entity=software", kPopular]];
}

- (void)query:(NSString *)u
{
	[self.task cancel];
	self.loading = YES;
	[self empty];
	self.task = [NSURLSession.sharedSession dataTaskWithURL:[NSURL URLWithString:u] completionHandler:^(NSData *data, NSURLResponse *r, NSError *e) {
		NSDictionary *d = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
		NSMutableArray *ok = [NSMutableArray new];
		for (NSDictionary *x in ([d isKindOfClass:NSDictionary.class] ? d[@"results"] : nil)) {
			if (![x isKindOfClass:NSDictionary.class] || !str(x[@"bundleId"]) || !str(x[@"trackViewUrl"])) continue;
			if ([x[@"price"] doubleValue] > 0) continue;          // free apps only
			[ok addObject:x];
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			if (e.code == NSURLErrorCancelled) return;
			self.loading = NO;
			self.items = ok;
			[self.tableView reloadData];
			[self empty];
			if (e) alert(self, @"App Store", [NSString stringWithFormat:@"No se pudo buscar: %@", e.localizedDescription]);
		});
	}];
	[self.task resume];
}

- (void)empty
{
	if (self.loading && !self.items.count) {
		UIActivityIndicatorView *sp = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
		[sp startAnimating];
		self.tableView.backgroundView = sp;
	} else if (!self.items.count) {
		UILabel *l = [UILabel new];
		l.text = @"Sin resultados gratuitos";
		l.textAlignment = NSTextAlignmentCenter;
		l.textColor = UIColor.secondaryLabelColor;
		self.tableView.backgroundView = l;
	} else {
		self.tableView.backgroundView = nil;
	}
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s { return self.items.count; }

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)s
{
	return self.items.count ? @"Solo apps gratuitas. «Obtener» abre la página de la versión descifrada; al pulsar descargar allí, la Tienda la instala sola." : nil;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [tv dequeueReusableCellWithIdentifier:@"c"];
	if (!c) {
		c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"c"];
		c.detailTextLabel.textColor = UIColor.secondaryLabelColor;
		c.textLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
		UIButton *b = pill();
		[b addTarget:self action:@selector(buttonTap:) forControlEvents:UIControlEventTouchUpInside];
		c.accessoryView = b;
	}
	NSDictionary *x = self.items[ip.row];
	c.textLabel.text = str(x[@"trackName"]);
	NSString *minOS = str(x[@"minimumOsVersion"]);
	NSString *sub = str(x[@"sellerName"]) ?: str(x[@"artistName"]) ?: @"";
	if (minOS && newer(minOS, UIDevice.currentDevice.systemVersion))
		sub = [NSString stringWithFormat:@"⚠️ la última pide iOS %@ · %@", minOS, sub];
	c.detailTextLabel.text = sub;
	NSURL *icon = url(x[@"artworkUrl100"]) ?: url(x[@"artworkUrl60"]);
	c.imageView.image = [Store.shared.icons objectForKey:icon] ?: placeholder();
	if (![Store.shared.icons objectForKey:icon]) {
		[Store.shared icon:icon done:^(UIImage *img) {
			UITableViewCell *cc = [tv cellForRowAtIndexPath:ip];
			if (cc && ip.row < (NSInteger)self.items.count && self.items[ip.row] == x) { cc.imageView.image = img; [cc setNeedsLayout]; }
		}];
	}
	UIButton *b = (UIButton *)c.accessoryView;
	b.tag = ip.row;
	[b setTitle:installedVersion(str(x[@"bundleId"])) ? @"Abrir" : @"Obtener" forState:UIControlStateNormal];
	[b sizeToFit];
	return c;
}

- (void)buttonTap:(UIButton *)b
{
	if (b.tag >= (NSInteger)self.items.count) return;
	NSDictionary *x = self.items[b.tag];
	if (installedVersion(str(x[@"bundleId"]))) {
		[[NSClassFromString(@"LSApplicationWorkspace") defaultWorkspace] openApplicationWithBundleID:str(x[@"bundleId"])];
		return;
	}
	[self get:x];
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	[self get:self.items[ip.row]];
}

- (void)get:(NSDictionary *)x
{
	// https://apps.apple.com/es/app/whatsapp-messenger/id310633997?uo=4 -> https://armconverter.com/store/es/app/whatsapp-messenger/id310633997
	NSURL *t = url(x[@"trackViewUrl"]);
	BrowserVC *b = [BrowserVC new];
	b.appName = str(x[@"trackName"]);
	b.start = [NSURL URLWithString:[kStoreSite stringByAppendingString:t.path ?: @"/"]];
	[self.navigationController pushViewController:b animated:YES];
}
@end

#pragma mark - app

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

static UINavigationController *nav(UIViewController *vc, NSString *title, NSString *symbol)
{
	vc.title = title;
	UINavigationController *n = [[UINavigationController alloc] initWithRootViewController:vc];
	n.navigationBar.prefersLargeTitles = YES;
	n.tabBarItem = [[UITabBarItem alloc] initWithTitle:title image:[UIImage systemImageNamed:symbol] tag:0];
	return n;
}

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	UITabBarController *tb = [UITabBarController new];
	AppsVC *all = [[AppsVC alloc] initWithStyle:UITableViewStylePlain];
	AppsVC *mine = [[AppsVC alloc] initWithStyle:UITableViewStylePlain];
	mine.onlyInstalled = YES;
	SourcesVC *src = [[SourcesVC alloc] initWithStyle:UITableViewStyleInsetGrouped];
	CatalogVC *cat = [[CatalogVC alloc] initWithStyle:UITableViewStyleGrouped];
	tb.viewControllers = @[nav(cat, @"App Store", @"bag.fill"), nav(all, @"Apps", @"square.grid.2x2.fill"),nav(mine, @"Instaladas", @"arrow.down.app.fill"),
			       nav(src, @"Fuentes", @"tray.full.fill")];
	self.window.rootViewController = tb;
	[self.window makeKeyAndVisible];
	[Store.shared reload:nil];
	return YES;
}

- (void)applicationDidBecomeActive:(UIApplication *)app
{
	changed();   // installed versions may have changed while away
}

// tienda://add?url=… or opening an AltStore "altstore://source?url=" style link adds a source
- (BOOL)application:(UIApplication *)app openURL:(NSURL *)u options:(NSDictionary *)o
{
	if (u.isFileURL) {   // an .ipa opened with Tienda from Files, Safari or a share sheet
		BOOL scoped = [u startAccessingSecurityScopedResource];
		NSString *dir = @"/var/mobile/Documents/Tienda";
		[NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
		NSString *dst = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"open-%@.ipa", NSUUID.UUID.UUIDString]];
		NSError *e = nil;
		BOOL ok = [NSFileManager.defaultManager copyItemAtPath:u.path toPath:dst error:&e];
		if (scoped) [u stopAccessingSecurityScopedResource];
		NSString *name = u.lastPathComponent.stringByDeletingPathExtension;
		UIViewController *vc = self.window.rootViewController;
		if (!ok) { alert(vc, name, [NSString stringWithFormat:@"No pude leer el archivo: %@", e.localizedDescription]); return NO; }
		[Store.shared installFile:dst name:name from:vc done:^(int rc) {
			if (rc == 0) alert(vc, [NSString stringWithFormat:@"%@ instalada", name], @"Ya está en la pantalla de inicio.");
		}];
		return YES;
	}
	NSURLComponents *c = [NSURLComponents componentsWithURL:u resolvingAgainstBaseURL:NO];
	for (NSURLQueryItem *q in c.queryItems) {
		if ([q.name isEqualToString:@"url"] && q.value.length) {
			NSMutableArray *l = [Store.shared.sources mutableCopy];
			if (![l containsObject:q.value]) [l addObject:q.value];
			Store.shared.sources = l;
			[Store.shared reload:nil];
			return YES;
		}
	}
	return NO;
}
@end

int main(int argc, char **argv)
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
	}
}
