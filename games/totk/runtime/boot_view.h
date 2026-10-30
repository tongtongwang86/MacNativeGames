/* The Mac boot screen, after the iPhone / iPad one (boot_view_ios.m): frontend.m shows it until the game's first frame. */
#import <Cocoa/Cocoa.h>

@interface TOTKMacBootView : NSView
/* folder: the launch pattern (tile.png, circle-*.png, line-*.png, pattern.json from ios/tools/launch_pattern.py); nil or
   missing: the plain ground colour. */
- (instancetype)initWithFrame:(NSRect)frame patternFolder:(NSString *)folder;
/* A step with no measure (progress < 0) or with one (0..1). detail: a second line, may be nil. */
- (void)setStage:(NSString *)stage detail:(NSString *)detail progress:(double)progress;
/* Fades out and removes itself. */
- (void)dismiss;
@end
