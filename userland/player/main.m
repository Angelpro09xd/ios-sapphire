/*
 * Reproductor: a tiny music player for the sapphire port's sound test. Lists the .mp3 / .m4a /
 * .wav files in /var/mobile/Media/Music and the app's Documents (Files app: "On My iPhone"),
 * plays the one tapped with AVAudioPlayer, then the next.
 */
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

@interface PlayerVC : UITableViewController <AVAudioPlayerDelegate>
@property (nonatomic, strong) NSArray<NSURL *> *songs;
@property (nonatomic, strong) AVAudioPlayer *player;
@property (nonatomic) NSInteger current;
@end

@implementation PlayerVC

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Reproductor";
	self.current = -1;
	[[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:nil];
	[[AVAudioSession sharedInstance] setActive:YES error:nil];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemPause
	    target:self action:@selector(toggle)];
	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
	    target:self action:@selector(reload)];
	[self reload];
}

- (void)reload
{
	NSMutableArray *found = [NSMutableArray array];
	NSArray *dirs = @[ @"/var/mobile/Media/Music",
		[NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject] ?: @"" ];
	for (NSString *d in dirs) {
		for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:d error:nil]) {
			NSString *e = f.pathExtension.lowercaseString;
			if ([@[ @"mp3", @"m4a", @"wav", @"aac", @"caf" ] containsObject:e]) {
				[found addObject:[NSURL fileURLWithPath:[d stringByAppendingPathComponent:f]]];
			}
		}
	}
	self.songs = found;
	[self.tableView reloadData];
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s
{
	return self.songs.count ?: 1;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
	UITableViewCell *c = [tv dequeueReusableCellWithIdentifier:@"s"] ?:
	    [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"s"];
	if (self.songs.count == 0) {
		c.textLabel.text = @"No hay canciones";
		c.detailTextLabel.text = @"Pon .mp3 en /var/mobile/Media/Music";
		return c;
	}
	NSURL *u = self.songs[ip.row];
	c.textLabel.text = u.lastPathComponent.stringByDeletingPathExtension;
	c.detailTextLabel.text = ip.row == self.current ? (self.player.playing ? @"▶︎ Sonando" : @"❚❚ En pausa") : u.pathExtension.uppercaseString;
	return c;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
	[tv deselectRowAtIndexPath:ip animated:YES];
	if (ip.row < (NSInteger)self.songs.count) {
		[self play:ip.row];
	}
}

- (void)play:(NSInteger)i
{
	NSError *err = nil;
	self.player = [[AVAudioPlayer alloc] initWithContentsOfURL:self.songs[i] error:&err];
	self.player.delegate = self;
	self.current = i;
	if (self.player == nil || ![self.player play]) {
		UIAlertController *a = [UIAlertController alertControllerWithTitle:@"No se puede reproducir"
		    message:err.localizedDescription ?: @"?" preferredStyle:UIAlertControllerStyleAlert];
		[a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
		[self presentViewController:a animated:YES completion:nil];
	}
	[self.tableView reloadData];
}

- (void)toggle
{
	if (self.player == nil) {
		if (self.songs.count > 0) {
			[self play:0];
		}
		return;
	}
	if (self.player.playing) {
		[self.player pause];
	} else {
		[self.player play];
	}
	[self.tableView reloadData];
}

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)p successfully:(BOOL)ok
{
	if (self.current + 1 < (NSInteger)self.songs.count) {
		[self play:self.current + 1];
	} else {
		[self.tableView reloadData];
	}
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)o
{
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:[[PlayerVC alloc] init]];
	[self.window makeKeyAndVisible];
	return YES;
}
@end

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
	}
}
