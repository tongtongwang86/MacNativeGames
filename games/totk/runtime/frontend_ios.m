/* Native iOS view and VI service adapter: the iPhone counterpart of frontend.m (same exported functions).
   The app (ios/app/Sources/main.m) owns UIKit: it creates the full-screen Metal view, hands its layer over with
   runtime_ios_attach_layer, and calls the loader's main on a game thread. No guest frame is synthesized. */
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include "runtime.h"
#include "totk_ui_lang.h"
#import "boot_view_ios.h"
static CAMetalLayer *metalLayer;
static TOTKBootView *bootView;   /* the boot screen (boot_view_ios.m) until the first presented frame */
static uint64_t bootStartNS;
static _Atomic uint64_t presentedFrames;
static CADisplayLink *displayLink;
static pthread_mutex_t vsyncLock=PTHREAD_MUTEX_INITIALIZER;
static void *vsyncEvents[16];
static unsigned vsyncCount;
void hle_bind_system_event(void *,int);
void hle_signal_system_event(void *);
void *front_metal_layer(void){return (__bridge void *)metalLayer;}
void front_frame_presented(void){
 if(atomic_fetch_add_explicit(&presentedFrames,1,memory_order_relaxed)==0){
  fprintf(stderr,"[totk-boot] %.3f first presented frame\n",(runtime_now_ns()-bootStartNS)/1e9);
  dispatch_async(dispatch_get_main_queue(), ^{[bootView dismiss];bootView=nil;});
 }
}
/* A step on the boot screen, from any thread: its name, a second line (may be nil), and progress 0..1 or -1 for none. */
static void boot_show(NSString *stage,NSString *detail,double progress){
 dispatch_async(dispatch_get_main_queue(), ^{[bootView setStage:stage detail:detail progress:progress];});
}
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total){
 static const char *last;
 if(stage!=last){fprintf(stderr,"[totk-boot] %.3f %s\n",bootStartNS?(runtime_now_ns()-bootStartNS)/1e9:0,stage?stage:"startup");last=stage;}
 NSString *s=[NSString stringWithUTF8String:stage?stage:TOTK_UI("正在启动","Starting")];
 if(total)boot_show(s,[NSString stringWithFormat:@"%.1f / %.1f GB",done/1e9,total/1e9],(double)done/(double)total);
 else boot_show(s,@(TOTK_UI("正在处理，请稍候","Working, please wait")),-1);
}
void runtime_boot_count(const char *stage,uint64_t done,uint64_t total){
 /* Steps counted in items, not bytes (the shader import: tens of thousands of files, minutes on a phone). */
 static const char *last;
 if(stage!=last){fprintf(stderr,"[totk-boot] %.3f %s\n",bootStartNS?(runtime_now_ns()-bootStartNS)/1e9:0,stage?stage:"startup");last=stage;}
 NSString *s=[NSString stringWithUTF8String:stage?stage:TOTK_UI("正在启动","Starting")];
 boot_show(s,[NSString stringWithFormat:@"%llu / %llu",(unsigned long long)done,(unsigned long long)total],total?(double)done/(double)total:-1);
}
void runtime_boot_status(const char *stage,const char *detail){
 if(bootStartNS)fprintf(stderr,"[totk-boot] %.3f %s — %s\n",(runtime_now_ns()-bootStartNS)/1e9,stage?stage:"startup",detail?detail:"");
 NSString *s=[NSString stringWithUTF8String:stage?stage:TOTK_UI("正在启动","Starting")];
 NSString *d=[NSString stringWithUTF8String:detail?detail:TOTK_UI("正在处理，请稍候","Working, please wait")];
 boot_show(s,d,-1);
}
/* Background shader/pipeline warm-up progress (iOS ships with the warm manifest off, but the hooks are shared with the Mac front end). */
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total){
 (void)shaders;(void)shader_total;(void)pipelines;(void)pipeline_total;
 double frac=total ? fmin(1.0,(double)done/(double)total) : 1.0;
 boot_show([NSString stringWithFormat:@(TOTK_UI("预热着色器与管线 %.0f%%","Warming shaders and pipelines %.0f%%")),floor(frac*100.0)],
           [NSString stringWithFormat:@(TOTK_UI("已处理 %llu/%llu 项","%llu/%llu items")),(unsigned long long)done,(unsigned long long)total],frac);
}
void runtime_warm_done(void){}
/* The "温控" of the present lines (hle_nvn.c): the system's thermal state, 0 nominal, 1 fair, 2 serious, 3 critical; the default
   (-1) was never replaced on iOS, so a phone slowing down as it warmed could not be told from other causes (2026-10-23). */
int dm_thermal_now(void){return (int)NSProcessInfo.processInfo.thermalState;}
uint64_t front_game_res(int docked){(void)docked;return (720ULL<<32)|1280;}
static _Atomic int appActive=1;
int runtime_front_focus(void){return atomic_load(&appActive);}
void mtl_app_active(int active);
void mtl_hud_safe_area(int w,int h,int side,int bottom);   /* gfx/nvn/nvn_mtl.m: the HUD boxes keep out of the safe area */
void runtime_ios_sync_safe_area(UIView *host);
/* Not active (background, Control Center, lock): stop GPU submission (iOS rejects background GPU work) and tell the game focus changed, like the Mac window. */
static void app_active_changed(int active){
 if(atomic_exchange(&appActive,active)==active)return;
 if(!active)runtime_input_clear();
 mtl_app_active(active);runtime_oe_push_message(15);
 fprintf(stderr,"[totk] app %s\n",active?"became active":"resigned active: GPU submission paused");
}
void runtime_front_request_close(void){fprintf(stderr,"[totk] close requested; iOS apps are closed by the user\n");}
void runtime_front_text_test(const char *op,const char *text){(void)text;fprintf(stderr,"[totk] text entry test \"%s\" ignored: iOS has no text entry bar (the keyboard request is cancelled)\n",op?op:"");}

@interface TOTKVsync:NSObject
@end
@implementation TOTKVsync
-(void)tick:(CADisplayLink *)l{(void)l;pthread_mutex_lock(&vsyncLock);for(unsigned i=0;i<vsyncCount;i++)hle_signal_system_event(vsyncEvents[i]);pthread_mutex_unlock(&vsyncLock);}
@end
static TOTKVsync *vsyncTarget;

/* The drawable at 720 rows (the system compositor scales it to the screen) or at the screen's own pixels with MetalFX
   scaling the picture to it (the Settings sheet's Upscaling; the same output stage as the Mac's MetalFX 2K). */
static CGSize drawableCompact,drawableNative;static int fullResolution;static __weak UIView *drawableHost;
void dm_gfx_set_flag(const char *name,int v);
static void drawable_apply(const char *why){
 CGSize px=fullResolution?drawableNative:drawableCompact;
 if(px.width<1||CGSizeEqualToSize(metalLayer.drawableSize,px))return;
 metalLayer.drawableSize=px;
 fprintf(stderr,"[totk] view drawable %.0fx%.0f (%s)\n",px.width,px.height,why);runtime_aspect_view(px.width,px.height);   /* full screen: the camera takes the drawable's shape (aspect_patch.h) */
 UIView *host=drawableHost;if(host)runtime_ios_sync_safe_area(host);
}
int runtime_ios_full_resolution(void){return fullResolution;}
void runtime_ios_set_full_resolution(int on){
 fullResolution=!!on;
 dispatch_async(dispatch_get_main_queue(),^{drawable_apply(fullResolution?"MetalFX to the screen":"scaled by the system");});
 dm_gfx_set_flag("mfx_output",fullResolution);dm_gfx_set_flag("mfx_upscale",fullResolution);   /* the output stage runs only with MetalFX upscaling on */
}

/* visionOS windows resize after launch; keep the drawable and camera aspect in step with the view. */
void runtime_ios_sync_drawable(UIView *host){
 if(!metalLayer||host.bounds.size.width<1||host.bounds.size.height<1)return;
 drawableHost=host;
#if TARGET_OS_VISION
 CGFloat s=host.traitCollection.displayScale>0?host.traitCollection.displayScale:2;
#else
 CGFloat s=host.window.screen.nativeScale>0?host.window.screen.nativeScale:UIScreen.mainScreen.nativeScale;
#endif
 metalLayer.contentsScale=s;
 /* TOTK_IOS_DRAWABLE_H (default 720; 0 = native pixels): build the drawable this many pixels tall at the view's aspect, so the
    game picture (1280x720 canvas) lands about 1:1 and the system compositor scales it to the screen instead of our GPU. */
 double vw=round(host.bounds.size.width*s),vh=round(host.bounds.size.height*s);
 const char *dh=getenv("TOTK_IOS_DRAWABLE_H");long want=dh?strtol(dh,NULL,10):720;
 /* the 16:9 picture should get about want rows: a screen narrower than 16:9 (an iPad, about 4:3; 2026-10-17) holds it by
    width, so there the drawable is want*16/9 columns wide (1281x890 on a 2360x1640 iPad); an iPhone keeps want rows */
 drawableNative=CGSizeMake(vw,vh);
 if(want>0){double h=vw/vh<16.0/9.0?round(want*16.0/9.0*vh/vw):(double)want;if(h<vh){vw=round(vw*h/vh);vh=h;}}
 drawableCompact=CGSizeMake(vw,vh);
 char why[64];snprintf(why,sizeof why,"screen scale %.1f, TOTK_IOS_DRAWABLE_H=%ld",s,want);
 drawable_apply(why);
}
/* The safe area (notch, rounded corners, home indicator) in drawable pixels: the HUD's corner boxes keep out of it
   (nvn_mtl.m). The view's safeAreaInsetsDidChange calls it too: at the first drawable the view is in no window yet and
   the insets are 0. */
void runtime_ios_sync_safe_area(UIView *host){
 CGSize px=metalLayer?metalLayer.drawableSize:CGSizeZero;
 if(px.width<1||host.bounds.size.width<1)return;
 UIEdgeInsets in=host.safeAreaInsets;double k=px.width/host.bounds.size.width;   /* points -> drawable pixels */
 mtl_hud_safe_area((int)px.width,(int)px.height,(int)lround(MAX(in.left,in.right)*k),(int)lround(in.bottom*k));
}

/* Called on the main thread by the app before the game thread starts. */
void runtime_ios_attach_layer(CAMetalLayer *layer,UIView *host){
 bootStartNS=runtime_now_ns();
 metalLayer=layer;metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;metalLayer.framebufferOnly=NO;
 runtime_ios_sync_drawable(host);
 /* The boot screen: the swirl pattern the build carries in GameData/launch (build_ipa.py --launch-pattern; without it
    the plain ground), the step in the bottom-right corner. A message (no game data) wraps to two lines there. */
 NSString *pattern=[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"GameData/launch"];
 if(![NSFileManager.defaultManager fileExistsAtPath:[pattern stringByAppendingPathComponent:@"pattern.json"]])pattern=nil;
 bootView=[[TOTKBootView alloc] initWithFrame:host.bounds patternFolder:pattern];
 [bootView setStage:@(TOTK_UI("正在启动海拉鲁","Starting Hyrule")) detail:@(TOTK_UI("正在准备游戏环境","Preparing the game environment")) progress:-1];
 [host addSubview:bootView];
 NSNotificationCenter *nc=NSNotificationCenter.defaultCenter;
 [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n){(void)n;app_active_changed(0);}];
 [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n){(void)n;app_active_changed(1);}];
 vsyncTarget=[TOTKVsync new];displayLink=[CADisplayLink displayLinkWithTarget:vsyncTarget selector:@selector(tick:)];
 displayLink.preferredFrameRateRange=CAFrameRateRangeMake(60,60,60);[displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
/* The app already runs the loader on a game thread with a large stack; the event loop belongs to UIKit. */
void runtime_front_run(void(*callback)(void)){
 if(!metalLayer)runtime_fail("iOS view not attached before boot");
 runtime_gamepad_start();
 @autoreleasepool{callback();}
}
static int viReady;
static unsigned displayIdentity,layerIdentity;
static void vi_initialize(void){if(!metalLayer)runtime_fail("VI requires the native iOS view");viReady=1;fprintf(stderr,"[totk] native VI display initialized\n");}
static uint32_t vi_open(void **out){if(!viReady||!out)return 0xf601;*out=&displayIdentity;return 0;}
static uint32_t vi_layer(void **out,void *display){if(!out||display!=&displayIdentity)return 0xf601;*out=&layerIdentity;return 0;}
static uint32_t vi_window(void **out,void *layer){if(!out||layer!=&layerIdentity)return 0xf601;*out=front_metal_layer();return 0;}
static uint32_t vi_vsync(void *ev,void *display){
 if(!ev||display!=&displayIdentity)return 0xf601;hle_bind_system_event(ev,1);
 pthread_mutex_lock(&vsyncLock);if(vsyncCount>=16)runtime_fail("VI display event limit");vsyncEvents[vsyncCount++]=ev;pthread_mutex_unlock(&vsyncLock);return 0;
}
void *runtime_front_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn2vi10InitializeEv",vi_initialize},{"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_open},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer},{"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window},
 {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}
