// Native Windows integration tests. Fixtures live under the working directory,
// never in a system temporary directory; no installed file associations change.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../icon.h"

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

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

std::vector<unsigned char> iconFixture(unsigned size = 32, unsigned margin = 8)
{
    std::vector<unsigned char> out;
    append16(out, 0); append16(out, 1); append16(out, 1);
    const auto dimension = static_cast<unsigned char>(size);
    out.insert(out.end(), {dimension, dimension, 0, 0});
    append16(out, 1); append16(out, 32);
    const unsigned maskStride = ((size + 31) / 32) * 4;
    append32(out, 40 + size * size * 4 + size * maskStride);
    append32(out, 22);
    append32(out, 40); append32(out, size); append32(out, size * 2);
    append16(out, 1); append16(out, 32);
    append32(out, BI_RGB); append32(out, size * size * 4);
    append32(out, 0); append32(out, 0); append32(out, 0); append32(out, 0);
    // ICO DIB pixels have straight BGRA. Half the colored square is translucent
    // with R>A, distinguishing straight pixels from premultiplied pixels.
    for (unsigned row = 0; row < size; ++row) {
        const unsigned y = size - 1 - row;
        for (unsigned x = 0; x < size; ++x) {
            if (x >= margin && x < size - margin && y >= margin && y < size - margin)
                out.insert(out.end(), {32, 96, 192,
                    static_cast<unsigned char>(x < size / 2 ? 255 : 128)});
            else
                out.insert(out.end(), {0, 0, 0, 0});
        }
    }
    for (unsigned row = 0; row < size; ++row) {
        const unsigned y = size - 1 - row;
        // A 1-bit AND mask, DWORD aligned, matching the transparent margins.
        for (unsigned byte = 0; byte < maskStride; ++byte) {
            unsigned mask = 0;
            for (unsigned bit = 0; bit < 8; ++bit) {
                const unsigned x = byte * 8 + bit;
                if (x < margin || x >= size - margin || y < margin || y >= size - margin)
                    mask |= 0x80u >> bit;
            }
            out.push_back(static_cast<unsigned char>(mask));
        }
    }
    return out;
}

struct Image {
    UINT width = 0, height = 0;
    std::vector<unsigned char> rgba;
};

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
                static_cast<std::uint64_t>(image.width) * image.height <= UINT32_MAX / 4,
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

void checkPartial(const Image& image, UINT x, UINT y)
{
    const auto* partial = image.rgba.data() +
        (static_cast<std::size_t>(y) * image.width + x) * 4;
    const std::string message = "Custom icon lost straight-alpha edge colors/partial transparency: size=" +
        std::to_string(image.width) + " RGBA=(" + std::to_string(partial[0]) + "," +
        std::to_string(partial[1]) + "," + std::to_string(partial[2]) + "," +
        std::to_string(partial[3]) + ")";
    require(std::abs(int(partial[0]) - 192) <= 6 &&
                std::abs(int(partial[1]) - 96) <= 6 &&
                std::abs(int(partial[2]) - 32) <= 6 &&
                std::abs(int(partial[3]) - 128) <= 8,
            message.c_str());
}

void checkFixture(const Image& image)
{
    const UINT size = image.width;
    checkPartial(image, size * 5 / 8, size / 2);
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

void checkPaddedFixture(const Image& image)
{
    const UINT size = image.width;
    checkPartial(image, size / 2 + size / 64, size / 2);
    require(image.rgba[3] == 0, "Padded icon lost its transparent corner");
    unsigned visible = 0;
    for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x)
            if (image.rgba[(static_cast<std::size_t>(y) * size + x) * 4 + 3] > 16) {
                ++visible;
                require(x >= size * 7 / 16 && x < size * 9 / 16 &&
                            y >= size * 7 / 16 && y < size * 9 / 16,
                        "Intentional wide artwork margins were cropped or rescaled");
            }
    require(visible >= 144 && visible <= 400,
            "A native 256px resource canvas was mistaken for a legacy 32px canvas");
}

fs::path customFolder(const fs::path& path, const fs::path& icon)
{
    require(fs::create_directory(path), "Create custom folder failed");
    // SHGetSetFolderCustomSettings creates ANSI desktop.ini on this Windows
    // runner, losing Japanese icon paths. Supply an explicit UTF-16LE BOM.
    const std::u16string contents = u"\uFEFF[.ShellClassInfo]\r\nIconResource=" +
        icon.u16string() + u",0\r\nIconFile=" + icon.u16string() + u"\r\nIconIndex=0\r\n";
    std::vector<unsigned char> bytes;
    for (char16_t ch : contents) append16(bytes, ch);
    const auto ini = path / L"desktop.ini";
    write(ini, bytes);
    const std::wstring expected = icon.wstring() + L",0";
    std::vector<wchar_t> decoded(expected.size() + 1, L'\0');
    const DWORD count = GetPrivateProfileStringW(L".ShellClassInfo", L"IconResource", L"",
        decoded.data(), static_cast<DWORD>(decoded.size()), ini.c_str());
    require(count == expected.size() && expected == decoded.data(),
            "desktop.ini did not preserve the Unicode icon resource path");
    require(SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) != 0,
            "Set desktop.ini attributes failed");
    require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM) != 0,
            "Set custom folder attributes failed");
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, path.c_str(), nullptr);
    return path;
}

void run()
{
    Fixtures fixtures;
    const auto file = fixtures.root / L"\u6587\u4ef6-\u00e9.txt";
    const auto folder = fixtures.root / L"\u30d5\u30a9\u30eb\u30c0";
    write(file, {'t', 'e', 's', 't'});
    require(fs::create_directory(folder), "Create Unicode folder failed");

    // Check representability only; do not allocate near-limit pixel buffers.
    require(!icons::validSize(-1) && !icons::validSize(0) &&
                icons::validSize(1) && icons::validSize(23171) &&
                icons::validSize(32767) && !icons::validSize(32768) &&
                !icons::validSize(INT32_MAX),
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
    reject(file.u16string(), 32768, icons::Kind::file);
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
    write(icon, iconFixture());
    const auto custom = customFolder(fixtures.root / L"\u30ab\u30b9\u30bf\u30e0", icon);
    for (int size : {32, 48, 64, 128, 256, 257}) {
        std::cout << "Legacy ICO file size=" << size << '\n';
        checkFixture(load(icon, size, icons::Kind::file));
        std::cout << "Legacy ICO folder size=" << size << '\n';
        checkFixture(load(custom, size, icons::Kind::folder));
    }

    // A real 256px canvas intentionally containing only 16px of artwork must
    // stay padded. Resource dimensions, never alpha bounds, control scaling.
    const auto paddedIcon = fixtures.root / L"\u4f59\u767d-256.ico";
    write(paddedIcon, iconFixture(256, 120));
    const auto paddedFolder = customFolder(fixtures.root / L"\u4f59\u767d", paddedIcon);
    for (int size : {256, 257}) {
        std::cout << "Intentionally padded ICO file size=" << size << '\n';
        checkPaddedFixture(load(paddedIcon, size, icons::Kind::file));
        std::cout << "Intentionally padded ICO folder size=" << size << '\n';
        checkPaddedFixture(load(paddedFolder, size, icons::Kind::folder));
    }
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
