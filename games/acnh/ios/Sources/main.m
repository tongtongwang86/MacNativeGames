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
#include "../../vendor/gamesettings/gamesettings.h"
int fps_overlay_hints_enabled(void);
int fps_overlay_toggle_hints(void);
uint64_t dm_nvn_frame_count(void);
int mfx_enabled(void);
void dm_gfx_set_flag(const char *name, int v);

/* The Settings sheet on iPhone / iPad (vendor/gamesettings): picture shape, render resolution, upscaling, frame
   interpolation, motion controls (the device's own gyro, device_motion.m, or a controller's) and the FPS overlay;
   keyboard bindings and the save folders. */
static int shape_get(void) { return runtime_aspect_on() ? 0 : 1; }
static void shape_set(int v) { if (shape_get() != v) runtime_aspect_toggle(); }
static int upscale_get(void) { return runtime_ios_full_resolution(); }
static void upscale_set(int v) { runtime_ios_set_full_resolution(v); }
static int interp_get(void) { return mfx_enabled(); }
static void interp_set(int v) { dm_gfx_set_flag("mfx_interp", v); }
static int motion_get(void) { return runtime_device_motion_enabled(); }
static void motion_set(int v) { runtime_hid_feedback_flag("gyro", !!v); runtime_device_motion_enable(v); }   /* off: the game sees a pad lying still */
/* Render resolution: the 3D scene's size (the UI keeps the game's 720p), read once at start (TOTK_RENDER_RES, before the
   game thread; runtime/acnh_patches.c render_install), so a change takes effect at the next start; kept in NSUserDefaults
   for that and in the Settings like the rest. 480p is 864x486, exactly 16:9 like the game's own sizes. */
static const char *const render_sizes[] = {"864x486", "960x540", "1280x720", "1600x900", "1920x1080"};
static const char *const render_choices[] = {"480p", "540p", "720p (the game's)", "900p", "1080p"};
static int render_get(void) {
    NSString *v = [NSUserDefaults.standardUserDefaults stringForKey:@"ACNHRenderRes"];
    for (int i = 0; i < 5; i++) if ([v isEqualToString:@(render_sizes[i])]) return i;
    return 2;
}
static void render_set(int v) { if (v >= 0 && v < 5) [NSUserDefaults.standardUserDefaults setObject:@(render_sizes[v]) forKey:@"ACNHRenderRes"]; }
static int overlay_get(void) { return fps_overlay_hints_enabled(); }
static void overlay_set(int v) { if (overlay_get() != v) fps_overlay_toggle_hints(); }
static const char *const shape_choices[] = {"The screen's shape", "16:9 (the game's own)"};
static const char *const upscale_choices[] = {"Off (720p, the iPad scales it)", "MetalFX at full resolution"};
static const char *const interp_choices[] = {"Off", "30 + 30", "Half the refresh rate", "Unlimited"};
static const char *const motion_choices[] = {"Off", "On"};
static const char *const overlay_choices[] = {"Hidden", "Shown"};
static const gamesettings_choice graphics_settings[] = {
    {"shape", "Picture shape", shape_choices, 2, shape_get, shape_set, "The camera fills the screen without black bars; the HUD keeps its shape in the middle."},
    {"render", "Render resolution", render_choices, 5, render_get, render_set, "The 3D scene's size; the HUD stays sharp. Lower runs faster; MetalFX upscaling sharpens it. Takes effect the next time the game starts."},
    {"upscale", "Upscaling", upscale_choices, 2, upscale_get, upscale_set, "MetalFX scales the 720p picture to the screen's own pixels; uses more GPU time and battery."},
    {"interpolation", "Frame interpolation", interp_choices, 4, interp_get, interp_set, "MetalFX generates in-between frames (half of 120 Hz is 60)."},
    {"motion", "Motion controls", motion_choices, 2, motion_get, motion_set, "Tilt and turn the device (or a controller with a gyro, such as a DualSense) where the game uses motion."},
    {"overlay", "FPS overlay", overlay_choices, 2, overlay_get, overlay_set, NULL},
};
static const gamesettings_config settings_config = {"01006F8002326000", "Animal Crossing: New Horizons", "[acnh]", graphics_settings, sizeof graphics_settings / sizeof *graphics_settings};

int totk_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
void runtime_ios_device_motion_start(UIView *view);
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
    runtime_ios_device_motion_start(self.view);   /* the device's gyro as the controller's (device_motion.m) */
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t thread; pthread_create(&thread, &attr, run_host, NULL);
    pthread_detach(thread); pthread_attr_destroy(&attr);
}
/* A hardware keyboard (Magic Keyboard, Bluetooth) plays with the same key bindings as the Mac (vendor/gamesettings). */
- (BOOL)canBecomeFirstResponder { return YES; }
- (void)viewWillAppear:(BOOL)animated { [super viewWillAppear:animated]; [self becomeFirstResponder]; }
- (NSArray<UIKeyCommand *> *)keyCommands {
    UIKeyCommand *settings = [UIKeyCommand commandWithTitle:@"Settings" image:nil action:@selector(openSettings) input:@"," modifierFlags:UIKeyModifierCommand propertyList:nil];
    return @[settings];
}
- (void)openSettings { gamesettings_show(); }
static BOOL forward_presses(NSSet<UIPress *> *presses, int down) {
    BOOL handled = NO;
    for (UIPress *p in presses) {
        if (!p.key || (p.key.modifierFlags & UIKeyModifierCommand)) continue;
        int code = gamesettings_mac_key_from_hid(p.key.keyCode); if (code < 0) continue;
        runtime_input_key((unsigned)code, down); handled = YES;
    }
    return handled;
}
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event { if (!forward_presses(presses, 1)) [super pressesBegan:presses withEvent:event]; }
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event { if (!forward_presses(presses, 0)) [super pressesEnded:presses withEvent:event]; }
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event { if (!forward_presses(presses, 0)) [super pressesCancelled:presses withEvent:event]; }
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
    { int r = render_get(); setenv("TOTK_RENDER_RES", render_sizes[r], 1); }   /* the Settings sheet's Render resolution */
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
    /* Settings: loaded off the main thread (iCloud may be slow to answer); graphics applied once the game draws. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ gamesettings_start(&settings_config); });
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) { if (dm_nvn_frame_count() < 60) return; [t invalidate]; gamesettings_apply_graphics(); }];
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
