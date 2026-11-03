#include "icon.h"
#import <AppKit/AppKit.h>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static std::u16string nativePath(NSString* path) {
    std::u16string result(path.length, u'\0');
    [path getCharacters:reinterpret_cast<unichar*>(result.data()) range:NSMakeRange(0, path.length)];
    return result;
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
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0, 0, 256, 256), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return rgba;
}

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
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
        require(!icons::validSize(23171) && icons::validSize(23170), "Wrong allocation limit");

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
        bool rejected = false;
        std::thread worker([&] {
            rejected = !icons::load(nativePath(file), 256, icons::Kind::file);
        });
        worker.join();
        require(rejected, "Off-main AppKit call not rejected");
        require([fm removeItemAtPath:file error:nil], "Cannot delete fixture");
        require(!icons::load(nativePath(file), 256, icons::Kind::file), "Deleted file has icon");
        require([fm removeItemAtPath:root error:nil], "Cannot clean fixtures");
        std::puts("PASS: native dimensions, alpha, Unicode, custom 32px icons, failures, repeated calls");
    }
}
