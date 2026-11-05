#include "icon.h"
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>

namespace icons {
namespace {
template<typename T> struct Owned {
    T value;
    ~Owned() { if (value) CFRelease(value); }
};
}

Result load(const std::u16string& path, std::int32_t size, Kind kind, bool hideShortcut) {
    @autoreleasepool {
        if (![NSThread isMainThread])
            return {{}, "AppKit extraction requires the Cocoa main thread"};
        if (!validSize(size))
            return {{}, "Invalid image size"};
        if (path.empty() || path.find(u'\0') != std::u16string::npos)
            return {{}, "Invalid path"};
        NSString* file = [[NSString alloc] initWithCharacters:
            reinterpret_cast<const unichar*>(path.data()) length:path.size()];
        BOOL directory = NO;
        if (![[NSFileManager defaultManager] fileExistsAtPath:file isDirectory:&directory] ||
            bool(directory) != (kind == Kind::folder))
            return {{}, "Path is missing or has the wrong file/folder kind"};
        if (hideShortcut) {
            NSURL* url = [NSURL fileURLWithPath:file];
            NSNumber* alias = nil;
            NSError* error = nil;
            if (![url getResourceValue:&alias forKey:NSURLIsAliasFileKey error:&error])
                return {{}, "Cannot read Finder alias metadata"};
            if (alias.boolValue) {
                NSURL* target = [NSURL URLByResolvingAliasFileAtURL:url
                    options:NSURLBookmarkResolutionWithoutUI |
                            NSURLBookmarkResolutionWithoutMounting error:&error];
                if (!target)
                    return {{}, "Cannot resolve Finder alias target"};
                file = target.path;
            }
        }
        NSImage* image = [[NSWorkspace sharedWorkspace] iconForFile:file];
        if (!image)
            return {{}, "NSWorkspace returned no icon"};
        NSRect proposed = NSMakeRect(0, 0, size, size);
        CGImageRef source = [image CGImageForProposedRect:&proposed context:nil hints:nil];
        if (!source || CGImageGetWidth(source) == 0 || CGImageGetHeight(source) == 0)
            return {{}, "Cannot obtain native icon pixels"};
        Owned<CGColorSpaceRef> color{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
        if (!color.value)
            return {{}, "Cannot create sRGB color space"};
        Owned<CGContextRef> context{CGBitmapContextCreate(nullptr, size, size, 8,
            std::size_t(size) * 4, color.value,
            CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big)};
        if (!context.value)
            return {{}, "Cannot allocate RGBA bitmap"};
        CGContextClearRect(context.value, CGRectMake(0, 0, size, size));
        const double width = CGImageGetWidth(source), height = CGImageGetHeight(source);
        const double scale = std::min(size / width, size / height);
        CGContextSetInterpolationQuality(context.value,
            scale > 1 ? kCGInterpolationNone : kCGInterpolationHigh);
        CGContextDrawImage(context.value, CGRectMake((size - width * scale) / 2,
            (size - height * scale) / 2, width * scale, height * scale), source);
        Owned<CGImageRef> rendered{CGBitmapContextCreateImage(context.value)};
        Owned<CFMutableDataRef> png{CFDataCreateMutable(kCFAllocatorDefault, 0)};
        if (!rendered.value || !png.value)
            return {{}, "Cannot allocate PNG encoder input"};
        Owned<CGImageDestinationRef> encoder{CGImageDestinationCreateWithData(
            png.value, CFSTR("public.png"), 1, nullptr)};
        if (!encoder.value)
            return {{}, "Cannot create PNG encoder"};
        CGImageDestinationAddImage(encoder.value, rendered.value, nullptr);
        if (!CGImageDestinationFinalize(encoder.value))
            return {{}, "PNG encoding failed"};
        const CFIndex length = CFDataGetLength(png.value);
        if (length <= 0 || length > INT32_MAX)
            return {{}, "PNG encoding failed or exceeds SDK length"};
        const auto* bytes = CFDataGetBytePtr(png.value);
        return {std::vector<unsigned char>(bytes, bytes + length), {}};
    }
}
}
