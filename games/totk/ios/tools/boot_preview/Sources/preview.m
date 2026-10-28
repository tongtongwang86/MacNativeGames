/* The boot screen in the Simulator: the game's startup steps in a loop, a tap switches the contrast style (its name shows
   for a moment in the top-left corner). For screenshots: TOTK_BOOT_STYLE=<name>, TOTK_BOOT_STEP=<0-3> holds one step,
   TOTK_BOOT_TWIRL=<speed> (0 holds the circles). */
#import <UIKit/UIKit.h>
#import "boot_view_ios.h"

typedef struct { NSString *stage, *detail; double from, to, seconds; } Step;   // progress from..to over the step; from < 0: none
static const Step kSteps[] = {
    {@"Starting Hyrule", @"Preparing the game environment", -1, -1, 3},
    {@"Verifying game data", nil, 0.0, 1.0, 6},
    {@"Checking the shader cache", @"Verifying the shaders bundled with the app", -1, -1, 3},
    {@"Warming shaders and pipelines", nil, 0.0, 1.0, 5},
};

@interface PreviewController : UIViewController @end
@implementation PreviewController {
    TOTKBootView *_boot; UILabel *_name; NSInteger _step; CFTimeInterval _stepStart; CADisplayLink *_tick; int _hold;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    NSString *folder = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"launch"];
    _boot = [[TOTKBootView alloc] initWithFrame:self.view.bounds patternFolder:folder];
    const char *tw = getenv("TOTK_BOOT_TWIRL"); if (tw) _boot.twirlSpeed = atof(tw);
    [self.view addSubview:_boot];
    _name = [UILabel new]; _name.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightMedium]; _name.textColor = UIColor.whiteColor;
    _name.backgroundColor = [UIColor colorWithWhite:0 alpha:0.6]; _name.alpha = 0; [self.view addSubview:_name];
    const char *hold = getenv("TOTK_BOOT_STEP"); _hold = hold && *hold ? atoi(hold) : -1;
    _step = _hold >= 0 ? _hold : 0; _stepStart = CACurrentMediaTime();
    [self.view addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(nextStyle)]];
    _tick = [CADisplayLink displayLinkWithTarget:self selector:@selector(advance)]; [_tick addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)advance {
    const Step *s = &kSteps[_step % (sizeof kSteps / sizeof *kSteps)];
    double t = (CACurrentMediaTime() - _stepStart) / s->seconds;
    if (_hold >= 0) t = 0.19 / fmax(s->to - s->from, 1e-9);   // a held step shows 19 %
    if (t >= 1 && _hold < 0) { _step++; _stepStart = CACurrentMediaTime(); return; }
    double p = s->from < 0 ? -1 : s->from + (s->to - s->from) * fmin(t, 1);
    NSString *detail = s->detail;
    if (_step % 4 == 1) detail = [NSString stringWithFormat:@"%.1f / 17.1 GB", 17.1 * p];
    if (_step % 4 == 3) detail = [NSString stringWithFormat:@"%d / 33311 items", (int)(33311 * p)];
    NSString *stage = _step % 4 == 3 ? [NSString stringWithFormat:@"%@ %d%%", s->stage, (int)(p * 100)] : s->stage;
    [_boot setStage:stage detail:detail progress:p];
}
- (void)nextStyle {
    _boot.contrast = (TOTKBootContrast)((_boot.contrast + 1) % TOTKBootContrastCount);
    _name.text = [NSString stringWithFormat:@"  %@  ", TOTKBootContrastName(_boot.contrast)]; [_name sizeToFit];
    _name.frame = CGRectMake(self.view.safeAreaInsets.left + 12, 12, _name.bounds.size.width, 24);
    _name.alpha = 1; [UIView animateWithDuration:0.6 delay:1.2 options:0 animations:^{ self->_name.alpha = 0; } completion:nil];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

@interface PreviewScene : UIResponder <UIWindowSceneDelegate> @property(nonatomic, strong) UIWindow *window; @end
@implementation PreviewScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [PreviewController new]; [self.window makeKeyAndVisible];
}
@end

@interface PreviewApp : UIResponder <UIApplicationDelegate> @end
@implementation PreviewApp
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options { (void)application; (void)options; return YES; }
@end

int main(int argc, char **argv) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(PreviewApp.class)); } }
