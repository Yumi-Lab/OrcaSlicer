#import "MacScreenshot.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

namespace Slic3r { namespace GUI {

bool capture_window_screenshot(void* nswindow_handle, const std::string& filepath)
{
    if (!nswindow_handle)
        return false;

    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nswindow_handle;
        CGWindowID windowID = (CGWindowID)[window windowNumber];

        // Capture the window content (no shadow/decorations beyond content)
        CGImageRef cgImage = CGWindowListCreateImage(
            CGRectNull,
            kCGWindowListOptionIncludingWindow,
            windowID,
            kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution
        );

        if (!cgImage)
            return false;

        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cgImage];
        NSData* pngData = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        BOOL ok = [pngData writeToFile:[NSString stringWithUTF8String:filepath.c_str()] atomically:YES];

        CGImageRelease(cgImage);
        return ok == YES;
    }
}

}} // namespace Slic3r::GUI
