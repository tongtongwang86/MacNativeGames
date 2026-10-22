/* The iPhone / iPad's own motion sensors as the game's six-axis sensor (2026-10-20), for motion aiming without a
   controller. The device is held in landscape, upright and facing the player, like a Switch in handheld mode; it is
   presented to the game as a controller held level in front of the player, the pose the game's motion aiming expects:
   the screen's right is the controller's x, the direction away from the player its y (the top edge), the screen's up its
   z (out of the face). So the device at rest, upright, reads like a controller lying face up (z = -1 G), turning it left
   and right turns the controller about its z, tilting it toward or away from you pitches it about x.
   CoreMotion's device motion (100 samples a second, rotation rate with the gyro's bias taken off, gravity + the user's
   acceleration in G) is in the portrait frame (x right, y toward the top, z out of the screen), turned here to the
   landscape screen by the interface orientation (read on the main queue twice a second), then fed to
   runtime/motion_fusion.c like a controller's samples; TOTK_GYRO_AXES still remaps after this.
   Paused while a controller gives motion (a DualSense, runtime/gamepad_feedback.m), while the app is inactive, and while
   motion controls are off in the Settings sheet (Motion controls). */
#import <CoreMotion/CoreMotion.h>
#import <UIKit/UIKit.h>
#include <stdatomic.h>
#include <stdio.h>
#include "runtime.h"

static CMMotionManager *manager;     /* main queue */
static NSOperationQueue *queue;
static __weak UIView *host;          /* main queue: the game view, for the interface orientation */
static BOOL controller, active = YES, enabled = YES, running;   /* main queue */
static _Atomic int orientation = UIInterfaceOrientationLandscapeRight, feeding;

/* Portrait device axes -> the controller the game sees (see the top). */
static void to_controller(const double v[3], double out[3]) {
    double right, up;
    switch (atomic_load(&orientation)) {
    case UIInterfaceOrientationLandscapeLeft: right = v[1]; up = -v[0]; break;       /* the device's top to the right */
    case UIInterfaceOrientationPortrait: right = v[0]; up = v[1]; break;
    case UIInterfaceOrientationPortraitUpsideDown: right = -v[0]; up = -v[1]; break;
    default: right = -v[1]; up = v[0]; break;                                       /* landscape right: its top to the left */
    }
    out[0] = right; out[1] = -v[2]; out[2] = up;
}
static void sample(CMDeviceMotion *m) {   /* queue */
    if (!m || !atomic_load(&feeding)) return;
    const double acc[3] = {m.gravity.x + m.userAcceleration.x, m.gravity.y + m.userAcceleration.y, m.gravity.z + m.userAcceleration.z},
                 rate[3] = {m.rotationRate.x, m.rotationRate.y, m.rotationRate.z};
    double a[3], g[3]; to_controller(acc, a); to_controller(rate, g);
    runtime_motion_feed(a, g, runtime_now_ns());
}
static void read_orientation(void) {   /* main queue */
    UIWindowScene *scene = host.window.windowScene;
    if (scene) atomic_store(&orientation, (int)scene.effectiveGeometry.interfaceOrientation);
}
static void update(void) {   /* main queue: on when wanted and nothing else gives motion */
    BOOL want = enabled && active && !controller && runtime_gyro_enabled() && manager.deviceMotionAvailable;
    if (want == running) return;
    running = want;
    if (want) {
        read_orientation();
        runtime_motion_deadzone_default(0); runtime_motion_live(1); atomic_store(&feeding, 1);
        [manager startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXArbitraryZVertical toQueue:queue
                                                withHandler:^(CMDeviceMotion *m, NSError *err) { (void)err; sample(m); }];
        fprintf(stderr, "[gyro] the device's own motion sensors on (%.0f samples a second)\n", 1 / manager.deviceMotionUpdateInterval);
    } else {
        atomic_store(&feeding, 0); [manager stopDeviceMotionUpdates];
        if (!controller) runtime_motion_live(0);   /* a controller that took over keeps its own */
        fprintf(stderr, "[gyro] the device's own motion sensors off (%s)\n", controller ? "a controller gives motion" : !enabled ? "motion controls off" : !active ? "app inactive" : "none");
    }
}

/* Called once by the game view when it appears. */
void runtime_ios_device_motion_start(UIView *view) {
    host = view;
    if (manager) return;
    manager = [CMMotionManager new]; manager.deviceMotionUpdateInterval = 1.0 / 100;
    queue = [NSOperationQueue new]; queue.name = @"totk.device-motion"; queue.maxConcurrentOperationCount = 1;
    queue.qualityOfService = NSQualityOfServiceUserInteractive;
    if (!manager.deviceMotionAvailable) fprintf(stderr, "[gyro] this device has no motion sensors\n");
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) { (void)t; read_orientation(); }];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; active = NO; update(); }];
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; active = YES; update(); }];
    update();
}
void runtime_device_motion_controller(int attached) {
    if (attached) atomic_store(&feeding, 0);   /* at once: the controller's samples must not mix with the device's */
    dispatch_async(dispatch_get_main_queue(), ^{ controller = !!attached; update(); });
}
void runtime_device_motion_enable(int on) {
    dispatch_async(dispatch_get_main_queue(), ^{ enabled = !!on; update(); });
}
int runtime_device_motion_enabled(void) { return enabled; }
