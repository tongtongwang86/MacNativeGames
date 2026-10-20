/* ACNH on iPhone / iPad: UIScene app around the native runtime (runtime/frontend_ios.m), as games/totk/ios does. */
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include "runtime.h"
#import "touch_pad.h"
#include "../../vendor/savesync/savesync.h"

int totk_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
static char *host_argv[3];
static void *run_host(void *unused) { (void)unused; return (void *)(intptr_t)totk_host_main(3, host_argv); }
/* The desktop keyboard-overlay shortcut has no counterpart on iOS. */
void runtime_shortcut_help_toggle(void) {}

@interface ACNHMetalView : UIView @end
@implementation ACNHMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface ACNHGameController : UIViewController @property(nonatomic,strong) ACNHMetalView *gameView; @end
@implementation ACNHGameController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.gameView = [[ACNHMetalView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.gameView];
    runtime_ios_attach_layer((CAMetalLayer *)self.gameView.layer, self.gameView);
    TOTKTouchPad *pad=[[TOTKTouchPad alloc] initWithFrame:self.view.bounds];pad.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:pad];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started; if (started) return; started = YES;
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread; pthread_create(&thread, &attr, run_host, NULL);
    pthread_detach(thread); pthread_attr_destroy(&attr);
}
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

@interface ACNHApp : UIResponder <UIApplicationDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation ACNHApp
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)options;
    // Controller input does not reset the idle timer; auto-lock would suspend the game mid-play or mid shader import.
    application.idleTimerDisabled = YES;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData"];
    for (NSString *name in @[@"save", @"temp", @"shader-cache", @"game-slot"]) [fm createDirectoryAtPath:[docs stringByAppendingPathComponent:name] withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *log = [docs stringByAppendingPathComponent:@"log.txt"];
    if (freopen(log.fileSystemRepresentation,"w",stderr)) setvbuf(stderr,NULL,_IOLBF,0);
    setenv("ACNH_ROMFS",[res stringByAppendingPathComponent:@"romfs"].fileSystemRepresentation,1);
    setenv("ACNH_AOC",[res stringByAppendingPathComponent:@"aoc"].fileSystemRepresentation,1);   /* DLC, one folder per index */
    /* The account the island's residents belong to (the Mac profile's, bundled by build_ipa.py), if the build had one. */
    NSString *uid = [res stringByAppendingPathComponent:@"account.uid"];
    if ([fm fileExistsAtPath:uid]) setenv("ACNH_ACCOUNT_UID_FILE", uid.fileSystemRepresentation, 1);
    setenv("ACNH_SPEEDFIX_DIR",[res stringByAppendingPathComponent:@"speedfix"].fileSystemRepresentation,1);   /* 60 FPS SpeedFix, when the build had it */
    setenv("TOTK_OPERATION_MODE","handheld",1);
    /* System fonts: the console's own dump when the build found one (GameData/shared-fonts), else the font the Mac
       build bundled (GameData/font.ttc / .ttf; iOS keeps its system fonts out of the app's reach). */
    setenv("ACNH_SHARED_FONTS",[res stringByAppendingPathComponent:@"shared-fonts"].fileSystemRepresentation,1);
    for (NSString *name in @[@"font.ttc", @"font.ttf", @"font.otf"]) {
        NSString *f = [res stringByAppendingPathComponent:name];
        if ([fm fileExistsAtPath:f]) { setenv("ACNH_FONT", f.fileSystemRepresentation, 0); break; }
    }
    setenv("TOTK_SHADER_SEED_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    setenv("TOTK_SHADER_CACHE",[docs stringByAppendingPathComponent:@"shader-cache"].fileSystemRepresentation,1);
    setenv("DM_NVN_SHMAP",[res stringByAppendingPathComponent:@"shader-seed/shader_map.tsv"].fileSystemRepresentation,1);
    setenv("DM_MSL_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    // The same environment as make boot on the Mac; the bundle is read-only, so state goes to Documents (saves: Files app).
    setenv("TOTK_PROFILE_DIR",[docs stringByAppendingPathComponent:@"save"].fileSystemRepresentation,1);
    setenv("TOTK_GAME_SLOT_DIR",[docs stringByAppendingPathComponent:@"game-slot"].fileSystemRepresentation,1);
    for (NSString *name in @[@"TOTK_GPU_ALLOW_SUBMIT",@"TOTK_GPU_WIDEN",@"TOTK_GPU_BLOCKRT",@"TOTK_GPU_MIDCLEAR"]) setenv(name.UTF8String,"1",1);
    setenv("TOTK_SLOT_OWNER","user",0); setenv("TOTK_SLOT_PURPOSE","player",0);
    unsetenv("TOTK_AUTOLOAD");
    chdir(docs.fileSystemRepresentation);
    host_argv[0] = strdup("ACNH"); host_argv[1] = strdup(res.fileSystemRepresentation); host_argv[2] = strdup("boot");
    return YES;
}
@end

/* The window comes with the scene. Info.plist (project.yml) names this class in its UIApplicationSceneManifest, and with a
   scene manifest a window the app delegate makes is in no scene and never on screen: until 2026-10-20 the game ran, played
   its sound and drew into a layer nobody saw (the display link stopped calling back, "visibility unknown"). iOS 27 also
   stops apps built with its SDK that do not adopt the UIScene life cycle. As games/totk/ios/Sources/main.m. */
@interface ACNHScene : UIResponder <UIWindowSceneDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation ACNHScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [ACNHGameController new]; [self.window makeKeyAndVisible];
}
/* Leaving the game: upload a save made in the last seconds now (savesync waits a few seconds after each commit), with
   the time iOS gives a backgrounded app, before it is suspended. */
- (void)sceneDidEnterBackground:(UIScene *)scene {
    (void)scene;
    __block UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"savesync" expirationHandler:^{
        [UIApplication.sharedApplication endBackgroundTask:task]; task = UIBackgroundTaskInvalid;
    }];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        savesync_flush(20);
        dispatch_async(dispatch_get_main_queue(), ^{ if (task != UIBackgroundTaskInvalid) { [UIApplication.sharedApplication endBackgroundTask:task]; task = UIBackgroundTaskInvalid; } });
    });
}
@end

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(ACNHApp.class)); } }
