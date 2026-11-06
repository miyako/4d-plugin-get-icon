// Native Windows integration tests. Fixtures live under the working directory,
// never in a system temporary directory; no installed file associations change.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../icon.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commoncontrols.h>
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
using icons::OverlayPolicy;
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

Image load(const fs::path& path, int size, icons::Kind kind,
           OverlayPolicy overlays = OverlayPolicy::system)
{
    Image image = decode(icons::load(path.u16string(), size, kind, overlays));
    require(image.width == static_cast<UINT>(size) && image.height == static_cast<UINT>(size),
            "PNG dimensions do not exactly match the requested size");
    bool visible = false;
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        visible |= image.rgba[i] != 0;
    require(visible, "Icon is completely transparent");
    return image;
}

void checkShellOverlay(const fs::path& path, icons::Kind kind)
{
    SHFILEINFOW info{};
    require(SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                SHGFI_ICON | SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX) != 0,
            "Cannot query reference Shell overlay");
    if (info.hIcon) DestroyIcon(info.hIcon);
    const unsigned selected = static_cast<unsigned>(info.iIcon) >> 24;
    const auto normal = load(path, 128, kind);
    const auto noShortcut = load(path, 128, kind, OverlayPolicy::noShortcut);
    const auto none = load(path, 128, kind, OverlayPolicy::none);
    require((normal.rgba != none.rgba) == (selected != 0),
            "Default policy did not match the Shell-selected overlay");
    const int shortcut = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
    require(shortcut >= 0, "Cannot query reference shortcut overlay");
    require(noShortcut.rgba == (selected == static_cast<unsigned>(shortcut)
                ? none.rgba : normal.rgba),
            "Shortcut suppression removed an unrelated badge or retained the arrow");
}

Image jumboOverlay(const fs::path& path, UINT size)
{
    SHFILEINFOW info{};
    require(SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                SHGFI_ICON | SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX) != 0,
            "Cannot query large badge reference");
    if (info.hIcon) DestroyIcon(info.hIcon);
    const unsigned selected = static_cast<unsigned>(info.iIcon) >> 24;
    require(selected != 0, "Shortcut fixture has no Shell overlay");
    ComPtr<IImageList> images;
    check(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(images.GetAddressOf())),
          "Get jumbo reference image list");
    int index = 0;
    check(images->GetOverlayImage(selected, &index), "Get jumbo reference overlay index");
    HICON icon = nullptr;
    check(images->GetIcon(index, ILD_TRANSPARENT, &icon), "Get jumbo reference overlay");
    require(icon != nullptr, "Null jumbo reference overlay");
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(wic.GetAddressOf())), "Create badge reference factory");
    ComPtr<IWICBitmap> bitmap;
    const HRESULT hr = wic->CreateBitmapFromHICON(icon, bitmap.GetAddressOf());
    DestroyIcon(icon);
    check(hr, "Decode jumbo reference overlay");
    Image overlay;
    check(bitmap->GetSize(&overlay.width, &overlay.height), "Get reference overlay dimensions");
    require(overlay.width == overlay.height && overlay.width > 0,
            "Expected square Shell overlay canvas");
    ComPtr<IWICBitmapSource> source = bitmap;
    ComPtr<IWICFormatConverter> premultiplied;
    ComPtr<IWICBitmapScaler> scaler;
    if (overlay.width > size) {
        check(wic->CreateFormatConverter(premultiplied.GetAddressOf()),
              "Create reference premultiplied converter");
        check(premultiplied->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
              WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom),
              "Convert reference overlay to premultiplied alpha");
        check(wic->CreateBitmapScaler(scaler.GetAddressOf()), "Create reference badge scaler");
        check(scaler->Initialize(premultiplied.Get(), size, size, WICBitmapInterpolationModeFant),
              "Reduce reference badge");
        source = scaler;
        overlay.width = overlay.height = size;
    }
    ComPtr<IWICFormatConverter> converter;
    check(wic->CreateFormatConverter(converter.GetAddressOf()), "Create reference badge converter");
    check(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA,
          WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom),
          "Convert reference badge to straight RGBA");
    overlay.rgba.resize(static_cast<std::size_t>(overlay.width) * overlay.height * 4);
    check(converter->CopyPixels(nullptr, overlay.width * 4,
          static_cast<UINT>(overlay.rgba.size()), overlay.rgba.data()), "Read reference badge pixels");
    return overlay;
}

void checkNativeBadge(const Image& plain, const Image& decorated, const Image& overlay)
{
    require(overlay.width <= plain.width && overlay.height <= plain.height,
            "Reference badge exceeds output canvas");
    unsigned changed = 0;
    for (UINT y = 0; y < plain.height; ++y)
        for (UINT x = 0; x < plain.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * plain.width + x) * 4;
            const auto* base = plain.rgba.data() + offset;
            const auto* actual = decorated.rgba.data() + offset;
            changed += !std::equal(base, base + 4, actual);
            if (x >= overlay.width || y < plain.height - overlay.height) {
                require(std::equal(base, base + 4, actual),
                        "Large badge grew beyond its native bottom-left canvas");
                continue;
            }
            const auto* badge = overlay.rgba.data() +
                (static_cast<std::size_t>(y - (plain.height - overlay.height)) *
                 overlay.width + x) * 4;
            const double alpha = badge[3] / 255.0;
            const double baseAlpha = base[3] / 255.0;
            const double combined = alpha + baseAlpha * (1 - alpha);
            for (unsigned channel = 0; channel < 4; ++channel) {
                const int expected = channel == 3 ? static_cast<int>(std::lround(combined * 255)) :
                    combined == 0 ? base[channel] : static_cast<int>(std::lround(
                        (badge[channel] * alpha + base[channel] * baseAlpha * (1 - alpha)) /
                        combined));
                require(std::abs(int(actual[channel]) - expected) <= 1,
                        "Badge pixels do not match the native high-resolution Shell overlay");
            }
        }
    require(changed > 0, "Native badge comparison did not exercise an overlay");
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

void checkEnlarged(const Image& source, const Image& enlarged)
{
    const UINT factor = enlarged.width / source.width;
    require(factor > 1 && source.width * factor == enlarged.width,
            "Invalid pixel replication test dimensions");
    for (UINT y = 0; y < enlarged.height; ++y)
        for (UINT x = 0; x < enlarged.width; ++x) {
            const auto* expected = source.rgba.data() +
                (static_cast<std::size_t>(y / factor) * source.width + x / factor) * 4;
            const auto* actual = enlarged.rgba.data() +
                (static_cast<std::size_t>(y) * enlarged.width + x) * 4;
            require(std::equal(expected, expected + 4, actual),
                    "Enlargement did not preserve each source pixel as an exact square");
        }
}

void shortcut(const fs::path& path, const fs::path& target, const fs::path& icon,
              int index = 0)
{
    ComPtr<IShellLinkW> link;
    check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(link.GetAddressOf())), "Create Shell shortcut");
    check(link->SetPath(target.c_str()), "Set shortcut target");
    if (!icon.empty())
        check(link->SetIconLocation(icon.c_str(), index), "Set shortcut icon");
    ComPtr<IPersistFile> persisted;
    check(link.As(&persisted), "Query shortcut persistence");
    check(persisted->Save(path.c_str(), TRUE), "Save shortcut");
}

void executableIcon(const fs::path& path, const char* level = nullptr)
{
    std::vector<wchar_t> module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    require(length > 0 && length < module.size(), "Cannot locate fixture executable");
    require(fs::copy_file(module.data(), path), "Cannot copy fixture executable");
    struct Resources {
        HANDLE handle;
        ~Resources() { if (handle) EndUpdateResourceW(handle, TRUE); }
    } resources{BeginUpdateResourceW(path.c_str(), TRUE)};
    require(resources.handle != nullptr, "Cannot edit fixture executable resources");
    auto ico = iconFixture();
    const DWORD bytes = static_cast<DWORD>(ico.size() - 22);
    require(UpdateResourceW(resources.handle, RT_ICON, MAKEINTRESOURCEW(1),
                            0, ico.data() + 22, bytes) != 0, "Cannot embed icon pixels");
    std::vector<unsigned char> group;
    append16(group, 0); append16(group, 1); append16(group, 1);
    group.insert(group.end(), {32, 32, 0, 0});
    append16(group, 1); append16(group, 32);
    append32(group, bytes); append16(group, 1);
    require(UpdateResourceW(resources.handle, RT_GROUP_ICON, MAKEINTRESOURCEW(1),
                            0, group.data(), static_cast<DWORD>(group.size())) != 0,
            "Cannot embed icon directory");
    if (level) {
        const std::string manifest =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
            "<trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\"><security>"
            "<requestedPrivileges><requestedExecutionLevel level=\"" + std::string(level) +
            "\" uiAccess=\"false\"/></requestedPrivileges></security></trustInfo></assembly>";
        require(UpdateResourceW(resources.handle, RT_MANIFEST,
                                MAKEINTRESOURCEW(CREATEPROCESS_MANIFEST_RESOURCE_ID),
                                MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                const_cast<char*>(manifest.data()),
                                static_cast<DWORD>(manifest.size())) != 0,
                "Cannot embed execution level manifest");
    }
    const BOOL saved = EndUpdateResourceW(resources.handle, FALSE);
    resources.handle = nullptr;
    require(saved != 0, "Cannot save fixture executable resources");
}

bool uacEnabled()
{
    DWORD value = 1, bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                     L"EnableLUA", RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS)
        return true;
    return value != 0;
}

// Compare a decorated rendering with its undecorated base: the UAC shield
// may only change the lower-right quadrant, and must be present with UAC on.
void checkShield(const Image& plain, const Image& shielded, bool expected)
{
    require(plain.width == shielded.width && plain.height == shielded.height,
            "Shield comparison dimensions differ");
    unsigned changed = 0;
    for (UINT y = 0; y < plain.height; ++y)
        for (UINT x = 0; x < plain.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * plain.width + x) * 4;
            if (std::equal(plain.rgba.data() + offset, plain.rgba.data() + offset + 4,
                           shielded.rgba.data() + offset))
                continue;
            ++changed;
            require(x >= plain.width / 2 && y >= plain.height / 2,
                    "UAC shield changed pixels outside the lower-right corner");
        }
    require((changed > 0) == expected,
            expected ? "UAC shield is missing" : "Unexpected UAC shield");
}

void runAsShortcut(const fs::path& path, const fs::path& target)
{
    shortcut(path, target, {});
    ComPtr<IShellLinkW> link;
    check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(link.GetAddressOf())), "Create Shell shortcut");
    ComPtr<IPersistFile> persisted;
    check(link.As(&persisted), "Query shortcut persistence");
    check(persisted->Load(path.c_str(), STGM_READWRITE), "Load shortcut");
    ComPtr<IShellLinkDataList> data;
    check(link.As(&data), "Query shortcut flags");
    DWORD flags = 0;
    check(data->GetFlags(&flags), "Get shortcut flags");
    check(data->SetFlags(flags | SLDF_RUNAS_USER), "Set run as administrator");
    check(persisted->Save(path.c_str(), TRUE), "Save shortcut");
}

void checkElevation(const fs::path& root, const fs::path& executable)
{
    const bool uac = uacEnabled();
    const auto elevated = root / L"\u7ba1\u7406\u8005-setup.exe";
    executableIcon(elevated, "requireAdministrator");
    const auto invoker = root / L"invoker.exe";
    executableIcon(invoker, "asInvoker");
    const auto highest = root / L"highest.exe";
    executableIcon(highest, "highestAvailable");
    for (int size : {17, 32, 48, 128, 256, 512}) {
        const auto plain = load(executable, size, icons::Kind::file);
        const auto none = load(elevated, size, icons::Kind::file, OverlayPolicy::none);
        require(none.rgba == plain.rgba, "No-overlays policy did not remove the UAC shield");
        const auto shielded = load(elevated, size, icons::Kind::file);
        checkShield(plain, shielded, uac);
        require(load(elevated, size, icons::Kind::file, OverlayPolicy::noShortcut).rgba ==
                    shielded.rgba,
                "Suppressing shortcut arrows removed the UAC shield");
        require(load(invoker, size, icons::Kind::file).rgba == plain.rgba,
                "asInvoker executable received a UAC shield");
        const auto maybe = load(highest, size, icons::Kind::file);
        require(maybe.rgba == plain.rgba || maybe.rgba == shielded.rgba,
                "highestAvailable executable received an unexpected decoration");
    }

    const auto elevatedLink = root / L"elevated-target.lnk";
    shortcut(elevatedLink, elevated, {});
    const auto runAsLink = root / L"run-as.lnk";
    runAsShortcut(runAsLink, executable);
    for (const auto& link : {elevatedLink, runAsLink})
        for (int size : {32, 256}) {
            const auto none = load(link, size, icons::Kind::file, OverlayPolicy::none);
            const auto shielded = load(link, size, icons::Kind::file, OverlayPolicy::noShortcut);
            checkShield(none, shielded, uac);
            require(load(link, size, icons::Kind::file).rgba != shielded.rgba,
                    "Elevated shortcut is missing its arrow");
        }

    // The compatibility "Run as administrator" setting is keyed by full path.
    const auto layered = root / L"layered.exe";
    executableIcon(layered);
    constexpr wchar_t layers[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    const std::wstring name = fs::absolute(layered).wstring();
    constexpr wchar_t value[] = L"~ HIGHDPIAWARE RunAsAdmin";
    require(RegSetKeyValueW(HKEY_CURRENT_USER, layers, name.c_str(), REG_SZ, value,
                            sizeof(value)) == ERROR_SUCCESS,
            "Cannot set compatibility layer");
    struct Layer {
        std::wstring name;
        ~Layer() { RegDeleteKeyValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
            name.c_str()); }
    } cleanup{name};
    checkShield(load(executable, 256, icons::Kind::file),
                load(layered, 256, icons::Kind::file), uac);
}

void run()
{
    Fixtures fixtures;
    const auto file = fixtures.root / L"\u6587\u4ef6-\u00e9.txt";
    const auto folder = fixtures.root / L"\u30d5\u30a9\u30eb\u30c0";
    write(file, {'t', 'e', 's', 't'});
    require(fs::create_directory(folder), "Create Unicode folder failed");
    checkShellOverlay(file, icons::Kind::file);
    checkShellOverlay(folder, icons::Kind::folder);
    const auto invalidPolicy = icons::load(file.u16string(), 32, icons::Kind::file,
                                          static_cast<OverlayPolicy>(99));
    require(!invalidPolicy && invalidPolicy.png.empty() && !invalidPolicy.error.empty(),
            "Invalid overlay policy did not fail explicitly");

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
    const auto executable = fixtures.root / L"legacy-32.exe";
    executableIcon(executable);
    const auto custom = customFolder(fixtures.root / L"\u30ab\u30b9\u30bf\u30e0", icon);
    for (int size : {32, 48, 64, 128, 256, 257}) {
        std::cout << "Legacy ICO file size=" << size << '\n';
        checkFixture(load(icon, size, icons::Kind::file));
        std::cout << "Legacy ICO folder size=" << size << '\n';
        checkFixture(load(custom, size, icons::Kind::folder));
    }
    for (const auto& target : {icon, custom, executable}) {
        const auto kind = target == custom ? icons::Kind::folder : icons::Kind::file;
        const auto source = load(target, 32, kind);
        for (int size : {128, 256}) {
            const auto enlarged = load(target, size, kind);
            checkFixture(enlarged);
            checkEnlarged(source, enlarged);
        }
        require(load(target, 256, kind).rgba ==
                    load(target, 256, kind, OverlayPolicy::noShortcut).rgba,
                "Suppressing shortcut arrows changed a non-shortcut");
        checkShellOverlay(target, kind);
    }

    const auto largeIcon = fixtures.root / L"native-256.ico";
    write(largeIcon, iconFixture(256, 64));
    for (const auto& target : {file, folder, executable, largeIcon}) {
        const auto link = fixtures.root / (target.filename().wstring() + L".lnk");
        const auto& resource = target == executable ? executable :
            target == largeIcon ? largeIcon : icon;
        shortcut(link, target, resource, target == executable ? -1 : 0);
        checkShellOverlay(link, icons::Kind::file);
        for (int size : {1, 17, 32, 48, 64, 128, 256, 257, 512, 1024}) {
            const auto plain = load(link, size, icons::Kind::file, OverlayPolicy::noShortcut);
            const auto decorated = load(link, size, icons::Kind::file);
            require(load(link, size, icons::Kind::file, OverlayPolicy::none).rgba == plain.rgba,
                    "No-overlays policy did not remove the shortcut arrow");
            require(plain.rgba == load(resource, size, icons::Kind::file).rgba,
                    "Suppressed shortcut did not preserve its custom icon");
            unsigned changed = 0;
            for (UINT y = 0; y < decorated.height; ++y)
                for (UINT x = 0; x < decorated.width; ++x) {
                    const auto offset = (static_cast<std::size_t>(y) * size + x) * 4;
                    if (!std::equal(plain.rgba.data() + offset,
                                    plain.rgba.data() + offset + 4,
                                    decorated.rgba.data() + offset)) {
                        ++changed;
                        require(x < (static_cast<UINT>(size) + 1) / 2 &&
                                    y >= static_cast<UINT>(size) / 2,
                                "Shortcut badge changed pixels outside the lower-left corner");
                    }
                }
            if (size >= 17)
                require(changed > 0, "Shortcut arrow is missing by default");
            if (size >= 128)
                checkNativeBadge(plain, decorated, jumboOverlay(link, size));
        }
        if (target != largeIcon)
            checkEnlarged(load(link, 32, icons::Kind::file, OverlayPolicy::noShortcut),
                          load(link, 256, icons::Kind::file, OverlayPolicy::noShortcut));
    }
    const auto defaultLink = fixtures.root / L"default-executable.lnk";
    shortcut(defaultLink, executable, {});
    checkEnlarged(load(executable, 32, icons::Kind::file),
                  load(defaultLink, 256, icons::Kind::file, OverlayPolicy::noShortcut));
    require(load(defaultLink, 256, icons::Kind::file).rgba !=
                load(defaultLink, 256, icons::Kind::file, OverlayPolicy::noShortcut).rgba,
            "Default executable shortcut is missing its arrow");

    // MSI advertised shortcuts often reference ICO data named *.exe. The Shell
    // pads such icons onto the jumbo canvas; enlarge them like any small icon.
    const auto msiIcon = fixtures.root / L"msi-product-icon.exe";
    write(msiIcon, iconFixture());
    const auto msiLink = fixtures.root / L"advertised.lnk";
    shortcut(msiLink, file, msiIcon);
    const auto msiSource = load(msiLink, 32, icons::Kind::file, OverlayPolicy::noShortcut);
    checkFixture(msiSource);
    for (int size : {128, 256, 512}) {
        std::cout << "MSI style ICO shortcut size=" << size << '\n';
        const auto enlarged = load(msiLink, size, icons::Kind::file, OverlayPolicy::noShortcut);
        checkFixture(enlarged);
        checkEnlarged(msiSource, enlarged);
    }

    checkElevation(fixtures.root, executable);

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
