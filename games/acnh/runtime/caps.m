/* caps:su (IScreenShotApplicationService): the game saving a screenshot to the console's album, as ACNH does for
   photos taken with the phone camera. Without the service the runtime stopped ("caps:su not implemented") when a
   photo was taken. A photo is saved as a PNG in <profile>/album/ (TOTK_PROFILE_DIR, else the working directory) and
   the game is told it worked. Commands: 32 SetShimLibraryVersion; 203 / 205 / 206 / 210 SaveScreenShotEx0..Ex3
   (attribute, report option, the image: 1280x720 RGBA in the first buffer large enough; out: an album entry). */
#include "runtime.h"
#include "ipc.h"
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#define SHOT_W 1280
#define SHOT_H 720

static const IpcBuffer *image_buffer(const IpcRequest *rq) {
    for (unsigned i = 0; i < rq->na; i++) if (rq->a[i].size >= SHOT_W * SHOT_H * 4) return &rq->a[i];
    for (unsigned i = 0; i < rq->nx; i++) if (rq->x[i].size >= SHOT_W * SHOT_H * 4) return &rq->x[i];
    return NULL;
}
static void save_png(const IpcBuffer *b) {
    const char *dir = getenv("TOTK_PROFILE_DIR");
    NSString *folder = [[NSString stringWithUTF8String:dir && *dir ? dir : "."] stringByAppendingPathComponent:@"album"];
    [[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *f = [NSDateFormatter new]; f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]; f.dateFormat = @"yyyyMMdd-HHmmss-SSS";
    NSString *path = [folder stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.png", [f stringFromDate:[NSDate date]]]];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate((void *)(uintptr_t)b->addr, SHOT_W, SHOT_H, 8, SHOT_W * 4, cs,
                                             (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CGImageDestinationRef dst = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
    if (img) CGImageRelease(img);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    fprintf(stderr, "[acnh] album: %s %s\n", dst ? "saved a photo to" : "could not save a photo to", path.UTF8String);
}
static void service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 32: return;                                   /* SetShimLibraryVersion */
    case 203: case 205: case 206: case 210: {          /* SaveScreenShotEx0 .. Ex3 */
        const IpcBuffer *b = image_buffer(rq);
        if (b) @autoreleasepool { save_png(b); }
        else fprintf(stderr, "[acnh] album: screenshot command %u without an image buffer, ignored\n", rq->cmd);
        uint8_t entry[0x20] = {0}; IPC_OUT(rs, entry); /* ApplicationAlbumEntry */
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
/* caps:u (IAlbumApplicationService): the game reading its album. It is reported empty: 32 SetShimLibraryVersion; the
   file-list commands (102, 140..144) answer 0 entries. Other commands are logged and answer success with no data,
   instead of stopping the game. */
static void album(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 32: return;
    case 102: case 140: case 141: case 142: case 143: case 144: { uint64_t count = 0; IPC_OUT(rs, count); return; }
    default: fprintf(stderr, "[acnh] album: caps:u command %u answered empty\n", rq->cmd); return;
    }
}
__attribute__((constructor)) static void register_caps(void) {
    runtime_ipc_register("caps:su", service, NULL, NULL);
    runtime_ipc_register("caps:u", album, NULL, NULL);
}
