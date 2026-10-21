// svg_to_pdf.m <svg> <png> <pdf>: draws an SVG with AppKit to a PNG preview and a vector PDF of its viewBox size (make_procon_pdf.sh).
#import <AppKit/AppKit.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        NSImage *img = [[NSImage alloc] initWithContentsOfFile:@(argv[1])]; if (!img) { fprintf(stderr, "cannot load\n"); return 1; }
        NSSize s = img.size; fprintf(stderr, "size %.1f x %.1f\n", s.width, s.height);
        NSBitmapImageRep *r = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)s.width pixelsHigh:(NSInteger)s.height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:r];
        [[NSColor colorWithWhite:0.5 alpha:1] setFill]; NSRectFill(NSMakeRect(0, 0, s.width, s.height));
        [img drawInRect:NSMakeRect(0, 0, s.width, s.height)]; [NSGraphicsContext restoreGraphicsState];
        [[r representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[2]) atomically:YES];
        if (argc > 3) {
            CGRect box = CGRectMake(0, 0, s.width, s.height);
            CGContextRef pdf = CGPDFContextCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[3])], &box, NULL);
            CGPDFContextBeginPage(pdf, NULL);
            [NSGraphicsContext saveGraphicsState]; NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithCGContext:pdf flipped:NO];
            [img drawInRect:NSMakeRect(0, 0, s.width, s.height)]; [NSGraphicsContext restoreGraphicsState];
            CGPDFContextEndPage(pdf); CGPDFContextClose(pdf); CGContextRelease(pdf);
        }
    }
    return 0;
}
