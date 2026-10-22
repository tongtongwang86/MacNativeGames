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

/* The Settings sheet's graphics on iPhone / iPad (vendor/gamesettings): the picture shape, MetalFX, the FPS overlay, and
   motion controls (the device's own gyro, ios/Sources/device_motion.m, or a controller's). */
static int shape_get(void) { return runtime_aspect_on() ? 0 : 1; }
static void shape_set(int v) { if (shape_get() != v) runtime_aspect_toggle(); }
static int upscale_get(void) { return runtime_ios_full_resolution(); }
static void upscale_set(int v) { runtime_ios_set_full_resolution(v); }
int mfx_enabled(void);
void dm_gfx_set_flag(const char *name, int v);
static int interp_get(void) { return mfx_enabled(); }
static void interp_set(int v) { dm_gfx_set_flag("mfx_interp", v); }
static int motion_get(void) { return runtime_device_motion_enabled(); }
static void motion_set(int v) { runtime_hid_feedback_flag("gyro", !!v); runtime_device_motion_enable(v); }   /* off: the game sees a pad lying still */
/* Render resolution: the 3D scene's size (the UI keeps its own), read once at start (TOTK_RENDER_RES, before the game
   thread), so a change takes effect at the next start; kept in NSUserDefaults for that and in the Settings like the rest. */
static const char *const render_sizes[] = {"854x480", "960x540", "1280x720", "1600x900", "1920x1080"};
static const char *const render_choices[] = {"480p", "540p", "720p (the game's)", "900p", "1080p"};
static int render_get(void) {
    NSString *v = [NSUserDefaults.standardUserDefaults stringForKey:@"TOTKRenderRes"];
    for (int i = 0; i < 5; i++) if ([v isEqualToString:@(render_sizes[i])]) return i;
    return 2;
}
static void render_set(int v) { if (v >= 0 && v < 5) [NSUserDefaults.standardUserDefaults setObject:@(render_sizes[v]) forKey:@"TOTKRenderRes"]; }
static int overlay_get(void) { return fps_overlay_hints_enabled(); }
static void overlay_set(int v) { if (overlay_get() != v) fps_overlay_toggle_hints(); }
static const char *const shape_choices[] = {"The screen's shape", "16:9 (the game's own)"};
static const char *const overlay_choices[] = {"Hidden", "Shown"};
static const char *const upscale_choices[] = {"Off (720p, the iPad scales it)", "MetalFX at full resolution"};
static const char *const interp_choices[] = {"Off", "30 + 30", "Half the refresh rate", "Unlimited"};
static const char *const motion_choices[] = {"Off", "On"};
static const gamesettings_choice graphics_settings[] = {
    {"shape", "Picture shape", shape_choices, 2, shape_get, shape_set, "The camera fills the screen without black bars; the HUD keeps its shape in the middle."},
    {"render", "Render resolution", render_choices, 5, render_get, render_set, "The 3D scene's size; the HUD stays sharp. Lower runs faster; MetalFX upscaling sharpens it. 900p and 1080p run the game in its docked mode (more detail, more GPU and battery). Takes effect the next time the game starts."},
    {"upscale", "Upscaling", upscale_choices, 2, upscale_get, upscale_set, "MetalFX scales the 720p picture to the screen's own pixels. New on iPad: uses more GPU time and battery."},
    {"interpolation", "Frame interpolation", interp_choices, 4, interp_get, interp_set, "MetalFX generates in-between frames (half of 120 Hz is 60). New on iPad."},
    {"motion", "Motion controls", motion_choices, 2, motion_get, motion_set, "Tilt and turn the device (or a controller with a gyro, such as a DualSense) to aim, like the Switch's motion controls. Which actions use motion is set in the game's own Options."},
    {"overlay", "FPS overlay", overlay_choices, 2, overlay_get, overlay_set, NULL},
};
static const gamesettings_config settings_config = {"0100F2C0115B6000", "Tears of the Kingdom", "[totk]", graphics_settings, sizeof graphics_settings / sizeof *graphics_settings};

int totk_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
void runtime_ios_device_motion_start(UIView *view);
static char *host_argv[3];
static void *run_host(void *unused) { (void)unused; return (void *)(intptr_t)totk_host_main(3, host_argv); }
/* The desktop keyboard-overlay shortcut has no counterpart on iOS. */
void runtime_shortcut_help_toggle(void) {}

@interface TOTKMetalView : UIView @end
@implementation TOTKMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

@interface TOTKGameController : UIViewController @property(nonatomic,strong) TOTKMetalView *gameView; @end
@implementation TOTKGameController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.gameView = [[TOTKMetalView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:self.gameView];
    runtime_ios_attach_layer((CAMetalLayer *)self.gameView.layer, self.gameView);
    TOTKTouchPad *pad=[[TOTKTouchPad alloc] initWithFrame:self.view.bounds];pad.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:pad];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started; if (started) return; started = YES;
    runtime_ios_device_motion_start(self.view);
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

@interface TOTKApp : UIResponder <UIApplicationDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation TOTKApp
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
    setenv("TOTK_ROMFS",[res stringByAppendingPathComponent:@"romfs"].fileSystemRepresentation,1);
    /* The Settings sheet's Render resolution. The game sizes its handheld render buffers for 720p: a larger scene size in
       handheld mode crashed it the moment a save loaded (null read at main+0x1c2e40, 2026-10-21; 900p and 1080p crash,
       480p and 720p do not, reproduced on the Mac). Above 720p the game is told it is docked, whose buffers fit 1080p
       (checked), as a docked Switch would; it then also uses its docked graphics settings. */
    { int r = render_get(); setenv("TOTK_RENDER_RES", render_sizes[r], 1); setenv("TOTK_OPERATION_MODE", r > 2 ? "docked" : "handheld", 1); }
    setenv("TOTK_ROMFS_PATCHES",[res stringByAppendingPathComponent:@"romfs-patches"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PATCHES_OUT",[res stringByAppendingPathComponent:@"romfs-patches-out"].fileSystemRepresentation,1);
    setenv("TOTK_SHARED_FONTS",[res stringByAppendingPathComponent:@"shared-fonts"].fileSystemRepresentation,1);
    setenv("TOTK_SHADER_SEED_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    setenv("TOTK_SHADER_CACHE",[docs stringByAppendingPathComponent:@"shader-cache"].fileSystemRepresentation,1);
    // Shaders the shared list lacks (totk_shader_contrib.h), in the Files app for players who want to send them; only the Mac asks.
    setenv("TOTK_SHADER_CONTRIB",[docs stringByAppendingPathComponent:@"shader-list-contribution.txt"].fileSystemRepresentation,1);
    setenv("DM_NVN_SHMAP",[res stringByAppendingPathComponent:@"shader-seed/shader_map.tsv"].fileSystemRepresentation,1);
    setenv("DM_MSL_DIR",[res stringByAppendingPathComponent:@"shader-seed"].fileSystemRepresentation,1);
    NSString *font = [res stringByAppendingPathComponent:@"font"]; setenv("TOTK_FONT_DIR",font.fileSystemRepresentation,1);
    // Same player environment as the Mac launcher (app/launcher.m); the bundle is read-only, so state goes to Documents.
    setenv("TOTK_PROFILE_DIR",[docs stringByAppendingPathComponent:@"save"].fileSystemRepresentation,1);
    setenv("TOTK_GAME_SLOT_DIR",[docs stringByAppendingPathComponent:@"game-slot"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PROOF_DIR",docs.fileSystemRepresentation,1);   // hash the 17 GB RomFS once, not on every launch
    for (NSString *name in @[@"TOTK_GPU_ALLOW_SUBMIT",@"TOTK_GPU_WIDEN",@"TOTK_GPU_BLOCKRT",@"TOTK_GPU_MIDCLEAR"]) setenv(name.UTF8String,"1",1);
    setenv("TOTK_SLOT_OWNER","user",0); setenv("TOTK_SLOT_PURPOSE","player",0);
    unsetenv("TOTK_AUTOLOAD");
    chdir(docs.fileSystemRepresentation);
    host_argv[0] = strdup("TOTK"); host_argv[1] = strdup(res.fileSystemRepresentation); host_argv[2] = strdup("boot");
    /* Settings: loaded off the main thread (iCloud may be slow to answer); graphics applied once the game draws. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ gamesettings_start(&settings_config); });
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *t) { if (dm_nvn_frame_count() < 60) return; [t invalidate]; gamesettings_apply_graphics(); }];
    return YES;
}
@end

/* The window comes with the scene: iOS 27 stops apps built with its SDK that do not adopt the UIScene life cycle
   (UIApplicationSceneManifest in project.yml names this class). */
@interface TOTKScene : UIResponder <UIWindowSceneDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation TOTKScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [TOTKGameController new]; [self.window makeKeyAndVisible];
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

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(TOTKApp.class)); } }
