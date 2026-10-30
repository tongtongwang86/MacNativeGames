/* The Mac boot screen, after the iPhone / iPad one (boot_view_ios.m, which has the design notes): the swirl pattern with
   each circle copy turning on its own and each line drifting up or down, the step in the bottom-right corner in
   starlight yellow over a progressive blur under the text only. frontend.m drives it. Mac only (the iOS target builds
   every .m in runtime/). */
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import "boot_view.h"
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>

static CGColorRef rgb(unsigned v, CGFloat a) {
    return [NSColor colorWithSRGBRed:((v >> 16) & 255) / 255.0 green:((v >> 8) & 255) / 255.0 blue:(v & 255) / 255.0 alpha:a].CGColor;
}
static NSColor *ns(unsigned v) { return [NSColor colorWithSRGBRed:((v >> 16) & 255) / 255.0 green:((v >> 8) & 255) / 255.0 blue:(v & 255) / 255.0 alpha:1]; }
#define GROUND 0x0e0705   /* the pattern's ground */
#define STAR 0xfff4cc     /* starlight: the step */
#define STAR_DIM 0xf2e3ad /* its second line */
#define ACCENT 0xffe9a6   /* the bar and the turning diamond */

static CGImageRef load(NSString *folder, NSString *name) CF_RETURNS_RETAINED {
    if (![name isKindOfClass:NSString.class]) return NULL;
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:[folder stringByAppendingPathComponent:name]], NULL);
    CGImageRef im = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (src) CFRelease(src);
    return im;
}
/* The private Core Animation filter the iOS blur uses ("variableBlur": radius = inputRadius x the mask's alpha); nil when
   this macOS has none, and a masked plain blur stands in. */
static NSObject *make_filter(void) {
    Class cls = NSClassFromString(@"CAFilter"); SEL sel = NSSelectorFromString(@"filterWithType:");
    if (!cls || ![cls respondsToSelector:sel]) return nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    NSObject *f = [cls performSelector:sel withObject:@"variableBlur"];
#pragma clang diagnostic pop
    [f setValue:@"totkVariableBlur" forKey:@"name"]; [f setValue:@YES forKey:@"inputNormalizeEdges"];
    return f;
}

typedef struct { CGFloat x, y, size; double period; } Circle;

/* A view whose own layer is ours (layer-hosting), drawn top-down like UIKit. */
@interface TOTKLayerHost : NSView @end
@implementation TOTKLayerHost
- (instancetype)initWithFrame:(NSRect)r { if ((self = [super initWithFrame:r])) { CALayer *l = [CALayer layer]; l.geometryFlipped = YES; self.layer = l; self.wantsLayer = YES; } return self; }
- (BOOL)isFlipped { return YES; }
- (NSView *)hitTest:(NSPoint)p { (void)p; return nil; }
@end

@interface TOTKFlippedView : NSView @end
@implementation TOTKFlippedView
- (BOOL)isFlipped { return YES; }
@end

@implementation TOTKMacBootView {
    TOTKLayerHost *_pattern, *_blur, *_deco;   // the swirl; the blur over it; the diamond and the bar
    NSView *_panel;                            // the corner block
    NSTextField *_stage, *_detail, *_percent;
    CALayer *_diamond, *_track, *_fill; CAGradientLayer *_sheen;
    CGImageRef _tile; CGFloat _period;
    Circle _circles[8]; CGImageRef _circleImages[8]; int _ncircles;
    CGFloat _lineX[64], _lineW[64]; CGImageRef _lineImages[64]; int _nlines;
    NSSize _builtFor; NSString *_maskFor; BOOL _variable;
    NSString *_stageText, *_detailText; double _progress;
}
- (BOOL)isFlipped { return YES; }
- (NSView *)hitTest:(NSPoint)p { (void)p; return nil; }   // the game's view under it keeps the mouse and keyboard

- (instancetype)initWithFrame:(NSRect)frame patternFolder:(NSString *)folder {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.wantsLayer = YES; self.layer.backgroundColor = rgb(GROUND, 1);
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _progress = -1; _stageText = @"Starting Hyrule";
    _pattern = [[TOTKLayerHost alloc] initWithFrame:self.bounds]; [self addSubview:_pattern];
    _blur = [[TOTKLayerHost alloc] initWithFrame:self.bounds]; [self addSubview:_blur];
    _variable = make_filter() != nil;
    if (!_variable) { _blur.layerUsesCoreImageFilters = YES; _blur.layer.mask = [CALayer layer]; }
    [self loadPattern:folder];
    _panel = [[TOTKFlippedView alloc] initWithFrame:NSZeroRect]; _panel.wantsLayer = YES; [self addSubview:_panel];
    NSShadow *sh = [NSShadow new]; sh.shadowColor = [NSColor colorWithWhite:0 alpha:0.35]; sh.shadowBlurRadius = 3; sh.shadowOffset = NSMakeSize(0, -1);
    _panel.shadow = sh;
    _deco = [[TOTKLayerHost alloc] initWithFrame:NSZeroRect]; [_panel addSubview:_deco];
    _diamond = [CALayer layer]; _diamond.bounds = CGRectMake(0, 0, 8, 8); _diamond.borderWidth = 1.4; _diamond.borderColor = rgb(ACCENT, 1);
    CAKeyframeAnimation *turn = [CAKeyframeAnimation animationWithKeyPath:@"transform.rotation.z"];
    turn.values = @[@(M_PI_4), @(M_PI_4 + M_PI), @(M_PI_4 + 2 * M_PI)]; turn.duration = 2; turn.repeatCount = HUGE_VALF; turn.removedOnCompletion = NO;
    CAMediaTimingFunction *ease = [CAMediaTimingFunction functionWithControlPoints:0.65 :0 :0.35 :1]; turn.timingFunctions = @[ease, ease];
    [_diamond addAnimation:turn forKey:@"turn"]; [_deco.layer addSublayer:_diamond];
    _track = [CALayer layer]; _track.masksToBounds = YES; _track.cornerRadius = 1; _track.backgroundColor = rgb(ACCENT, 0.18);
    _fill = [CALayer layer]; _fill.backgroundColor = rgb(ACCENT, 1); _fill.cornerRadius = 1; [_track addSublayer:_fill];
    _sheen = [CAGradientLayer layer]; _sheen.startPoint = CGPointMake(0, 0.5); _sheen.endPoint = CGPointMake(1, 0.5);
    _sheen.colors = @[(__bridge id)rgb(0xfffae6, 0), (__bridge id)rgb(0xfffae6, 0.85), (__bridge id)rgb(0xfffae6, 0)];
    [_track addSublayer:_sheen]; [_deco.layer addSublayer:_track];
    _stage = [NSTextField labelWithString:@""]; _detail = [NSTextField wrappingLabelWithString:@""]; _percent = [NSTextField labelWithString:@""];
    _detail.maximumNumberOfLines = 2; _percent.alignment = NSTextAlignmentRight;
    for (NSTextField *t in @[_stage, _detail, _percent]) { t.drawsBackground = NO; t.bezeled = NO; t.editable = NO; t.selectable = NO; [_panel addSubview:t]; }
    [self setStage:_stageText detail:nil progress:-1];
    return self;
}
- (void)dealloc {
    CGImageRelease(_tile);
    for (int i = 0; i < _ncircles; i++) CGImageRelease(_circleImages[i]);
    for (int i = 0; i < _nlines; i++) CGImageRelease(_lineImages[i]);
}

- (void)loadPattern:(NSString *)folder {
    if (!folder) return;
    NSData *json = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"pattern.json"]];
    NSDictionary *meta = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
    if (![meta isKindOfClass:NSDictionary.class] || [meta[@"period"] doubleValue] <= 0) return;
    _period = [meta[@"period"] doubleValue];
    NSArray *lines = [meta[@"lines"] isKindOfClass:NSArray.class] ? meta[@"lines"] : nil;
    for (NSDictionary *l in lines) {
        if (_nlines == 64 || ![l isKindOfClass:NSDictionary.class]) break;
        CGImageRef im = load(folder, l[@"file"]);
        if (!im || [l[@"width"] doubleValue] <= 0) { CGImageRelease(im); continue; }
        _lineX[_nlines] = [l[@"x"] doubleValue]; _lineW[_nlines] = [l[@"width"] doubleValue]; _lineImages[_nlines++] = im;
    }
    if (!_nlines) _tile = load(folder, @"tile.png");   // an older pattern: the whole picture, still
    if (!_nlines && !_tile) return;
    static const struct { const char *name; double period; } pace[] = {{"F", 140}, {"A", 70}, {"B", 82}, {"D", 52}, {"C", 34}};
    for (NSDictionary *c in [meta[@"circles"] isKindOfClass:NSArray.class] ? meta[@"circles"] : nil) {
        if (_ncircles == 8 || ![c isKindOfClass:NSDictionary.class]) break;
        CGImageRef im = load(folder, c[@"file"]); if (!im) continue;
        Circle k = {[c[@"x"] doubleValue], [c[@"y"] doubleValue], [c[@"size"] doubleValue], 60};
        for (unsigned i = 0; i < sizeof pace / sizeof *pace; i++) if ([c[@"name"] isEqual:@(pace[i].name)]) k.period = pace[i].period;
        _circles[_ncircles] = k; _circleImages[_ncircles++] = im;
    }
}

/* As on iOS: the tile 0.88 of the shorter side, each line a strip repeating a tile at a time and sliding one tile up or
   down (25 to 90 s), each circle copy turning its own way at 0.55 to 1.6 times its circle's pace. */
- (void)buildPattern {
    NSSize s = self.bounds.size;
    if ((!_nlines && !_tile) || NSEqualSizes(s, _builtFor) || s.width < 1) return;
    _builtFor = s;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    _pattern.layer.sublayers = nil;
    CGFloat tilePt = round(MIN(s.width, s.height) * 0.88), k = tilePt / _period, scale = self.window.backingScaleFactor ?: 2;
    int nx = (int)ceil(s.width / tilePt), ny = (int)ceil(s.height / tilePt);
    CFTimeInterval now = CACurrentMediaTime();
    if (_nlines) { for (int tx = -1; tx <= nx; tx++) for (int i = 0; i < _nlines; i++) {
        CGFloat w = _lineW[i] * k, x = (tx + (_lineX[i] - _lineW[i] / 2) / _period) * tilePt;
        if (x + w < 0 || x > s.width) continue;
        CGFloat h = w * CGImageGetHeight(_lineImages[i]) / CGImageGetWidth(_lineImages[i]);   // one tile tall
        CAReplicatorLayer *strip = [CAReplicatorLayer layer]; strip.frame = CGRectMake(x, -tilePt, w, s.height + 2 * tilePt);
        strip.instanceCount = (NSInteger)ceil(strip.frame.size.height / h) + 1; strip.instanceTransform = CATransform3DMakeTranslation(0, h, 0);
        CALayer *one = [CALayer layer]; one.frame = CGRectMake(0, 0, w, h); one.contents = (__bridge id)_lineImages[i]; one.contentsScale = scale;
        [strip addSublayer:one];
        CABasicAnimation *flow = [CABasicAnimation animationWithKeyPath:@"transform.translation.y"];
        flow.fromValue = @0; flow.toValue = @(arc4random_uniform(2) ? h : -h);
        flow.duration = 25 + arc4random_uniform(6500) / 100.0; flow.repeatCount = HUGE_VALF; flow.removedOnCompletion = NO;
        flow.beginTime = now - arc4random_uniform((uint32_t)(flow.duration * 1000)) / 1000.0;
        [strip addAnimation:flow forKey:@"flow"];
        [_pattern.layer addSublayer:strip];
    } } else { for (int ty = 0; ty < ny; ty++) for (int tx = 0; tx < nx; tx++) {
        CALayer *t = [CALayer layer]; t.frame = CGRectMake(tx * tilePt, ty * tilePt, tilePt, tilePt); t.contents = (__bridge id)_tile;
        [_pattern.layer addSublayer:t];
    } }
    for (int i = 0; i < _ncircles; i++) {
        Circle c = _circles[i]; CGFloat d = c.size * k;
        for (int ty = -1; ty <= ny; ty++) for (int tx = -1; tx <= nx; tx++) {
            CGPoint at = CGPointMake((tx + c.x / _period) * tilePt, (ty + c.y / _period) * tilePt);
            if (at.x + d / 2 < 0 || at.y + d / 2 < 0 || at.x - d / 2 > s.width || at.y - d / 2 > s.height) continue;
            CALayer *l = [CALayer layer]; l.contents = (__bridge id)_circleImages[i]; l.bounds = CGRectMake(0, 0, d, d); l.position = at;
            l.contentsScale = scale; l.allowsEdgeAntialiasing = YES;
            CABasicAnimation *spin = [CABasicAnimation animationWithKeyPath:@"transform.rotation.z"];
            spin.fromValue = @0; spin.toValue = @(2 * M_PI * (arc4random_uniform(2) ? 1 : -1));
            spin.duration = c.period * (0.55 + arc4random_uniform(1050) / 1000.0); spin.repeatCount = HUGE_VALF; spin.removedOnCompletion = NO;
            spin.beginTime = now - arc4random_uniform((uint32_t)(spin.duration * 1000)) / 1000.0;
            [l addAnimation:spin forKey:@"twirl"];
            [_pattern.layer addSublayer:l];
        }
    }
    [CATransaction commit];
}

/* The blur's mask over the whole view: full over the block (25 pt in from its edges), a smoothstep falloff over the next
   100 pt with squircle contours (|x|^4 + |y|^4), as on iOS. */
- (void)refreshBlur:(NSRect)p {
    NSSize v = self.bounds.size;
    if (v.width < 1 || v.height < 1) return;
    NSString *key = [NSString stringWithFormat:@"%.0fx%.0f %@", v.width, v.height, NSStringFromRect(p)];
    if ([key isEqualToString:_maskFor]) return;
    _maskFor = key;
    CGFloat cx = NSMidX(p), cy = NSMidY(p), hx = fmax(p.size.width / 2 - 25, 0), hy = fmax(p.size.height / 2 - 25, 0), pad = 100;
    int w = 256, h = MAX(1, (int)lround(256 * v.height / v.width)); double ppp = v.width / w;
    uint8_t *px = calloc((size_t)w * h, 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double ox = fmax(fabs((x + 0.5) * ppp - cx) - hx, 0) / pad, oy = fmax(fabs((y + 0.5) * ppp - cy) - hy, 0) / pad;
        double t = pow(pow(ox, 4) + pow(oy, 4), 0.25); if (t >= 1) continue;
        double u = 1 - t, a = u * u * (3 - 2 * u); uint8_t *q = px + ((size_t)y * w + x) * 4;
        q[0] = q[1] = q[2] = q[3] = (uint8_t)lround(a * 255);
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGImageRef mask = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs); free(px);
    if (!mask) return;
    if (_variable) {
        NSObject *f = make_filter();   // a fresh object: Core Animation notices filter changes by identity
        [f setValue:@2.6 forKey:@"inputRadius"]; [f setValue:(__bridge id)mask forKey:@"inputMaskImage"];
        _blur.layer.backgroundFilters = f ? @[f] : nil;
    } else {
        CIFilter *g = [CIFilter filterWithName:@"CIGaussianBlur"]; [g setValue:@2.6 forKey:kCIInputRadiusKey];
        _blur.layer.backgroundFilters = @[g];
        _blur.layer.mask.frame = _blur.layer.bounds; _blur.layer.mask.contents = (__bridge id)mask;
    }
    CGImageRelease(mask);
}

- (void)setStage:(NSString *)stage detail:(NSString *)detail progress:(double)progress {
    _stageText = stage.length ? stage : @"Starting Hyrule"; _detailText = detail.length ? detail : nil; _progress = progress;
    self.needsLayout = YES;
}

- (NSAttributedString *)text:(NSString *)s font:(NSFont *)f color:(unsigned)c kern:(CGFloat)kern {
    return [[NSAttributedString alloc] initWithString:s attributes:@{NSFontAttributeName: f, NSForegroundColorAttributeName: ns(c), NSKernAttributeName: @(kern)}];
}

/* The block sized for the window: the iPad's at a 720 pt tall window, up to half again on a bigger one. */
- (void)layout {
    [super layout];
    NSSize s = self.bounds.size;
    _pattern.frame = _blur.frame = self.bounds;
    [self buildPattern];
    CGFloat z = fmin(fmax(MIN(s.width, s.height) / 720, 1), 1.5);
    NSFont *stageFont = [NSFont fontWithName:@"AvenirNext-Regular" size:15 * z] ?: [NSFont systemFontOfSize:15 * z];
    NSFont *detailFont = [NSFont fontWithName:@"AvenirNext-Medium" size:9.5 * z] ?: [NSFont systemFontOfSize:9.5 * z];
    BOOL hasBar = _progress >= 0;
    _stage.attributedStringValue = [self text:_stageText font:stageFont color:STAR kern:0.6 * z];
    _detail.attributedStringValue = [self text:_detailText ? _detailText.uppercaseString : @"" font:detailFont color:STAR_DIM kern:1.8 * z];
    _percent.attributedStringValue = [self text:hasBar ? [NSString stringWithFormat:@"%d%%", (int)floor(fmin(_progress, 1) * 100)] : @""
                                           font:[NSFont monospacedDigitSystemFontOfSize:12 * z weight:NSFontWeightRegular] color:STAR kern:0];
    CGFloat W = 320 * z, percentW = hasBar ? 44 * z : 0, rowH = ceil(MAX(_stage.attributedStringValue.size.height, 20 * z)), y;
    _stage.frame = NSMakeRect(18 * z - 2, 0, W - 18 * z - percentW, rowH);
    NSFont *percentFont = [NSFont monospacedDigitSystemFontOfSize:12 * z weight:NSFontWeightRegular];   // on the step's baseline
    _percent.frame = NSMakeRect(W - percentW, round(stageFont.ascender - percentFont.ascender), percentW, rowH); _percent.hidden = !hasBar;
    y = rowH + 8 * z;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    _diamond.position = CGPointMake(4 * z, rowH / 2 + 1); _diamond.bounds = CGRectMake(0, 0, 8 * z, 8 * z);
    _track.hidden = !hasBar;
    if (hasBar) {
        _track.frame = CGRectMake(0, y, W, 2); _fill.frame = CGRectMake(0, 0, W * fmax(0, fmin(_progress, 1)), 2); _sheen.frame = CGRectMake(-50, 0, 50, 2);
        if (![_sheen animationForKey:@"sheen"]) {
            CABasicAnimation *run = [CABasicAnimation animationWithKeyPath:@"position.x"];
            run.fromValue = @(-25); run.toValue = @(W + 25); run.duration = 1.8; run.repeatCount = HUGE_VALF; run.removedOnCompletion = NO;
            run.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
            [_sheen addAnimation:run forKey:@"sheen"];
        }
        y += 2 + 8 * z;
    }
    [CATransaction commit];
    _detail.preferredMaxLayoutWidth = W;
    CGFloat detailH = _detailText ? ceil([_detail.attributedStringValue boundingRectWithSize:NSMakeSize(W, 200) options:NSStringDrawingUsesLineFragmentOrigin].size.height) : 0;
    _detail.frame = NSMakeRect(-2, y, W + 4, detailH); _detail.hidden = !detailH;
    CGFloat H = detailH ? y + detailH : rowH, margin = 36 * z;
    _panel.frame = NSMakeRect(s.width - margin - W, s.height - margin * 0.8 - H, W, H);
    _deco.frame = _panel.bounds;
    [self refreshBlur:_panel.frame];
}

- (void)dismiss {
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *c) { c.duration = 0.45; self.animator.alphaValue = 0; }
                        completionHandler:^{ [self removeFromSuperview]; }];
}
@end
#endif
