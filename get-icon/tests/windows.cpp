// Native Windows integration tests. Fixtures live under the working directory,
// never in a system temporary directory; no installed file associations change.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../icon.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void check(HRESULT hr, const char* operation)
{
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " HRESULT=" +
                                 std::to_string(static_cast<unsigned long>(hr)));
}

struct Apartment {
    HRESULT hr;
    explicit Apartment(DWORD mode) : hr(CoInitializeEx(nullptr, mode))
    {
        require(hr == S_OK || hr == S_FALSE, "COM initialization failed");
    }
    ~Apartment() { CoUninitialize(); }
};

struct Fixtures {
    fs::path root;
    Fixtures()
    {
        GUID id{};
        check(CoCreateGuid(&id), "CoCreateGuid");
        wchar_t name[64]{};
        require(StringFromGUID2(id, name, 64) != 0, "StringFromGUID2 failed");
        root = fs::current_path() /
            (std::wstring(L"native-icon-tests-\u65e5\u672c\u8a9e-\u00e9-") + name);
        require(fs::create_directory(root), "Create fixture directory failed");
    }
    ~Fixtures()
    {
        std::error_code error;
        // desktop.ini is hidden/system and customized folders are read-only.
        fs::recursive_directory_iterator it(root, error), end;
        while (!error && it != end) {
            SetFileAttributesW(it->path().c_str(), FILE_ATTRIBUTE_NORMAL);
            it.increment(error);
        }
        SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_NORMAL);
        fs::remove_all(root, error);
        if (error) std::cerr << "Fixture cleanup failed: " << error.message() << '\n';
    }
};

void write(const fs::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    require(static_cast<bool>(out), "Write fixture failed");
}

void append16(std::vector<unsigned char>& out, unsigned value)
{
    out.push_back(static_cast<unsigned char>(value));
    out.push_back(static_cast<unsigned char>(value >> 8));
}

void append32(std::vector<unsigned char>& out, unsigned value)
{
    append16(out, value);
    append16(out, value >> 16);
}

std::vector<unsigned char> legacyIcon()
{
    std::vector<unsigned char> out;
    append16(out, 0); append16(out, 1); append16(out, 1);
    out.insert(out.end(), {32, 32, 0, 0});
    append16(out, 1); append16(out, 32);
    append32(out, 40 + 32 * 32 * 4 + 32 * 4);
    append32(out, 22);
    append32(out, 40); append32(out, 32); append32(out, 64);
    append16(out, 1); append16(out, 32);
    append32(out, BI_RGB); append32(out, 32 * 32 * 4);
    append32(out, 0); append32(out, 0); append32(out, 0); append32(out, 0);
    // ICO DIB pixels have straight BGRA. Half the colored square is translucent
    // with R>A, distinguishing straight pixels from premultiplied pixels.
    for (unsigned row = 0; row < 32; ++row) {
        const unsigned y = 31 - row;
        for (unsigned x = 0; x < 32; ++x) {
            if (x >= 8 && x < 24 && y >= 8 && y < 24)
                out.insert(out.end(), {32, 96, 192,
                    static_cast<unsigned char>(x < 16 ? 255 : 128)});
            else
                out.insert(out.end(), {0, 0, 0, 0});
        }
    }
    for (unsigned row = 0; row < 32; ++row) {
        const unsigned y = 31 - row;
        // A 1-bit AND mask, DWORD aligned, matching the transparent margins.
        const unsigned mask = (y >= 8 && y < 24) ? 0xFF0000FFu : 0xFFFFFFFFu;
        append32(out, mask);
    }
    return out;
}

struct Image {
    UINT width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

void diagnoseShellBitmap(const fs::path& path, int size, const char* label)
{
    ComPtr<IShellItemImageFactory> shell;
    check(SHCreateItemFromParsingName(path.c_str(), nullptr,
          IID_PPV_ARGS(shell.GetAddressOf())), "Diagnostic shell factory");
    HBITMAP raw = nullptr;
    check(shell->GetImage(SIZE{size, size},
          static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_SCALEUP), &raw),
          "Diagnostic GetImage");
    auto deleter = [](HBITMAP value) { DeleteObject(value); };
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(deleter)> owned(raw, deleter);
    DIBSECTION dib{};
    const int objectBytes = GetObjectW(raw, sizeof(dib), &dib);
    std::cout << "SHELL " << label << " request=" << size
              << " GetObject=" << objectBytes
              << " dimensions=" << dib.dsBm.bmWidth << 'x' << dib.dsBm.bmHeight
              << " bitCount=" << dib.dsBm.bmBitsPixel
              << " dibHeight=" << dib.dsBmih.biHeight << '\n';
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(wic.GetAddressOf())), "Diagnostic WIC factory");
    for (auto option : {WICBitmapUseAlpha, WICBitmapUsePremultipliedAlpha}) {
        ComPtr<IWICBitmap> bitmap;
        check(wic->CreateBitmapFromHBITMAP(raw, nullptr, option, bitmap.GetAddressOf()),
              "Diagnostic import HBITMAP");
        UINT width = 0, height = 0;
        check(bitmap->GetSize(&width, &height), "Diagnostic bitmap dimensions");
        ComPtr<IWICFormatConverter> converter;
        check(wic->CreateFormatConverter(converter.GetAddressOf()), "Diagnostic converter");
        check(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppRGBA,
              WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom),
              "Diagnostic straight RGBA conversion");
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        check(converter->CopyPixels(nullptr, width * 4,
              static_cast<UINT>(pixels.size()), pixels.data()), "Diagnostic CopyPixels");
        std::cout << "  import=" << int(option) << " RGBA:";
        for (auto x : {width / 8, width * 3 / 8, width * 5 / 8}) {
            const auto offset = (static_cast<std::size_t>(height / 2) * width + x) * 4;
            std::cout << " (" << int(pixels[offset]) << ',' << int(pixels[offset + 1])
                      << ',' << int(pixels[offset + 2]) << ',' << int(pixels[offset + 3])
                      << ')';
        }
        UINT left = width, top = height, right = 0, bottom = 0;
        bool partialFound = false;
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
                if (pixels[offset + 3]) {
                    left = std::min(left, x); right = std::max(right, x);
                    top = std::min(top, y); bottom = std::max(bottom, y);
                }
                if (!partialFound && pixels[offset + 3] && pixels[offset + 3] != 255) {
                    partialFound = true;
                    std::cout << " firstPartial@" << x << ',' << y << "=("
                              << int(pixels[offset]) << ',' << int(pixels[offset + 1])
                              << ',' << int(pixels[offset + 2]) << ','
                              << int(pixels[offset + 3]) << ')';
                }
            }
        std::cout << " alphaBounds=" << left << ',' << top << '-' << right << ','
                  << bottom << '\n';
    }
}

Image decode(const icons::Result& result)
{
    require(static_cast<bool>(result), result.error.c_str());
    require(result.error.empty(), "Successful extraction also reported an error");
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    require(result.png.size() >= sizeof(signature) &&
                std::equal(std::begin(signature), std::end(signature), result.png.begin()),
            "Output is not a PNG");
    require(result.png.size() <= INT32_MAX, "PNG exceeds SDK signed buffer limit");
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(wic.GetAddressOf())), "Create decoder factory");
    ComPtr<IWICStream> stream;
    check(wic->CreateStream(stream.GetAddressOf()), "Create decoder stream");
    check(stream->InitializeFromMemory(const_cast<BYTE*>(result.png.data()),
                                        static_cast<DWORD>(result.png.size())),
          "Initialize decoder stream");
    ComPtr<IWICBitmapDecoder> decoder;
    check(wic->CreateDecoderFromStream(stream.Get(), nullptr,
          WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()), "Create PNG decoder");
    GUID container{};
    check(decoder->GetContainerFormat(&container), "Get decoded container");
    require(IsEqualGUID(container, GUID_ContainerFormatPng), "Decoder container is not PNG");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, frame.GetAddressOf()), "Get PNG frame");
    Image image;
    check(frame->GetSize(&image.width, &image.height), "Get decoded size");
    require(image.width > 0 && image.height > 0 &&
                static_cast<std::uint64_t>(image.width) * image.height <= INT32_MAX / 4,
            "Invalid decoded dimensions");
    WICPixelFormatGUID nativeFormat{};
    check(frame->GetPixelFormat(&nativeFormat), "Get PNG pixel format");
    require(IsEqualGUID(nativeFormat, GUID_WICPixelFormat32bppBGRA) ||
                IsEqualGUID(nativeFormat, GUID_WICPixelFormat32bppRGBA),
            "PNG did not retain an alpha-bearing 32-bit format");
    ComPtr<IWICFormatConverter> converter;
    check(wic->CreateFormatConverter(converter.GetAddressOf()), "Create PNG converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
          WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom),
          "Initialize straight RGBA decoder");
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    check(converter->CopyPixels(nullptr, image.width * 4,
          static_cast<UINT>(image.rgba.size()), image.rgba.data()), "Decode PNG pixels");
    return image;
}

Image load(const fs::path& path, int size, icons::Kind kind)
{
    Image image = decode(icons::load(path.u16string(), size, kind));
    require(image.width == static_cast<UINT>(size) && image.height == static_cast<UINT>(size),
            "PNG dimensions do not exactly match the requested size");
    bool visible = false;
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        visible |= image.rgba[i] != 0;
    require(visible, "Icon is completely transparent");
    return image;
}

void reject(const std::u16string& path, int size, icons::Kind kind)
{
    const auto result = icons::load(path, size, kind);
    require(!result && result.png.empty() && !result.error.empty(),
            "Invalid input did not return an explicit failure");
}

void checkFixture(const Image& image)
{
    const UINT size = image.width;
    const auto* partial = image.rgba.data() +
        (static_cast<std::size_t>(size / 2) * size + size * 5 / 8) * 4;
    std::cout << "PNG fixture " << size << " RGBA=(" << int(partial[0]) << ','
              << int(partial[1]) << ',' << int(partial[2]) << ',' << int(partial[3])
              << ")\n";
    require(std::abs(int(partial[0]) - 192) <= 6 &&
                std::abs(int(partial[1]) - 96) <= 6 &&
                std::abs(int(partial[2]) - 32) <= 6 &&
                std::abs(int(partial[3]) - 128) <= 8,
            "Custom icon lost straight-alpha edge colors/partial transparency");
    require(image.rgba[3] == 0, "Custom icon lost its transparent corner");
    UINT left = size, right = 0, top = size, bottom = 0;
    for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x)
            if (image.rgba[(static_cast<std::size_t>(y) * size + x) * 4 + 3] > 16) {
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
            }
    require(left >= size / 8 && top >= size / 8 &&
                right < size * 7 / 8 && bottom < size * 7 / 8,
            "Custom icon artwork margins were cropped away");
    require(right >= left && bottom >= top &&
                right - left + 1 >= size / 3 && bottom - top + 1 >= size / 3,
            "Legacy 32px custom icon was not scaled to the requested canvas");
}

void run()
{
    Fixtures fixtures;
    const auto file = fixtures.root / L"\u6587\u4ef6-\u00e9.txt";
    const auto folder = fixtures.root / L"\u30d5\u30a9\u30eb\u30c0";
    write(file, {'t', 'e', 's', 't'});
    require(fs::create_directory(folder), "Create Unicode folder failed");

    require(!icons::validSize(-1) && !icons::validSize(0) &&
                icons::validSize(1) && icons::validSize(23170) &&
                !icons::validSize(23171) && !icons::validSize(INT32_MAX),
            "Size arithmetic boundary validation failed");
    for (int size : {1, 17, 31, 32, 63, 255, 256, 257}) {
        load(file, size, icons::Kind::file);
        load(folder, size, icons::Kind::folder);
    }
    reject((fixtures.root / L"missing.txt").u16string(), 256, icons::Kind::file);
    reject((fixtures.root / L"missing-folder").u16string(), 256, icons::Kind::folder);
    reject(file.u16string(), 256, icons::Kind::folder);
    reject(folder.u16string(), 256, icons::Kind::file);
    reject({}, 256, icons::Kind::file);
    reject(file.u16string() + std::u16string(u"\0suffix", 7), 256, icons::Kind::file);
    reject(file.u16string(), 0, icons::Kind::file);
    reject(file.u16string(), -1, icons::Kind::file);
    reject(file.u16string(), 23171, icons::Kind::file);
    reject(file.u16string(), INT32_MAX, icons::Kind::file);
    reject(file.u16string(), 256, static_cast<icons::Kind>(99));

    // A caller can already own an MTA; extraction must still use its own STA.
    bool mtaPassed = false;
    std::string mtaError;
    std::thread mta([&] {
        try {
            Apartment apartment(COINIT_MULTITHREADED);
            load(file, 256, icons::Kind::file);
            Apartment stillMta(COINIT_MULTITHREADED);
            mtaPassed = true;
        } catch (const std::exception& error) {
            mtaError = error.what();
        }
    });
    mta.join();
    require(mtaPassed, mtaError.c_str());
    // An S_FALSE initialization is balanced independently of the caller's STA.
    {
        Apartment nested(COINIT_APARTMENTTHREADED);
        require(nested.hr == S_FALSE, "Expected already initialized STA");
        load(folder, 256, icons::Kind::folder);
    }
    Apartment stillSta(COINIT_APARTMENTTHREADED);
    require(stillSta.hr == S_FALSE, "Backend uninitialized the caller's COM apartment");

    // Default size is supplied by the wrapper; load receives the explicit 256.
    load(file, 256, icons::Kind::file);
    load(file, 33, icons::Kind::file);
    load(folder, 65, icons::Kind::folder);
    const DWORD handlesBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 32; ++i) {
        load(file, 33, icons::Kind::file);
        load(folder, 65, icons::Kind::folder);
    }
    const DWORD handlesAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    require(handlesBefore == 0 || handlesAfter <= handlesBefore + 4,
            "Repeated extraction leaked GDI objects");

    const auto icon = fixtures.root / L"\u900f\u660e-32.ico";
    const auto custom = fixtures.root / L"\u30ab\u30b9\u30bf\u30e0";
    write(icon, legacyIcon());
    require(fs::create_directory(custom), "Create custom folder failed");
    std::wstring iconPath = icon.wstring();
    SHFOLDERCUSTOMSETTINGS settings{};
    settings.dwSize = sizeof(settings);
    settings.dwMask = FCSM_ICONFILE;
    settings.pszIconFile = iconPath.data();
    settings.cchIconFile = static_cast<DWORD>(iconPath.size() + 1);
    settings.iIconIndex = 0;
    check(SHGetSetFolderCustomSettings(&settings, custom.c_str(), FCS_FORCEWRITE),
          "Set desktop.ini custom folder icon");
    require(SetFileAttributesW(custom.c_str(), FILE_ATTRIBUTE_READONLY) != 0,
            "Set custom folder read-only attribute failed");
    require(fs::exists(custom / L"desktop.ini"), "Shell did not create desktop.ini");
    {
        std::ifstream ini(custom / L"desktop.ini", std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(ini)), {});
        std::cout << "AUTO desktop.ini bytes=" << bytes.size() << " prefix:";
        for (std::size_t i = 0; i < std::min<std::size_t>(bytes.size(), 80); ++i)
            std::cout << ' ' << std::hex << unsigned(bytes[i]);
        std::cout << std::dec << " folderAttrs=" << GetFileAttributesW(custom.c_str())
                  << " iniAttrs=" << GetFileAttributesW((custom / L"desktop.ini").c_str())
                  << '\n';
        wchar_t resource[1024]{};
        const DWORD count = GetPrivateProfileStringW(L".ShellClassInfo", L"IconResource", L"",
            resource, 1024, (custom / L"desktop.ini").c_str());
        std::cout << "AUTO IconResource chars=" << count
                  << " expected=" << iconPath.size() + 2 << " units:";
        for (DWORD i = 0; i < count; ++i)
            std::cout << ' ' << std::hex << unsigned(resource[i]);
        std::cout << std::dec << '\n';
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, custom.c_str(), nullptr);
    diagnoseShellBitmap(icon, 32, "ICO");
    diagnoseShellBitmap(custom, 32, "folder");
    diagnoseShellBitmap(custom, 256, "folder");
    diagnoseShellBitmap(custom, 257, "folder");
    checkFixture(load(icon, 32, icons::Kind::file));

    const auto explicitCustom = fixtures.root / L"\u30ab\u30b9\u30bf\u30e0-Unicode-INI";
    require(fs::create_directory(explicitCustom), "Create explicit custom folder failed");
    write(explicitCustom / L"fixture.ico", legacyIcon());
    const std::u16string contents =
        u"\uFEFF[.ShellClassInfo]\r\nIconResource=fixture.ico,0\r\n"
        u"IconFile=fixture.ico\r\nIconIndex=0\r\n";
    std::vector<unsigned char> iniBytes;
    for (char16_t ch : contents) append16(iniBytes, ch);
    write(explicitCustom / L"desktop.ini", iniBytes);
    require(SetFileAttributesW((explicitCustom / L"desktop.ini").c_str(),
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) != 0,
            "Set explicit desktop.ini attributes failed");
    require(SetFileAttributesW(explicitCustom.c_str(),
            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM) != 0,
            "Set explicit custom folder attributes failed");
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, explicitCustom.c_str(), nullptr);
    diagnoseShellBitmap(explicitCustom, 32, "explicit folder");
    diagnoseShellBitmap(explicitCustom, 16, "explicit folder");
    diagnoseShellBitmap(explicitCustom, 31, "explicit folder");
    diagnoseShellBitmap(explicitCustom, 48, "explicit folder");
    diagnoseShellBitmap(explicitCustom, 64, "explicit folder");
    diagnoseShellBitmap(explicitCustom, 128, "explicit folder");
    diagnoseShellBitmap(explicitCustom, 256, "explicit folder");
    diagnoseShellBitmap(explicitCustom, 257, "explicit folder");
    diagnoseShellBitmap(icon, 256, "ICO");
    checkFixture(load(explicitCustom, 32, icons::Kind::folder));
    checkFixture(load(explicitCustom, 256, icons::Kind::folder));
    checkFixture(load(explicitCustom, 257, icons::Kind::folder));
}
} // namespace

int main()
{
    try {
        Apartment apartment(COINIT_APARTMENTTHREADED);
        run();
        std::cout << "PASS: Windows native icon extraction and PNG alpha tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
