#include "icon.h"
#import <AppKit/AppKit.h>
#include <cstdio>
#include <stdexcept>
#include <thread>

static void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FixtureDirectory {
    NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    ~FixtureDirectory() {
        NSFileManager* fm = NSFileManager.defaultManager;
        if ([fm fileExistsAtPath:path] && ![fm removeItemAtPath:path error:nil])
            std::fprintf(stderr, "Cannot clean native test fixture directory\n");
    }
};

static std::u16string nativePath(NSString* path) {
    std::u16string result(path.length, u'\0');
    [path getCharacters:reinterpret_cast<unichar*>(result.data()) range:NSMakeRange(0, path.length)];
    return result;
}

static void createAlias(NSString* target, NSString* alias) {
    NSURL* targetURL = [NSURL fileURLWithPath:target];
    NSData* bookmark = [targetURL bookmarkDataWithOptions:
        NSURLBookmarkCreationSuitableForBookmarkFile
        includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    require(bookmark != nil, "Cannot create alias bookmark");
    require([NSURL writeBookmarkData:bookmark toURL:[NSURL fileURLWithPath:alias]
        options:0 error:nil], "Cannot write Finder alias");
}

static NSBitmapImageRep* decode(const icons::Result& result, int size) {
    if (!result)
        std::fprintf(stderr, "%s\n", result.error.c_str());
    require(bool(result), "Expected encoded PNG");
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    require(result.png.size() > 26 && !memcmp(signature, result.png.data(), 8), "Not PNG");
    require(result.png[24] == 8 && result.png[25] == 6, "PNG must carry 8-bit RGBA");
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithData:
        [NSData dataWithBytes:result.png.data() length:result.png.size()]];
    require(bitmap && bitmap.pixelsWide == size && bitmap.pixelsHigh == size, "Wrong pixel dimensions");
    require(bitmap.hasAlpha, "PNG lost alpha");
    return bitmap;
}

static std::vector<unsigned char> raster(CGImageRef image) {
    std::vector<unsigned char> rgba(256 * 256 * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(rgba.data(), 256, 256, 8, 1024, space,
        CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    require(context, "Cannot render reference");
    CGContextSetInterpolationQuality(context,
        CGImageGetWidth(image) < 256 && CGImageGetHeight(image) < 256
            ? kCGInterpolationNone : kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0, 0, 256, 256), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return rgba;
}

static void requireTargetArtwork(const std::vector<unsigned char>& actual,
                                 const std::vector<unsigned char>& expected) {
    // The native badge occupies the lower left, not the upper-right target artwork.
    for (int y = 128; y < 256; ++y) {
        for (int x = 128; x < 256; ++x) {
            for (int channel = 0; channel < 4; ++channel) {
                int offset = (y * 256 + x) * 4 + channel;
                require(std::abs(int(actual[offset]) - int(expected[offset])) <= 2,
                        "Alias replaced the target artwork with a generic document");
            }
        }
    }
}

int main() try {
    @autoreleasepool {
        [NSApplication sharedApplication];
        FixtureDirectory fixture;
        NSString* root = fixture.path;
        NSFileManager* fm = NSFileManager.defaultManager;
        require([fm createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil],
                "Cannot create fixture directory");
        NSString* folder = [root stringByAppendingPathComponent:@"folder-\u65e5\u672c\u8a9e"];
        NSString* file = [root stringByAppendingPathComponent:@"file-\u65e5\u672c\u8a9e.txt"];
        require([fm createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil],
                "Cannot create Unicode folder");
        require([@"fixture" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil],
                "Cannot create Unicode file");
        for (int size : {1, 16, 31, 32, 127, 256, 513}) {
            decode(icons::load(nativePath(file), size, icons::Kind::file), size);
            decode(icons::load(nativePath(folder), size, icons::Kind::folder), size);
        }
        require(!icons::load(nativePath(file), 0, icons::Kind::file), "Zero native size accepted");
        require(!icons::load(nativePath(file), -1, icons::Kind::file), "Negative native size accepted");
        require(!icons::load(nativePath(file), INT32_MAX, icons::Kind::file), "Overflow size accepted");
        require(!icons::load(nativePath(file), 256, icons::Kind::folder), "File accepted as folder");
        require(!icons::load(nativePath(folder), 256, icons::Kind::file), "Folder accepted as file");
        require(!icons::validSize(32768) && icons::validSize(32767), "Wrong allocation limit");

        NSString* plainAlias = [root stringByAppendingPathComponent:@"plain-folder-alias"];
        createAlias(folder, plainAlias);
        for (int size : {32, 128, 256, 513}) {
            auto expected = raster(decode(icons::load(nativePath(folder), size,
                icons::Kind::folder), size).CGImage);
            for (auto kind : {icons::Kind::file, icons::Kind::folder}) {
                auto actual = raster(decode(icons::load(nativePath(plainAlias), size, kind), size).CGImage);
                requireTargetArtwork(actual, expected);
                require(actual != expected, "Folder alias has no arrow");
            }
        }

        NSBitmapImageRep* art = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
            pixelsWide:32 pixelsHigh:32 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
            isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bitmapFormat:0 bytesPerRow:128 bitsPerPixel:32];
        memset(art.bitmapData, 0, 4096);
        for (int y = 8; y < 24; ++y) {
            for (int x = 8; x < 24; ++x) {
                unsigned char* p = art.bitmapData + y * art.bytesPerRow + x * 4;
                p[0] = 110; p[1] = 40; p[2] = 20; p[3] = 128;
            }
        }
        NSImage* custom = [[NSImage alloc] initWithSize:NSMakeSize(32, 32)];
        [custom addRepresentation:art];
        for (NSString* target in @[file, folder]) {
            require([[NSWorkspace sharedWorkspace] setIcon:custom forFile:target options:0],
                    "Cannot set Finder custom icon");
            auto result = icons::load(nativePath(target), 256,
                [target isEqual:file] ? icons::Kind::file : icons::Kind::folder);
            NSBitmapImageRep* bitmap = decode(result, 256);
            NSColor* center = [[bitmap colorAtX:128 y:128] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            NSColor* corner = [bitmap colorAtX:0 y:0];
            require(center.alphaComponent > .40 && center.alphaComponent < .60, "Custom icon alpha incorrect");
            require(corner.alphaComponent < .01, "Custom icon margins lost transparency");
            NSImage* supplied = [[NSWorkspace sharedWorkspace] iconForFile:target];
            NSRect rect = NSMakeRect(0, 0, 256, 256);
            auto reference = raster([supplied CGImageForProposedRect:&rect context:nil hints:nil]);
            auto actual = raster(bitmap.CGImage);
            // Finder can transform the custom artwork when installing its icon.
            // Compare against the actual OS-provided image, not the pre-install bytes.
            for (int x : {64, 65, 128, 190, 191}) {
                for (int channel = 0; channel < 4; ++channel) {
                    int offset = (128 * 256 + x) * 4 + channel;
                    require(std::abs(int(reference[offset]) - int(actual[offset])) <= 5,
                            "PNG changed native premultiplied edge color");
                }
            }
            for (int i = 0; i < 50; ++i)
                decode(icons::load(nativePath(target), 32, [target isEqual:file]
                    ? icons::Kind::file : icons::Kind::folder), 32);
        }
        for (NSString* target in @[file, folder]) {
            NSString* alias = [target stringByAppendingString:@".alias"];
            createAlias(target, alias);
            NSString* folderPath = [alias stringByAppendingString:@"/"];
            if ([target isEqual:file]) {
                for (auto policy : {icons::OverlayPolicy::system,
                                    icons::OverlayPolicy::noShortcut, icons::OverlayPolicy::none})
                    require(!icons::load(nativePath(folderPath), 256, icons::Kind::folder, policy),
                            "Alias to a file accepted as a folder");
            }
            for (int size : {32, 128, 256}) {
                NSBitmapImageRep* plain = decode(icons::load(nativePath(alias), size,
                    icons::Kind::file, icons::OverlayPolicy::noShortcut), size);
                NSBitmapImageRep* none = decode(icons::load(nativePath(alias), size,
                    icons::Kind::file, icons::OverlayPolicy::none), size);
                require(raster(none.CGImage) == raster(plain.CGImage),
                        "No-overlays policy did not suppress the Finder alias arrow");
                NSBitmapImageRep* expected = decode(icons::load(nativePath(target), size,
                    [target isEqual:file] ? icons::Kind::file : icons::Kind::folder), size);
                require(raster(plain.CGImage) == raster(expected.CGImage),
                        "Suppressed Finder alias does not match target icon");
                NSBitmapImageRep* decorated = decode(icons::load(nativePath(alias), size,
                    icons::Kind::file), size);
                require(raster(decorated.CGImage) != raster(plain.CGImage),
                        "Finder alias arrow is missing by default");
                requireTargetArtwork(raster(decorated.CGImage), raster(expected.CGImage));
                if ([target isEqual:folder]) {
                    for (NSString* input in @[alias, folderPath,
                                              [folderPath stringByAppendingString:@"/"]]) {
                        for (auto policy : {icons::OverlayPolicy::system,
                                            icons::OverlayPolicy::noShortcut, icons::OverlayPolicy::none}) {
                            NSBitmapImageRep* folderIcon = decode(icons::load(nativePath(input), size,
                                icons::Kind::folder, policy), size);
                            require(raster(folderIcon.CGImage) == raster(
                                policy == icons::OverlayPolicy::system ? decorated.CGImage : plain.CGImage),
                                "Folder alias path changed the icon or its overlay policy");
                        }
                    }
                    for (int y = 8; y < 24; ++y) {
                        for (int x = 8; x < 24; ++x) {
                            unsigned char* p = art.bitmapData + y * art.bytesPerRow + x * 4;
                            p[0] = 20; p[1] = 110; p[2] = 40; p[3] = 255;
                        }
                    }
                    require([[NSWorkspace sharedWorkspace] setIcon:custom forFile:plainAlias options:0],
                            "Cannot set alias-specific custom icon");
                    NSRect proposed = NSMakeRect(0, 0, 256, 256);
                    auto customAliasPixels = raster([[[NSWorkspace sharedWorkspace] iconForFile:plainAlias]
                        CGImageForProposedRect:&proposed context:nil hints:nil]);
                    for (auto kind : {icons::Kind::file, icons::Kind::folder}) {
                        auto actual = raster(decode(icons::load(nativePath(plainAlias), 256, kind), 256).CGImage);
                        requireTargetArtwork(actual, customAliasPixels);
                        for (auto policy : {icons::OverlayPolicy::noShortcut, icons::OverlayPolicy::none}) {
                            auto suppressed = raster(decode(icons::load(nativePath(plainAlias), 256,
                                kind, policy), 256).CGImage);
                            auto targetPixels = raster(decode(icons::load(nativePath(folder), 256,
                                icons::Kind::folder), 256).CGImage);
                            require(suppressed == targetPixels, "Suppression retained alias-specific custom artwork");
                            require(suppressed != actual, "Default lost alias-specific custom artwork");
                        }
                    }
                }
            }
        }
        bool rejected = false;
        std::thread worker([&] {
            rejected = !icons::load(nativePath(file), 256, icons::Kind::file);
        });
        worker.join();
        require(rejected, "Off-main AppKit call not rejected");
        require([fm removeItemAtPath:file error:nil], "Cannot delete fixture");
        require(!icons::load(nativePath(file), 256, icons::Kind::file), "Deleted file has icon");
        require([fm removeItemAtPath:folder error:nil], "Cannot delete folder target");
        NSString* brokenFolderAlias = [[folder stringByAppendingString:@".alias"]
            stringByAppendingString:@"/"];
        for (auto policy : {icons::OverlayPolicy::system,
                            icons::OverlayPolicy::noShortcut, icons::OverlayPolicy::none}) {
            auto result = icons::load(nativePath(brokenFolderAlias), 256, icons::Kind::folder, policy);
            require(!result && !result.error.empty(),
                    "Broken folder alias must fail with an explicit diagnostic");
            auto fileResult = icons::load(nativePath([file stringByAppendingString:@".alias"]),
                256, icons::Kind::file, policy);
            require(!fileResult && !fileResult.error.empty(),
                    "Broken file alias must fail with an explicit diagnostic");
        }
        require([fm removeItemAtPath:root error:nil], "Cannot clean fixtures");
        std::puts("PASS: native dimensions, alpha, Unicode, custom 32px icons, folder aliases, failures, repeated calls");
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
