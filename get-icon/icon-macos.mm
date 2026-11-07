#include "icon.h"
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstring>

namespace icons {
namespace {
template<typename T> struct Owned {
    T value;
    ~Owned() { if (value) CFRelease(value); }
};

bool aliasHasCustomIcon(NSString* path, bool& custom) {
    // NSURLCustomIconKey is documented as unimplemented; use the native Finder flag.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    FSRef ref;
    FSCatalogInfo catalog{};
    OSStatus status = FSPathMakeRef(
        reinterpret_cast<const UInt8*>(path.fileSystemRepresentation), &ref, nullptr);
    if (status == noErr)
        status = FSGetCatalogInfo(&ref, kFSCatInfoFinderInfo, &catalog, nullptr, nullptr, nullptr);
#pragma clang diagnostic pop
    if (status != noErr)
        return false;
    FileInfo info{};
    std::memcpy(&info, catalog.finderInfo, sizeof(info));
    custom = (info.finderFlags & kHasCustomIcon) != 0;
    return true;
}
}

Result load(const std::u16string& path, std::int32_t size, OverlayPolicy overlays) {
    @autoreleasepool {
        if (![NSThread isMainThread])
            return {{}, "AppKit extraction requires the Cocoa main thread"};
        if (!validSize(size))
            return {{}, "Invalid image size"};
        if (overlays != OverlayPolicy::system && overlays != OverlayPolicy::noShortcut &&
            overlays != OverlayPolicy::none)
            return {{}, "Invalid icon overlay policy"};
        if (path.empty() || path.find(u'\0') != std::u16string::npos)
            return {{}, "Invalid path"};
        NSString* file = [[NSString alloc] initWithCharacters:
            reinterpret_cast<const unichar*>(path.data()) length:path.size()];
        // 4D Folder paths end in '/', but a Finder folder alias is a file on disk.
        while (file.length > 1 && [file hasSuffix:@"/"])
            file = [file substringToIndex:file.length - 1];
        if (![[NSFileManager defaultManager] fileExistsAtPath:file])
            return {{}, "Path is missing"};
        NSImage* badge = nil;
        {
            NSURL* url = [NSURL fileURLWithPath:file];
            NSNumber* alias = nil;
            NSError* error = nil;
            if (![url getResourceValue:&alias forKey:NSURLIsAliasFileKey error:&error])
                return {{}, "Cannot read Finder alias metadata"};
            if (alias.boolValue) {
                bool custom = false;
                if (overlays == OverlayPolicy::system &&
                    !aliasHasCustomIcon(file, custom))
                    return {{}, "Cannot read Finder alias custom icon metadata"};
                NSURL* target = [NSURL URLByResolvingAliasFileAtURL:url
                    options:NSURLBookmarkResolutionWithoutUI |
                            NSURLBookmarkResolutionWithoutMounting error:&error];
                if (!target)
                    return {{}, "Cannot resolve Finder alias target"};
                if (![[NSFileManager defaultManager] fileExistsAtPath:target.path])
                    return {{}, "Finder alias target is missing"};
                if (overlays != OverlayPolicy::system || !custom)
                    file = target.path;
                if (overlays == OverlayPolicy::system && !custom) {
                    // NSWorkspace can return generic document artwork for an alias.
                    // Use the target artwork and the OS's full-canvas alias badge.
                    NSBundle* types = [NSBundle bundleWithPath:
                        @"/System/Library/CoreServices/CoreTypes.bundle"];
                    NSString* badgePath = [types pathForResource:@"AliasBadgeIcon" ofType:@"icns"];
                    if (!badgePath)
                        return {{}, "Cannot locate the system Finder alias badge"};
                    badge = [[NSImage alloc] initWithContentsOfFile:badgePath];
                    if (!badge)
                        return {{}, "Cannot load the system Finder alias badge"};
                }
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
        if (badge) {
            CGImageRef overlay = [badge CGImageForProposedRect:&proposed context:nil hints:nil];
            if (!overlay || CGImageGetWidth(overlay) == 0 || CGImageGetHeight(overlay) == 0)
                return {{}, "Cannot obtain Finder alias badge pixels"};
            CGContextSetInterpolationQuality(context.value, kCGInterpolationHigh);
            CGContextDrawImage(context.value, CGRectMake(0, 0, size, size), overlay);
        }
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
