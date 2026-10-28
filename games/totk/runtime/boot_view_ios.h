/* The iPhone / iPad boot screen (frontend_ios.m shows it until the game's first frame; ios/tools/boot_preview runs it in
   the Simulator on its own). */
#import <UIKit/UIKit.h>

/* How the text in the corner stands off the pattern. TOTK_BOOT_STYLE (a number or the name) or the defaults key
   TOTKBootStyle picks one; the preview cycles them. */
typedef NS_ENUM(NSInteger, TOTKBootContrast) {
    TOTKBootContrastHalo,     // a tight dark halo around each glyph, nothing behind the text
    TOTKBootContrastGlass,    // a small frosted card
    TOTKBootContrastPlaque,   // a small solid card in the pattern's ground colour, hairline border
    TOTKBootContrastBand,     // a full-width band along the bottom edge
    TOTKBootContrastDim,      // the whole pattern dimmed, nothing behind the text
    TOTKBootContrastOutline,  // the glyphs outlined in the ground colour
    TOTKBootContrastBold,     // larger, heavier type with the halo
    TOTKBootContrastBlur,     // a progressive blur: strongest behind the text, fading out towards the middle
    TOTKBootContrastBlurBand, // a progressive blur along the bottom edge, clear above
    TOTKBootContrastCount
};
NSString *TOTKBootContrastName(TOTKBootContrast style);
TOTKBootContrast TOTKBootContrastFromEnvironment(TOTKBootContrast fallback);

@interface TOTKBootView : UIView
/* folder: the launch pattern (tile.png, circle-*.png, pattern.json from ios/tools/launch_pattern.py); nil or missing:
   the plain ground colour. */
- (instancetype)initWithFrame:(CGRect)frame patternFolder:(NSString *)folder;
@property (nonatomic) TOTKBootContrast contrast;
@property (nonatomic) double twirlSpeed;   // 1 = the designed pace; 0 holds the circles still
/* A step with no measure (progress < 0) or with one (0..1). detail: a second line, may be nil. */
- (void)setStage:(NSString *)stage detail:(NSString *)detail progress:(double)progress;
/* Fades out and removes itself. */
- (void)dismiss;
@end
