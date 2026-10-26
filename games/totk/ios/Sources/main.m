#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#import <QuartzCore/CAMetalLayer.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include "runtime.h"
#include "dmsh_chain.h"
#import "touch_pad.h"
#include "../../vendor/savesync/savesync.h"
#include "../../vendor/gamesettings/gamesettings.h"
int fps_overlay_hints_enabled(void);
int fps_overlay_toggle_hints(void);
uint64_t dm_nvn_frame_count(void);

/* The Settings sheet's graphics on iPhone / iPad (vendor/gamesettings): the picture shape, MetalFX, the FPS overlay, and
   motion controls (the device's own gyro, ios/Sources/device_motion.m, or a controller's). */
/* Picture shape: the phone fills its screen unless TOTK_ASPECT=0 (aspect_patch.h), which is read once at start, so the
   choice is kept in NSUserDefaults and takes effect at the next start. */
static int shape_get(void) { return [NSUserDefaults.standardUserDefaults boolForKey:@"TOTKShape169"] ? 1 : 0; }
static void shape_set(int v) { [NSUserDefaults.standardUserDefaults setBool:v == 1 forKey:@"TOTKShape169"]; }
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
/* Console mode: what the game is told it is, read once at start like the render size. Automatic: handheld up to 720p,
   docked above (handheld cannot render larger, see the start). Handheld keeps the render size at 720p or lower. */
static const char *const console_values[] = {"auto", "handheld", "docked"};
static const char *const console_choices[] = {"Automatic", "Handheld", "Docked"};
static int console_get(void) {
    NSString *v = [NSUserDefaults.standardUserDefaults stringForKey:@"TOTKOperationMode"];
    for (int i = 0; i < 3; i++) if ([v isEqualToString:@(console_values[i])]) return i;
    return 0;
}
static void console_set(int v) { if (v >= 0 && v < 3) [NSUserDefaults.standardUserDefaults setObject:@(console_values[v]) forKey:@"TOTKOperationMode"]; }
static int overlay_get(void) { return fps_overlay_hints_enabled(); }
static void overlay_set(int v) { if (overlay_get() != v) fps_overlay_toggle_hints(); }
static const char *const shape_choices[] = {"The screen's shape", "16:9 (the game's own)"};
static const char *const overlay_choices[] = {"Hidden", "Shown"};
static const char *const upscale_choices[] = {"Off (720p, or 1080p docked; the system scales it)", "MetalFX at full resolution"};
static const char *const interp_choices[] = {"Off", "30 + 30", "Half the refresh rate", "Unlimited"};
static const char *const motion_choices[] = {"Off", "On"};
/* Not const: the console mode's and render resolution's ids are this device type's own (per_device_ids). */
static gamesettings_choice graphics_settings[] = {
    {"shape", "Picture shape", shape_choices, 2, shape_get, shape_set, "The camera fills the screen without black bars; the HUD keeps its shape in the middle. Takes effect the next time the game starts."},
    {"console_ios", "Console mode", console_choices, 3, console_get, console_set, "Docked is the game's TV mode: more detail, more GPU and battery. Handheld keeps the render resolution at 720p or lower. Automatic: handheld up to 720p, docked above. This device's own setting. Takes effect the next time the game starts."},
    {"render", "Render resolution", render_choices, 5, render_get, render_set, "The 3D scene's size; the HUD stays sharp. Lower runs faster; MetalFX upscaling sharpens it. 900p and 1080p run the game in its docked mode (more detail, more GPU and battery). This device's own setting. Takes effect the next time the game starts."},
    {"upscale", "Upscaling", upscale_choices, 2, upscale_get, upscale_set, "MetalFX scales the 720p picture to the screen's own pixels. New on iPad: uses more GPU time and battery."},
    {"interpolation", "Frame interpolation", interp_choices, 4, interp_get, interp_set, "MetalFX generates in-between frames (half of 120 Hz is 60). New on iPad."},
    {"motion", "Motion controls", motion_choices, 2, motion_get, motion_set, "Tilt and turn the device (or a controller with a gyro, such as a DualSense) to aim, like the Switch's motion controls. Which actions use motion is set in the game's own Options."},
    {"overlay", "FPS overlay", overlay_choices, 2, overlay_get, overlay_set, NULL},
};
/* Console mode and render resolution are read at start from NSUserDefaults, before the settings are loaded (they are applied
   after 60 frames). Shared through iCloud they came a start late, and an iPhone and an iPad overwrote each other's size;
   with an id per device type each keeps its own, and NSUserDefaults already holds it at start. */
static void per_device_ids(void) {
    int pad = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad;
    for (unsigned i = 0; i < sizeof graphics_settings / sizeof *graphics_settings; i++) {
        if (!strcmp(graphics_settings[i].id, "render")) graphics_settings[i].id = pad ? "render_ipad" : "render_iphone";
        else if (!strcmp(graphics_settings[i].id, "console_ios")) graphics_settings[i].id = pad ? "console_ipad" : "console_iphone";
    }
}
static const gamesettings_config settings_config = {"0100F2C0115B6000", "Tears of the Kingdom", "[totk]", graphics_settings, sizeof graphics_settings / sizeof *graphics_settings};

int totk_host_main(int argc, char **argv);
void runtime_ios_attach_layer(CAMetalLayer *layer, UIView *host);
void runtime_ios_sync_drawable(UIView *host);
void runtime_ios_sync_safe_area(UIView *host);
void runtime_ios_device_motion_start(UIView *view);
static char *host_argv[3];
static NSString *g_no_game_data;   // set when there is no RomFS image to play from (didFinishLaunching)
static NSString *g_seed_pack, *g_seed_unpack_to;   // a pushed shader pack to unpack before the game starts (seed_unpack)

/* The startup shaders arrive as one stream cut into 1 GiB parts (<folder>.pack.000, .001, ...; ios/tools/install_device.py
   push_seed): devicectl stops after ~25,000 of the seed's 128,000 files when it copies a folder (2026-10-23). Stream:
   "TOTKPACK1", then per file a little-endian u16 name length, the name, a u64 size and the bytes; a zero name length
   ends it. Unpacked into <folder>.tmp, renamed to <folder> when complete, the parts removed. Returns nil or the reason. */
static NSString *seed_unpack(NSString *base, NSString *dest, void (^progress)(uint64_t done, uint64_t total)) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray<NSString *> *parts = [NSMutableArray array]; uint64_t total = 0;
    for (int i = 0;; i++) {
        NSString *p = [base stringByAppendingFormat:@".%03d", i];
        NSDictionary *a = [fm attributesOfItemAtPath:p error:nil]; if (!a) break;
        [parts addObject:p]; total += a.fileSize;
    }
    if (!parts.count) return @"no pack";
    __block NSUInteger at = 0; __block FILE *f = fopen(parts[0].fileSystemRepresentation, "rb"); __block uint64_t done = 0;
    BOOL (^take)(void *, size_t) = ^BOOL(void *out, size_t n) {   // n bytes of the stream, across part boundaries
        uint8_t *o = out;
        while (n) {
            if (!f) return NO;
            size_t got = fread(o, 1, n, f); o += got; n -= got; done += got;
            if (n) { fclose(f); f = ++at < parts.count ? fopen(parts[at].fileSystemRepresentation, "rb") : NULL; }
        }
        return YES;
    };
    NSString *tmp = [dest stringByAppendingString:@".tmp"]; [fm removeItemAtPath:tmp error:nil];
    if (![fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil]) { if (f) fclose(f); return @"cannot make the folder"; }
    char magic[9]; NSString *why = nil; size_t cap = 1 << 20; uint8_t *buf = malloc(cap); uint64_t files = 0, last = 0;
    if (!take(magic, 9) || memcmp(magic, "TOTKPACK1", 9)) why = @"not a shader pack";
    while (!why) {
        uint16_t nl; if (!take(&nl, 2)) { why = @"truncated"; break; }
        if (!nl) break;
        char name[256]; uint64_t size;
        if (nl >= sizeof name || !take(name, nl)) { why = @"bad name"; break; }
        name[nl] = 0;
        if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, "..") || !take(&size, 8)) { why = @"bad entry"; break; }
        FILE *o = fopen([tmp stringByAppendingPathComponent:@(name)].fileSystemRepresentation, "wb"); if (!o) { why = @"cannot write"; break; }
        while (size && !why) { size_t n = size < cap ? (size_t)size : cap; if (!take(buf, n) || fwrite(buf, 1, n, o) != n) why = @"truncated or disk full"; size -= n; }
        fclose(o); files++;
        if (progress && done - last > (32u << 20)) { last = done; progress(done, total); }
    }
    free(buf); if (f) fclose(f);
    if (why) { [fm removeItemAtPath:tmp error:nil]; return why; }
    [fm removeItemAtPath:dest error:nil];
    if (![fm moveItemAtPath:tmp toPath:dest error:nil]) return @"cannot rename";
    for (NSString *p in parts) [fm removeItemAtPath:p error:nil];
    fprintf(stderr, "[totk] shader pack unpacked: %llu files, %.0f MB\n", (unsigned long long)files, total / 1048576.0);
    return nil;
}
static void *run_host(void *unused) { (void)unused; return (void *)(intptr_t)totk_host_main(3, host_argv); }
/* Key 1's settings panel is Mac only (runtime/frontend.m). */
void runtime_settings_toggle(void) {}

@interface TOTKMetalView : UIView @end
@implementation TOTKMetalView
+ (Class)layerClass { return CAMetalLayer.class; }
#if TARGET_OS_VISION
- (void)layoutSubviews { [super layoutSubviews]; runtime_ios_sync_drawable(self); }
#endif
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; runtime_ios_sync_safe_area(self); }   // the HUD boxes keep out of the notch
@end

@interface TOTKGameController : UIViewController @property(nonatomic,strong) TOTKMetalView *gameView; @end
@implementation TOTKGameController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;
    self.gameView = [[TOTKMetalView alloc] initWithFrame:self.view.bounds];
    self.gameView.autoresizingMask = UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
#if TARGET_OS_VISION
    // Keep gamepad actions in GameController instead of visionOS converting them to pinch events.
    GCEventInteraction *controllerInput = [GCEventInteraction new];
    controllerInput.handledEventTypes = GCUIEventTypeGamepad;
    [self.gameView addInteraction:controllerInput];
#endif
    [self.view addSubview:self.gameView];
    runtime_ios_attach_layer((CAMetalLayer *)self.gameView.layer, self.gameView);
    TOTKTouchPad *pad=[[TOTKTouchPad alloc] initWithFrame:self.view.bounds];pad.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;[self.view addSubview:pad];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    static BOOL started; if (started) return; started = YES;
    runtime_ios_device_motion_start(self.view);
    /* What happens before the game starts is said on the boot screen (runtime/frontend_ios.m), not on a label of its own:
       one laid over it showed both texts on top of each other (2026-10-23). */
    BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];   // the same rule as the boot screen (totk_ui_lang.h)
    if (g_no_game_data) {
        runtime_boot_status(zh ? "无法开始游戏" : "Cannot start the game", g_no_game_data.UTF8String);
        return;
    }
    if (g_seed_pack) {   // shaders installed as a pack: unpack once, on screen, then start (not on the main thread: a long launch is killed)
        static const char *stage_zh = "正在准备着色器（更新后只做一次）", *stage_en = "Preparing the shaders (once after an update)";
        const char *stage = zh ? stage_zh : stage_en;
        runtime_boot_progress(stage, 0, 0);
        NSString *pack = g_seed_pack, *to = g_seed_unpack_to; g_seed_pack = nil;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
            NSString *why = seed_unpack(pack, to, ^(uint64_t done, uint64_t total) { runtime_boot_progress(stage, done, total); });
            fprintf(stderr, "[totk] shader pack: %s in %.1f s\n", why ? why.UTF8String : "ready", CFAbsoluteTimeGetCurrent() - t0);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (why) { runtime_boot_status(zh ? "着色器准备失败" : "Could not prepare the shaders", (zh
                               ? [NSString stringWithFormat:@"%@。请在 Mac 上用 ios/tools/install_device.py 重新安装。", why]
                               : [NSString stringWithFormat:@"%@. Install again from the Mac with ios/tools/install_device.py.", why]).UTF8String); return; }
                [self startGame];
            });
        });
        return;
    }
    [self startGame];
}
- (void)startGame {
    /* Playback category (PR #97): plays with the ring / silent switch off and routes to the speaker. Without an active
       session the game's CoreAudio output stays on a null sink on iOS 27 ("no device sink in use"); activating it in
       didFinishLaunching blocked the main thread before the log file was opened. Interruptions: audio_observe. */
    AVAudioSession *audio = AVAudioSession.sharedInstance;
    NSError *catErr = nil, *actErr = nil;
    [audio setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&catErr];
    BOOL activated = [audio setActive:YES error:&actErr];
    fprintf(stderr, "[totk-audio] audio session category=%s active=%d route=%s err=%s%s\n",
            audio.category.UTF8String ?: "?", (int)activated, audio.currentRoute.outputs.firstObject.portType.UTF8String ?: "?",
            catErr ? catErr.localizedDescription.UTF8String : "none", actErr ? actErr.localizedDescription.UTF8String : "");
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

/* iOS 27 ends apps linked against its SDK that keep the window in the app delegate (UIKit traps in
   _UIApplicationEvaluateRuntimeIssueForNoSceneLifecycleAdoption at launch), so the window belongs to a window scene. */
@interface TOTKSceneDelegate : UIResponder <UIWindowSceneDelegate> @property(nonatomic,strong) UIWindow *window; @end
@implementation TOTKSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [TOTKGameController new]; [self.window makeKeyAndVisible];
#if TARGET_OS_VISION
    UIWindowSceneGeometryPreferencesVision *prefs = [[UIWindowSceneGeometryPreferencesVision alloc] initWithSize:CGSizeMake(1280, 720)];
    prefs.resizingRestrictions = UIWindowSceneResizingRestrictionsUniform;
    [(UIWindowScene *)scene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError *error) {
        fprintf(stderr, "[totk] 16:9 window refused: %s\n", error.localizedDescription.UTF8String);
    }];
#endif
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

/* Game audio after interruptions (from nxnative b301431). A call, Siri or an alarm takes the audio session and the
   system stops the game's output unit; a media services reset invalidates it. Nothing started it again, so the game
   stayed silent. When an interruption ends the session is made active again and the output started
   (runtime_audio_output_restart); a reset rebuilds the output. Becoming active does the same, for an interruption
   whose "ended" never came. Becoming active also turns the idle timer off again. */
static BOOL g_audio_interrupted;
static void audio_resume(int recreate, const char *why) {
    NSError *err = nil;
    if (![AVAudioSession.sharedInstance setActive:YES error:&err]) fprintf(stderr,"[totk-audio] audio session not active again (%s): %s\n",why,err.localizedDescription.UTF8String);
    g_audio_interrupted = NO; runtime_audio_output_restart(recreate);
}
static void audio_observe(void) {
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter; NSOperationQueue *main = NSOperationQueue.mainQueue;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        NSUInteger type = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        fprintf(stderr,"[totk-audio] audio session interruption %s\n",type == AVAudioSessionInterruptionTypeBegan ? "began" : "ended");
        if (type == AVAudioSessionInterruptionTypeBegan) g_audio_interrupted = YES; else audio_resume(0,"interruption ended");
    }];
    [nc addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; fprintf(stderr,"[totk-audio] media services were reset\n"); audio_resume(1,"media services reset");
    }];
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:main usingBlock:^(NSNotification *n) {
        (void)n; UIApplication.sharedApplication.idleTimerDisabled = YES;
        if (g_audio_interrupted) audio_resume(0,"became active during an interruption");
    }];
}

@interface TOTKApp : UIResponder <UIApplicationDelegate> @end
@implementation TOTKApp
- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)application; (void)options;
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = TOTKSceneDelegate.class; return c;
}
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)options;
    // Controller input does not reset the idle timer; auto-lock would suspend the game mid-play or mid shader import.
    application.idleTimerDisabled = YES;
    audio_observe();
    /* The Mac launcher passes the preferred language as TOTK_LOCALE (PR #97); "zh-Hans-CN" maps to zh-Hans in services.c. */
    NSString *preferred = NSLocale.preferredLanguages.firstObject;
    if (preferred.length) setenv("TOTK_LOCALE", preferred.UTF8String, 1);
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData"];
    for (NSString *name in @[@"save", @"temp", @"shader-cache", @"game-slot"]) [fm createDirectoryAtPath:[docs stringByAppendingPathComponent:name] withIntermediateDirectories:YES attributes:nil error:nil];
    /* The log is written only with debug on: the Settings app (Settings.bundle › TOTK_DEBUG_LOG) or TOTK_DEBUG_LOG=1 from
       devicectl (it wins over the setting). Off, stderr goes to /dev/null fully buffered, so nothing reaches the phone's
       storage (a line per write had stalled the game in bursts), and an old log.txt is removed. */
    NSString *log = [docs stringByAppendingPathComponent:@"log.txt"];
    const char *debugEnv = getenv("TOTK_DEBUG_LOG");
    BOOL debugLog = debugEnv ? (*debugEnv && strcmp(debugEnv,"0")) : [NSUserDefaults.standardUserDefaults boolForKey:@"TOTK_DEBUG_LOG"];
    if (debugLog) { if (freopen(log.fileSystemRepresentation,"w",stderr)) setvbuf(stderr,NULL,_IOLBF,0); }
    else { [fm removeItemAtPath:log error:nil]; if (freopen("/dev/null","w",stderr)) setvbuf(stderr,NULL,_IOFBF,1<<16); }
    setenv("TOTK_ROMFS",[res stringByAppendingPathComponent:@"romfs"].fileSystemRepresentation,1);
    /* The 17 GB image may live in Documents/GameData instead of the app (romfs.bin or romfs.bin.000, ...; ios/tools/
       install_device.py --game-data puts it there): a new build then installs without it, and reinstalling keeps it.
       Excluded from iCloud backups. The runtime still hashes it once against the build's pin (TOTK_ROMFS_PROOF_DIR). */
    NSString *gameData = [docs stringByAppendingPathComponent:@"GameData"];
    BOOL image = [fm fileExistsAtPath:[gameData stringByAppendingPathComponent:@"romfs.bin"]] ||
                 [fm fileExistsAtPath:[gameData stringByAppendingPathComponent:@"romfs.bin.000"]];
    NSString *mark = [NSString stringWithContentsOfFile:[gameData stringByAppendingPathComponent:@"romfs.sha256"] encoding:NSUTF8StringEncoding error:nil];
    BOOL copying = mark && [mark stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length != 64;   // install_device.py writes the hash last
    if (image && !copying) {
        [[NSURL fileURLWithPath:gameData isDirectory:YES] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
        setenv("TOTK_ROMFS_DATA",gameData.fileSystemRepresentation,1);
    } else if (![fm fileExistsAtPath:[res stringByAppendingPathComponent:@"romfs/romfs.bin"]]) {
        /* No image to play from: said on the screen instead of the game ending at once (runtime_fail exits). */
        BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];
        NSString *app = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleDisplayName"] ?: @"TOTK";   // the Files app's folder name
        g_no_game_data = copying || image
            ? [NSString stringWithFormat:(zh ? @"游戏数据还在复制到“文件 › 我的 iPhone › %@ › GameData”，复制完再打开游戏。"
                                             : @"The game data is still being copied to Files › On My iPhone › %@ › GameData. Open the game again when it is done."), app]
            : [NSString stringWithFormat:(zh ? @"没有游戏数据。请在 Mac 上用 ios/tools/install_device.py --game-data 把它复制到“文件 › 我的 iPhone › %@ › GameData”。"
                                             : @"No game data. On the Mac, copy it to Files › On My iPhone › %@ › GameData with ios/tools/install_device.py --game-data."), app];
        fprintf(stderr, "[totk] no game image: %s\n", g_no_game_data.UTF8String);
    }
    /* The startup shaders likewise (build_app.py --ios-game-data documents): install_device.py puts the seed into its own
       folder in Documents/GameData once per content and writes shader-seed.txt last ("<folder>\n<fingerprint>"), so the app
       carries none and a new build installs in seconds. Read in place like the app's own (totk_shader_seed.h); folders of
       earlier seeds are removed in the background. Without either seed the game has no shaders: said on the screen. */
    NSString *seedDir = [res stringByAppendingPathComponent:@"shader-seed"];
    NSArray<NSString *> *seedMark = [[NSString stringWithContentsOfFile:[gameData stringByAppendingPathComponent:@"shader-seed.txt"] encoding:NSUTF8StringEncoding error:nil] componentsSeparatedByString:@"\n"];
    NSString *pushedName = seedMark.count >= 2 && seedMark[1].length == 64 && [seedMark[0] hasPrefix:@"shader-seed-"] && ![seedMark[0] containsString:@"/"] ? seedMark[0] : nil;
    NSString *pushed = pushedName ? [gameData stringByAppendingPathComponent:pushedName] : nil;
    BOOL packed = pushed && ![fm fileExistsAtPath:[pushed stringByAppendingPathComponent:@"seed-manifest.json"]] &&
                  [fm fileExistsAtPath:[pushed stringByAppendingString:@".pack.000"]];
    if (packed) { g_seed_pack = [pushed stringByAppendingString:@".pack"]; g_seed_unpack_to = pushed; }   // unpacked in viewDidAppear
    if (pushed && (packed || [fm fileExistsAtPath:[pushed stringByAppendingPathComponent:@"seed-manifest.json"]])) {
        seedDir = pushed; setenv("TOTK_SHADER_SEED_PUSHED", pushed.fileSystemRepresentation, 1);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
            NSFileManager *files = [NSFileManager new];
            for (NSString *name in [files contentsOfDirectoryAtPath:gameData error:nil] ?: @[])
                if ([name hasPrefix:@"shader-seed-"] && ![name hasPrefix:pushedName]) [files removeItemAtPath:[gameData stringByAppendingPathComponent:name] error:nil];
        });
    } else if (!g_no_game_data && ![fm fileExistsAtPath:[seedDir stringByAppendingPathComponent:@"seed-manifest.json"]]) {
        BOOL zh = [NSLocale.preferredLanguages.firstObject hasPrefix:@"zh"];
        g_no_game_data = zh ? @"没有着色器。请在 Mac 上用 ios/tools/install_device.py 重新安装（会把着色器复制到手机上）。"
                            : @"No shaders. Install the game again from the Mac with ios/tools/install_device.py (it copies them to the device).";
        fprintf(stderr, "[totk] no shader seed: %s\n", g_no_game_data.UTF8String);
    }
#if TARGET_OS_VISION
    setenv("TOTK_OPERATION_MODE","docked",1);
    setenv("TOTK_IOS_DRAWABLE_H","1080",0);
    setenv("TOTK_RENDER_RES","1600x900",0);   // avoid the iPhone's default 960x540 in 1.4.3
    setenv("TOTK_MFX_UPSCALE","0",0);
    setenv("TOTK_FPS_OVERLAY","0",0);
#else
    /* The Settings sheet's Render resolution. The game sizes its handheld render buffers for 720p: a larger scene size in
       handheld mode crashed it the moment a save loaded (null read at main+0x1c2e40, 2026-10-21; 900p and 1080p crash,
       480p and 720p do not, reproduced on the Mac). Above 720p the game is told it is docked, whose buffers fit 1080p
       (checked), as a docked Switch would; it then also uses its docked graphics settings. Docked, the game composes a
       1920x1080 canvas: the drawable is then 1080 rows instead of 720 (frontend_ios.m), or 900p / 1080p are squeezed back
       into 720 rows and look no sharper than 720p. */
    per_device_ids();
    { int r = render_get(), c = console_get(); if (c == 1 && r > 2) r = 2;
      int docked = c == 2 || (c == 0 && r > 2);
      setenv("TOTK_RENDER_RES", render_sizes[r], 1); setenv("TOTK_OPERATION_MODE", docked ? "docked" : "handheld", 1);
      if (docked) setenv("TOTK_IOS_DRAWABLE_H", "1080", 0);
      fprintf(stderr, "[totk] console mode %s (%s), render %s\n", docked ? "docked" : "handheld", console_values[c], render_sizes[r]); }
    if (shape_get() == 1) setenv("TOTK_ASPECT", "0", 0);   // the Settings sheet's 16:9
#endif
    // Game language: the Settings app (Settings.bundle › TOTK_LANG) stores the choice in our defaults; empty means the
    // system language, which is what the runtime already follows. iOS 27 does not honour Info.plist LSEnvironment, so
    // the value must be applied here. overwrite=0 keeps devicectl --environment-variables winning when testing from the Mac.
    NSString *lang = [NSUserDefaults.standardUserDefaults stringForKey:@"TOTK_LANG"];
    if (lang.length>0&&lang.length<8) setenv("TOTK_LANG",lang.UTF8String,0);
    setenv("TOTK_ROMFS_PATCHES",[res stringByAppendingPathComponent:@"romfs-patches"].fileSystemRepresentation,1);
    setenv("TOTK_ROMFS_PATCHES_OUT",[res stringByAppendingPathComponent:@"romfs-patches-out"].fileSystemRepresentation,1);
    setenv("TOTK_SHARED_FONTS",[res stringByAppendingPathComponent:@"shared-fonts"].fileSystemRepresentation,1);
    NSString *aoc = [res stringByAppendingPathComponent:@"aoc"];   // Breath of the Wild's add-on content packs (build_ipa.py; runtime/aoc_romfs.c)
    if ([fm fileExistsAtPath:aoc]) setenv("TOTK_AOC",aoc.fileSystemRepresentation,1);
    setenv("TOTK_SHADER_SEED_DIR",seedDir.fileSystemRepresentation,1);
    /* One folder per translation chain, as on the Mac: the runtime refuses translations of another chain, and the seed
       import keeps files that are already there. Builds before the v5 chain (2026-10-20) kept them directly in
       shader-cache; those, and other chains' folders, are only cache and are removed in the background. */
    NSString *cacheRoot = [docs stringByAppendingPathComponent:@"shader-cache"], *chainDir = @"rt-" DMSH_CHAIN;
    NSString *cache = [cacheRoot stringByAppendingPathComponent:chainDir];
    /* Test launches (devicectl --environment-variables '{"TOTK_SHADER_CACHE":"cache-test/1"}') can name another folder
       in Documents, e.g. an empty one to time a first start; it lies outside shader-cache, so the cleanup leaves it. */
    const char *testCache = getenv("TOTK_SHADER_CACHE");
    if (testCache && *testCache && !strstr(testCache, "..")) cache = [docs stringByAppendingPathComponent:@(testCache)];
    [fm createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:nil];
    setenv("TOTK_SHADER_CACHE",cache.fileSystemRepresentation,1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
        NSFileManager *files = [NSFileManager new]; NSUInteger removed = 0;
        for (NSString *name in [files contentsOfDirectoryAtPath:cacheRoot error:nil] ?: @[]) {
            if ([name isEqualToString:chainDir]) continue;
            if ([files removeItemAtPath:[cacheRoot stringByAppendingPathComponent:name] error:nil]) removed++;
        }
        if (removed) fprintf(stderr,"[shader-cache] removed %lu entries of earlier translation chains\n",(unsigned long)removed);
    });
    // Shaders the shared list lacks (totk_shader_contrib.h), in the Files app for players who want to send them; only the Mac asks.
    setenv("TOTK_SHADER_CONTRIB",[docs stringByAppendingPathComponent:@"shader-list-contribution.txt"].fileSystemRepresentation,1);
    setenv("DM_NVN_SHMAP",[seedDir stringByAppendingPathComponent:@"shader_map.tsv"].fileSystemRepresentation,1);
    setenv("DM_MSL_DIR",seedDir.fileSystemRepresentation,1);
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
    return YES;   // the window is made when the window scene connects (TOTKSceneDelegate)
}
@end

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(TOTKApp.class)); } }
