/* The boot screen: the player's swirl pattern (ios/tools/launch_pattern.py) with each of its circles turning on its own,
   and the startup step in the bottom-right corner in starlight yellow. iOS only; frontend_ios.m drives it, and
   ios/tools/boot_preview shows it in the Simulator. */
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import "boot_view_ios.h"
#import <QuartzCore/QuartzCore.h>

static UIColor *rgb(unsigned v, CGFloat a) { return [UIColor colorWithRed:((v >> 16) & 255) / 255.0 green:((v >> 8) & 255) / 255.0 blue:(v & 255) / 255.0 alpha:a]; }
#define GROUND 0x0e0705   /* the pattern's ground (pattern.json "ground"; the plain background without a pattern) */
#define STAR 0xfff4cc     /* starlight: the step */
#define STAR_DIM 0xf2e3ad /* its second line */
#define ACCENT 0xffe9a6   /* the bar and the turning diamond */

static NSString *const kNames[TOTKBootContrastCount] = {@"halo", @"glass", @"plaque", @"band", @"dim", @"outline", @"bold", @"blur", @"blur-band"};
NSString *TOTKBootContrastName(TOTKBootContrast style) { return style >= 0 && style < TOTKBootContrastCount ? kNames[style] : @"halo"; }
TOTKBootContrast TOTKBootContrastFromEnvironment(TOTKBootContrast fallback) {
    const char *e = getenv("TOTK_BOOT_STYLE");
    NSString *v = e && *e ? @(e) : [NSUserDefaults.standardUserDefaults stringForKey:@"TOTKBootStyle"];
    if (!v.length) return fallback;
    for (NSInteger i = 0; i < TOTKBootContrastCount; i++) if ([v caseInsensitiveCompare:kNames[i]] == NSOrderedSame) return (TOTKBootContrast)i;
    NSInteger n = v.integerValue;
    return n >= 0 && n < TOTKBootContrastCount && [v rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet].location == 0 ? (TOTKBootContrast)n : fallback;
}

/* A progressive blur, after tongdisplay's ProgressiveBlur (itself after github.com/nikstar/VariableBlur): the effect
   view's backdrop layer gets one Core Animation filter of type "variableBlur" (private API, hence the reversed names),
   whose radius at each pixel is inputRadius x the alpha of inputMaskImage. The view covers the whole screen, so the
   mask alone decides where the blur is: a layer edge inside the picture showed as a faint line on an A12X iPad. A tint
   with the same mask darkens in step with the blur. Without the filter, a standard blur masked by the same image. */
@interface NSString (TOTKReverse) - (NSString *)reverseString; @end
@interface TOTKVariableBlurView : UIVisualEffectView
@property (nonatomic) CGPoint radialFrom;     // unit point where the blur is full (radial); x < 0: linear
@property (nonatomic) CGSize radialExtent;    // half-size (points) of the rounded-rectangle area it falls off over
@property (nonatomic) CGPoint linearStart, linearEnd;   // unit points: no blur at start, full at end
@property (nonatomic) CGFloat maxRadius, tint;
@property (nonatomic) CGFloat frost;          // a white veil with the same mask: frosted glass rather than shade (0 = none)
/* Around a box instead of from a point: full strength over the box (half-size coreHalf round radialFrom), then a
   smoothstep falloff over `padding` points beyond it, its contours squircles. coreHalf zero: the logarithmic radial. */
@property (nonatomic) CGSize coreHalf;
@property (nonatomic) CGFloat padding;
- (void)refresh;   // after a change: masks drawn again only when the size or the spec changed
@end

@implementation TOTKVariableBlurView {
    BOOL _variable; UIImageView *_fallbackMask, *_tintView, *_frostView; CGImageRef _ramp; NSString *_drawnFor;
}
static NSObject *make_filter(void) {
    Class cls = NSClassFromString([@"retliFAC" reverseString]);                          // CAFilter
    SEL sel = NSSelectorFromString([@":epyThtiWretlif" reverseString]);                  // filterWithType:
    if (!cls || ![cls respondsToSelector:sel]) return nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    NSObject *f = [cls performSelector:sel withObject:@"variableBlur"];
#pragma clang diagnostic pop
    [f setValue:@"totkVariableBlur" forKey:@"name"];
    [f setValue:@YES forKey:@"inputNormalizeEdges"];
    return f;
}
- (instancetype)init {
    if (!(self = [super initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleRegular]])) return nil;
    self.userInteractionEnabled = NO; _maxRadius = 32; _tint = 0.65; _radialFrom = CGPointMake(1, 1);
    _variable = make_filter() != nil;
    if (_variable) for (UIView *v in [self.subviews subarrayWithRange:NSMakeRange(1, self.subviews.count - 1)]) v.alpha = 0;   // our tint instead
    else { _fallbackMask = [UIImageView new]; self.maskView = _fallbackMask; }
    _tintView = [UIImageView new]; [self.contentView addSubview:_tintView];
    _frostView = [UIImageView new]; [self.contentView addSubview:_frostView];
    return self;
}
- (void)dealloc { CGImageRelease(_ramp); }
- (CALayer *)backdrop { return self.subviews.firstObject.layer; }
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window && self.backdrop) [self.backdrop setValue:@(self.window.traitCollection.displayScale) forKey:@"scale"];   // no pixelation where the blur ends
}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-implementations"
- (void)traitCollectionDidChange:(UITraitCollection *)previous { (void)previous; }   // not super: it puts the standard filters back
#pragma clang diagnostic pop
- (void)layoutSubviews { [super layoutSubviews]; _tintView.frame = _frostView.frame = self.bounds; _fallbackMask.frame = self.bounds; [self refresh]; }

/* The mask, 256 pixels wide stretched over the view. Radial: contours are squircles, |x|^4 + |y|^4 = t^4 (flat along the
   screen's edges, rounded towards the middle), and the strength falls off logarithmically, s(t) = ln(1 + 6(1 - t)) / ln 7,
   staying high where the text is, with a smoothstep taper over the last 35 % so the blur ends without a ring. Linear: a
   smoothstep from start to end. alpha scales it (the tint's darkness); white: the blur mask, else black. */
- (CGImageRef)newMaskWhite:(BOOL)white alpha:(double)a CF_RETURNS_RETAINED {
    CGSize v = self.bounds.size;
    int w = 256, h = MAX(1, (int)lround(256 * v.height / MAX(v.width, 1)));
    double ppp = v.width / w;   // points per mask pixel
    uint8_t *px = calloc((size_t)w * h, 4);
    BOOL radial = _radialFrom.x >= 0;
    double cx = _radialFrom.x * v.width, cy = _radialFrom.y * v.height;
    double ex = _radialExtent.width > 0 ? _radialExtent.width : MIN(v.width, v.height), ey = _radialExtent.height > 0 ? _radialExtent.height : MIN(v.width, v.height);
    double sx = _linearStart.x * v.width, sy = _linearStart.y * v.height, dx0 = _linearEnd.x * v.width - sx, dy0 = _linearEnd.y * v.height - sy;
    double len2 = fmax(dx0 * dx0 + dy0 * dy0, 1e-9), n = 4, k = 6, logK = log(1 + k);
    for (int py = 0; py < h; py++) for (int pxx = 0; pxx < w; pxx++) {
        double X = (pxx + 0.5) * ppp, Y = (py + 0.5) * ppp, s;
        if (radial && _coreHalf.width > 0 && _padding > 0) {
            double ox = fmax(fabs(X - cx) - _coreHalf.width, 0) / _padding, oy = fmax(fabs(Y - cy) - _coreHalf.height, 0) / _padding;
            double t = pow(pow(ox, n) + pow(oy, n), 1 / n);
            if (t >= 1) continue;
            double u = 1 - t;
            s = u * u * (3 - 2 * u);
        } else if (radial) {
            double t = pow(pow(fabs(X - cx) / ex, n) + pow(fabs(Y - cy) / ey, n), 1 / n);
            if (t >= 1) continue;
            double u = fmin((1 - t) / 0.35, 1);
            s = log(1 + k * (1 - t)) / logK * u * u * (3 - 2 * u);
        } else {
            double t = fmin(fmax(((X - sx) * dx0 + (Y - sy) * dy0) / len2, 0), 1);
            s = t * t * (3 - 2 * t);
        }
        double al = a * s; uint8_t *p = px + ((size_t)py * w + pxx) * 4;   // premultiplied RGBA
        uint8_t c = (uint8_t)lround((white ? al : 0) * 255);
        p[0] = p[1] = p[2] = c; p[3] = (uint8_t)lround(al * 255);
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs); free(px);
    return img;
}
- (void)refresh {
    CGSize v = self.bounds.size;
    if (v.width < 1 || v.height < 1) return;
    NSString *key = [NSString stringWithFormat:@"%.0fx%.0f %.3f,%.3f %.1fx%.1f %.3f,%.3f-%.3f,%.3f %.2f %.2f %.2f", v.width, v.height, _radialFrom.x, _radialFrom.y,
                     _radialExtent.width, _radialExtent.height, _linearStart.x, _linearStart.y, _linearEnd.x, _linearEnd.y, _maxRadius, _tint, _frost];
    key = [key stringByAppendingFormat:@" %.1fx%.1f %.1f", _coreHalf.width, _coreHalf.height, _padding];
    if ([key isEqualToString:_drawnFor]) return;
    _drawnFor = key;
    CGImageRelease(_ramp); _ramp = [self newMaskWhite:YES alpha:1];
    CGImageRef tint = _tint > 0 ? [self newMaskWhite:NO alpha:_tint] : NULL;
    _tintView.image = tint ? [UIImage imageWithCGImage:tint] : nil; CGImageRelease(tint);
    CGImageRef frost = _frost > 0 ? [self newMaskWhite:YES alpha:_frost] : NULL;
    _frostView.image = frost ? [UIImage imageWithCGImage:frost] : nil; CGImageRelease(frost);
    if (_variable) {
        NSObject *f = make_filter();   // a fresh filter object: Core Animation notices filter changes by identity
        [f setValue:@(_maxRadius) forKey:@"inputRadius"];
        [f setValue:(__bridge id)_ramp forKey:@"inputMaskImage"];
        self.backdrop.filters = f ? @[f] : nil;
    } else {
        _fallbackMask.image = _ramp ? [UIImage imageWithCGImage:_ramp] : nil;
    }
}
@end

@implementation NSString (TOTKReverse)
- (NSString *)reverseString {
    NSMutableString *r = [NSMutableString stringWithCapacity:self.length];
    for (NSInteger i = (NSInteger)self.length - 1; i >= 0; i--) [r appendFormat:@"%C", [self characterAtIndex:(NSUInteger)i]];
    return r;
}
@end

/* One circle of the tile: centre and image size in tile pixels, its image, how long a turn takes and which way. */
typedef struct { CGFloat x, y, size; double period; int dir; } Circle;

@implementation TOTKBootView {
    UIView *_pattern;                 // the tile and the turning circles
    CALayer *_tileLayer;
    NSMutableArray<CALayer *> *_circleLayers;
    UIImage *_tile;                   // the ground under the circles: stripes.png (the lines alone), else tile.png
    BOOL _stripesOnly;                // _tile has no circles in it, so it may move
    CGFloat _period;                  // the tile in the picture's pixels (pattern.json "period")
    Circle _circles[8]; CGImageRef _circleImages[8]; int _ncircles;
    CGFloat _lineX[64], _lineW[64]; CGImageRef _lineImages[64]; int _nlines;   // the lines on their own (pattern.json "lines")
    CGSize _builtFor;                 // the bounds the circle layers were laid out for
    UIView *_panel;                   // the corner block
    UIView *_backing;                 // glass / plaque / band / blur, behind the block
    TOTKVariableBlurView *_vblur;     // the progressive blur styles, full screen
    CALayer *_diamond;
    UILabel *_stage, *_detail, *_percent;
    UIView *_track; CALayer *_fill; CAGradientLayer *_sheen;
    NSString *_stageText, *_detailText; double _progress;
}

- (instancetype)initWithFrame:(CGRect)frame patternFolder:(NSString *)folder {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.backgroundColor = rgb(GROUND, 1);
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _twirlSpeed = 1; _progress = -1; _circleLayers = [NSMutableArray array]; _stageText = @"Starting Hyrule";
    _pattern = [[UIView alloc] initWithFrame:self.bounds]; _pattern.userInteractionEnabled = NO;
    [self addSubview:_pattern];
    [self loadPattern:folder];
    _backing = [UIView new]; _backing.userInteractionEnabled = NO; [self addSubview:_backing];
    _panel = [UIView new]; _panel.userInteractionEnabled = NO; [self addSubview:_panel];
    _diamond = [CALayer layer]; _diamond.bounds = CGRectMake(0, 0, 8, 8); _diamond.borderWidth = 1.4; _diamond.borderColor = rgb(ACCENT, 1).CGColor;
    CAKeyframeAnimation *turn = [CAKeyframeAnimation animationWithKeyPath:@"transform.rotation.z"];
    turn.values = @[@(M_PI_4), @(M_PI_4 + M_PI), @(M_PI_4 + 2 * M_PI)]; turn.duration = 2; turn.repeatCount = HUGE_VALF; turn.removedOnCompletion = NO;
    CAMediaTimingFunction *ease = [CAMediaTimingFunction functionWithControlPoints:0.65 :0 :0.35 :1]; turn.timingFunctions = @[ease, ease];
    [_diamond addAnimation:turn forKey:@"turn"];
    [_panel.layer addSublayer:_diamond];
    _stage = [UILabel new]; _detail = [UILabel new]; _detail.numberOfLines = 2; _percent = [UILabel new]; _percent.textAlignment = NSTextAlignmentRight;
    for (UILabel *l in @[_stage, _detail, _percent]) [_panel addSubview:l];
    _track = [UIView new]; _track.clipsToBounds = YES; _track.layer.cornerRadius = 1; _track.backgroundColor = rgb(ACCENT, 0.18);
    _fill = [CALayer layer]; _fill.backgroundColor = rgb(ACCENT, 1).CGColor; _fill.cornerRadius = 1; [_track.layer addSublayer:_fill];
    _sheen = [CAGradientLayer layer]; _sheen.startPoint = CGPointMake(0, 0.5); _sheen.endPoint = CGPointMake(1, 0.5);
    _sheen.colors = @[(id)rgb(0xfffae6, 0).CGColor, (id)rgb(0xfffae6, 0.85).CGColor, (id)rgb(0xfffae6, 0).CGColor];
    [_track.layer addSublayer:_sheen]; [_panel addSubview:_track];
    self.contrast = TOTKBootContrastFromEnvironment(TOTKBootContrastBlur);
    [self setStage:@"Starting Hyrule" detail:nil progress:-1];
    return self;
}

- (void)dealloc {
    for (int i = 0; i < _ncircles; i++) CGImageRelease(_circleImages[i]);
    for (int i = 0; i < _nlines; i++) CGImageRelease(_lineImages[i]);
}

- (void)loadPattern:(NSString *)folder {
    if (!folder) return;
    NSData *json = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"pattern.json"]];
    NSDictionary *meta = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:nil] : nil;
    /* The wavy lines on their own move under the circles; a pattern without them falls back to the whole tile, still. */
    NSString *ground = [meta isKindOfClass:NSDictionary.class] && [meta[@"stripes"] isKindOfClass:NSString.class] ? meta[@"stripes"] : nil;
    UIImage *tile = ground ? [UIImage imageWithContentsOfFile:[folder stringByAppendingPathComponent:ground]] : nil;
    _stripesOnly = tile.CGImage != NULL;
    if (!_stripesOnly) tile = [UIImage imageWithContentsOfFile:[folder stringByAppendingPathComponent:@"tile.png"]];
    if (![meta isKindOfClass:NSDictionary.class] || !tile.CGImage || [meta[@"period"] doubleValue] <= 0) return;
    _tile = tile; _period = [meta[@"period"] doubleValue];
    NSArray *lines = [meta[@"lines"] isKindOfClass:NSArray.class] ? meta[@"lines"] : nil;
    for (NSDictionary *l in lines) {
        if (_nlines == 64 || ![l isKindOfClass:NSDictionary.class]) break;
        UIImage *im = [UIImage imageWithContentsOfFile:[folder stringByAppendingPathComponent:l[@"file"]]];
        if (!im.CGImage || [l[@"width"] doubleValue] <= 0) continue;
        _lineX[_nlines] = [l[@"x"] doubleValue]; _lineW[_nlines] = [l[@"width"] doubleValue]; _lineImages[_nlines++] = CGImageRetain(im.CGImage);
    }
    /* Each circle its own pace (a turn in 34 s to 140 s) and neighbours the other way round, so none moves in step. */
    static const struct { const char *name; double period; int dir; } pace[] = {{"F", 140, 1}, {"A", 70, -1}, {"B", 82, 1}, {"D", 52, -1}, {"C", 34, 1}};
    for (NSDictionary *c in meta[@"circles"]) {
        if (_ncircles == 8 || ![c isKindOfClass:NSDictionary.class]) break;
        UIImage *im = [UIImage imageWithContentsOfFile:[folder stringByAppendingPathComponent:c[@"file"]]];
        if (!im.CGImage) continue;
        Circle k = {[c[@"x"] doubleValue], [c[@"y"] doubleValue], [c[@"size"] doubleValue], 60, 1};
        for (unsigned i = 0; i < sizeof pace / sizeof *pace; i++) if ([c[@"name"] isEqual:@(pace[i].name)]) { k.period = pace[i].period; k.dir = pace[i].dir; }
        _circles[_ncircles] = k; _circleImages[_ncircles++] = CGImageRetain(im.CGImage);
    }
}

/* The tile is about 0.88 of the screen's shorter side (the size the design was made at), whole circles turning on top
   of it: one layer per circle per tile copy that reaches the screen, laid out again only when the size changes. */
- (void)buildPattern {
    CGSize s = self.bounds.size;
    if (!_tile || CGSizeEqualToSize(s, _builtFor) || s.width < 1) return;
    _builtFor = s;
    [_tileLayer removeFromSuperlayer]; for (CALayer *l in _circleLayers) [l removeFromSuperlayer]; [_circleLayers removeAllObjects];
    CGFloat tilePt = round(MIN(s.width, s.height) * 0.88), k = tilePt / _period;
    UIImage *scaled = [UIImage imageWithCGImage:_tile.CGImage scale:CGImageGetWidth(_tile.CGImage) / tilePt orientation:UIImageOrientationUp];
    int nx = (int)ceil(s.width / tilePt), ny = (int)ceil(s.height / tilePt);
    CFTimeInterval now = CACurrentMediaTime();
    if (_nlines) {
        /* Every line on the screen flows by itself, up or down, a tile every 25 to 90 s: a strip a tile taller than the
           screen at each end, its line repeating down it, sliding one tile and starting over (the line wraps, so the
           jump back is invisible). */
        _tileLayer = [CALayer layer]; _tileLayer.frame = (CGRect){CGPointZero, s};
        for (int tx = -1; tx <= nx; tx++) for (int i = 0; i < _nlines; i++) {
            CGFloat w = _lineW[i] * k, x = (tx + (_lineX[i] - _lineW[i] / 2) / _period) * tilePt;
            if (x + w < 0 || x > s.width) continue;
            UIImage *line = [UIImage imageWithCGImage:_lineImages[i] scale:CGImageGetWidth(_lineImages[i]) / w orientation:UIImageOrientationUp];
            CALayer *l = [CALayer layer]; l.frame = CGRectMake(x, -tilePt, w, s.height + 2 * tilePt);
            l.backgroundColor = [UIColor colorWithPatternImage:line].CGColor;
            if (_twirlSpeed > 0) {
                CABasicAnimation *flow = [CABasicAnimation animationWithKeyPath:@"transform.translation.y"];
                flow.fromValue = @0; flow.toValue = @(arc4random_uniform(2) ? tilePt : -tilePt);
                flow.duration = (25 + arc4random_uniform(6500) / 100.0) / _twirlSpeed;
                flow.repeatCount = HUGE_VALF; flow.removedOnCompletion = NO;
                flow.beginTime = now - arc4random_uniform((uint32_t)(flow.duration * 1000)) / 1000.0;   // each from its own place
                [l addAnimation:flow forKey:@"flow"];
            }
            [_tileLayer addSublayer:l];
        }
    } else {
        /* Older patterns: the lines in one sheet (stripes.png) flowing down together, or the whole picture, still. */
        _tileLayer = [CALayer layer]; _tileLayer.frame = CGRectMake(0, _stripesOnly ? -tilePt : 0, s.width, s.height + (_stripesOnly ? tilePt : 0));
        _tileLayer.backgroundColor = [UIColor colorWithPatternImage:scaled].CGColor;
        if (_stripesOnly && _twirlSpeed > 0) {
            CABasicAnimation *flow = [CABasicAnimation animationWithKeyPath:@"transform.translation.y"];
            flow.fromValue = @0; flow.toValue = @(tilePt); flow.duration = 40 / _twirlSpeed;
            flow.repeatCount = HUGE_VALF; flow.removedOnCompletion = NO;
            [_tileLayer addAnimation:flow forKey:@"flow"];
        }
    }
    [_pattern.layer addSublayer:_tileLayer];
    for (int i = 0; i < _ncircles; i++) {           // pattern.json lists the circle behind the others first
        Circle c = _circles[i]; CGFloat d = c.size * k;
        for (int ty = -1; ty <= ny; ty++) for (int tx = -1; tx <= nx; tx++) {
            CGPoint at = CGPointMake((tx + c.x / _period) * tilePt, (ty + c.y / _period) * tilePt);
            if (at.x + d / 2 < 0 || at.y + d / 2 < 0 || at.x - d / 2 > s.width || at.y - d / 2 > s.height) continue;
            CALayer *l = [CALayer layer]; l.contents = (__bridge id)_circleImages[i]; l.bounds = CGRectMake(0, 0, d, d); l.position = at;
            l.contentsScale = self.traitCollection.displayScale > 0 ? self.traitCollection.displayScale : 2; l.allowsEdgeAntialiasing = YES;
            if (_twirlSpeed > 0) {
                /* every copy its own pace (0.55 to 1.6 times its circle's) and its own way round */
                CABasicAnimation *spin = [CABasicAnimation animationWithKeyPath:@"transform.rotation.z"];
                int dir = arc4random_uniform(2) ? 1 : -1;
                spin.fromValue = @0; spin.toValue = @(2 * M_PI * dir);
                spin.duration = c.period * (0.55 + arc4random_uniform(1050) / 1000.0) / _twirlSpeed;
                spin.repeatCount = HUGE_VALF; spin.removedOnCompletion = NO;
                spin.beginTime = now - arc4random_uniform((uint32_t)(spin.duration * 1000)) / 1000.0;
                [l addAnimation:spin forKey:@"twirl"];
            }
            [_pattern.layer addSublayer:l]; [_circleLayers addObject:l];
        }
    }
}

- (void)setTwirlSpeed:(double)twirlSpeed { _twirlSpeed = twirlSpeed; _builtFor = CGSizeZero; [self setNeedsLayout]; }

- (void)setContrast:(TOTKBootContrast)contrast {
    _contrast = contrast;
    [_backing.subviews makeObjectsPerformSelector:@selector(removeFromSuperview)];
    _backing.backgroundColor = nil; _backing.layer.borderWidth = 0; _backing.layer.cornerRadius = 0; _backing.clipsToBounds = NO;
    _pattern.alpha = contrast == TOTKBootContrastDim ? 0.45 : 1;
    if (contrast == TOTKBootContrastGlass) {
        UIVisualEffectView *blur = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterialDark]];
        blur.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight; blur.frame = _backing.bounds;
        [_backing addSubview:blur];
        _backing.layer.cornerRadius = 14; _backing.clipsToBounds = YES; _backing.layer.borderWidth = 0.5; _backing.layer.borderColor = rgb(STAR, 0.16).CGColor;
    } else if (contrast == TOTKBootContrastPlaque) {
        _backing.backgroundColor = rgb(GROUND, 0.9); _backing.layer.cornerRadius = 10;
        _backing.layer.borderWidth = 0.5; _backing.layer.borderColor = rgb(STAR, 0.3).CGColor;
    } else if (contrast == TOTKBootContrastBand) {
        _backing.backgroundColor = rgb(GROUND, 0.92);
        UIView *rule = [UIView new]; rule.backgroundColor = rgb(STAR, 0.22); rule.autoresizingMask = UIViewAutoresizingFlexibleWidth;
        rule.frame = CGRectMake(0, 0, 1, 0.5); [_backing addSubview:rule];
    }
    /* The progressive blurs: a full-screen variable blur under the block (layout gives it the panel's extent). */
    [_vblur removeFromSuperview]; _vblur = nil;
    BOOL blurred = contrast == TOTKBootContrastBlur || contrast == TOTKBootContrastBlurBand;
    if (blurred) {
        _vblur = [TOTKVariableBlurView new];
        /* Blur alone, no tint or frost: the pattern softened, its own colours kept. */
        if (contrast == TOTKBootContrastBlur) { _vblur.radialFrom = CGPointMake(1, 1); _vblur.maxRadius = 2.6; _vblur.tint = 0; _vblur.frost = 0; }
        else { _vblur.radialFrom = CGPointMake(-1, -1); _vblur.linearStart = CGPointMake(0.5, 0.45); _vblur.linearEnd = CGPointMake(0.5, 1); _vblur.maxRadius = 14; _vblur.tint = 0; _vblur.frost = 0; }
        [self insertSubview:_vblur aboveSubview:_pattern];
    }
    BOOL halo = contrast == TOTKBootContrastHalo || contrast == TOTKBootContrastDim || contrast == TOTKBootContrastBold;
    if (blurred) {   // over the blur: a soft shadow on the whole block (black 35 %, 3 pt, 1 pt down)
        _panel.layer.shadowColor = UIColor.blackColor.CGColor; _panel.layer.shadowOffset = CGSizeMake(0, 1);
        _panel.layer.shadowOpacity = 0.35; _panel.layer.shadowRadius = 3;
    } else {
        _panel.layer.shadowColor = rgb(GROUND, 1).CGColor; _panel.layer.shadowOffset = CGSizeZero;
        _panel.layer.shadowOpacity = halo ? 1 : 0; _panel.layer.shadowRadius = contrast == TOTKBootContrastBold ? 3 : 2.5;
    }
    [self applyText]; [self setNeedsLayout];
}

- (void)setStage:(NSString *)stage detail:(NSString *)detail progress:(double)progress {
    _stageText = stage.length ? stage : @"Starting Hyrule"; _detailText = detail.length ? detail : nil; _progress = progress;
    [self applyText]; [self setNeedsLayout];
}

- (NSAttributedString *)text:(NSString *)s font:(UIFont *)f color:(UIColor *)c kern:(CGFloat)kern {
    NSMutableDictionary *a = [@{NSFontAttributeName: f, NSForegroundColorAttributeName: c, NSKernAttributeName: @(kern)} mutableCopy];
    if (_contrast == TOTKBootContrastOutline) { a[NSStrokeWidthAttributeName] = @-3.5; a[NSStrokeColorAttributeName] = rgb(GROUND, 1); }
    return [[NSAttributedString alloc] initWithString:s attributes:a];
}

- (void)applyText {
    BOOL bold = _contrast == TOTKBootContrastBold;
    UIFont *stageFont = [UIFont fontWithName:bold ? @"AvenirNext-DemiBold" : @"AvenirNext-Regular" size:bold ? 17 : 15] ?: [UIFont systemFontOfSize:15];
    UIFont *detailFont = [UIFont fontWithName:bold ? @"AvenirNext-DemiBold" : @"AvenirNext-Medium" size:bold ? 10.5 : 9.5] ?: [UIFont systemFontOfSize:9.5];
    _stage.attributedText = [self text:_stageText ?: @"" font:stageFont color:rgb(STAR, 1) kern:0.6];
    _detail.attributedText = _detailText ? [self text:_detailText.uppercaseString font:detailFont color:rgb(STAR_DIM, 1) kern:1.8] : nil;
    _percent.attributedText = _progress >= 0 ? [self text:[NSString stringWithFormat:@"%d%%", (int)floor(fmin(_progress, 1) * 100)]
                                                     font:[UIFont monospacedDigitSystemFontOfSize:bold ? 13 : 12 weight:bold ? UIFontWeightSemibold : UIFontWeightRegular]
                                                    color:rgb(STAR, 1) kern:0] : nil;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    _pattern.frame = self.bounds;
    [self buildPattern];
    CGSize s = self.bounds.size; UIEdgeInsets safe = self.safeAreaInsets;
    BOOL pad = MIN(s.width, s.height) > 600;
    CGFloat W = pad ? 320 : 270, x = 0, y = 0;
    BOOL hasBar = _progress >= 0;
    CGFloat percentW = hasBar ? 44 : 0;
    CGFloat rowH = ceil(MAX(_stage.attributedText.size.height, 20));
    _diamond.position = CGPointMake(4, rowH / 2);
    _stage.frame = CGRectMake(18, 0, W - 18 - percentW, rowH);
    _percent.frame = CGRectMake(W - percentW, 0, percentW, rowH); _percent.hidden = !hasBar;
    y = rowH + 8;
    _track.hidden = !hasBar;
    if (hasBar) {
        _track.frame = CGRectMake(x, y, W, 2);
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        _fill.frame = CGRectMake(0, 0, W * fmax(0, fmin(_progress, 1)), 2);
        _sheen.frame = CGRectMake(-50, 0, 50, 2);
        [CATransaction commit];
        if (![_sheen animationForKey:@"sheen"]) {
            CABasicAnimation *run = [CABasicAnimation animationWithKeyPath:@"position.x"];
            run.fromValue = @(-25); run.toValue = @(W + 25); run.duration = 1.8; run.repeatCount = HUGE_VALF; run.removedOnCompletion = NO;
            run.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
            [_sheen addAnimation:run forKey:@"sheen"];
        }
        y += 2 + 8;
    }
    CGFloat detailH = _detail.attributedText ? ceil([_detail.attributedText boundingRectWithSize:CGSizeMake(W, 100) options:NSStringDrawingUsesLineFragmentOrigin context:nil].size.height) : 0;
    _detail.frame = CGRectMake(0, y, W, detailH); _detail.hidden = !detailH;
    CGFloat H = detailH ? y + detailH : rowH;
    CGFloat margin = pad ? 36 : 28;
    _panel.frame = CGRectMake(s.width - MAX(safe.right, 0) - margin - W, s.height - MAX(safe.bottom, 8) - margin * 0.8 - H, W, H);
    CGRect p = _panel.frame;
    switch (_contrast) {
    case TOTKBootContrastGlass: case TOTKBootContrastPlaque: _backing.frame = CGRectInset(p, -16, -13); break;
    case TOTKBootContrastBand: _backing.frame = CGRectMake(0, p.origin.y - 18, s.width, s.height - p.origin.y + 18); break;
    default: _backing.frame = CGRectZero;
    }
    if (_vblur) {
        _vblur.frame = self.bounds;
        if (_contrast == TOTKBootContrastBlur) {
            /* only under the text: full strength over the middle of the block (25 pt in from its edges), then easing out
               (smoothstep) over the next 100 pt */
            _vblur.radialFrom = CGPointMake(CGRectGetMidX(p) / s.width, CGRectGetMidY(p) / s.height);
            _vblur.coreHalf = CGSizeMake(p.size.width / 2-25, p.size.height / 2 -25);
            _vblur.padding = 100;
        }
        [_vblur refresh];
    }
}

- (void)dismiss {
    [UIView animateWithDuration:0.45 animations:^{ self.alpha = 0; } completion:^(BOOL done) { (void)done; [self removeFromSuperview]; }];
}
@end
#endif
