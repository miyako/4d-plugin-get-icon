#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "icon.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commoncontrols.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace icons {
namespace {
using Microsoft::WRL::ComPtr;

Result failure(const char* operation, HRESULT hr)
{
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    return {{}, std::string(operation) + " failed (" + code + ")"};
}

struct Apartment {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                               COINIT_DISABLE_OLE1DDE);
    ~Apartment()
    {
        if (status == S_OK || status == S_FALSE)
            CoUninitialize();
    }
};

struct DeleteBitmap {
    void operator()(HBITMAP bitmap) const { DeleteObject(bitmap); }
};
using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, DeleteBitmap>;

struct DestroyIcon {
    void operator()(HICON icon) const { ::DestroyIcon(icon); }
};
using Icon = std::unique_ptr<std::remove_pointer_t<HICON>, DestroyIcon>;

HRESULT systemImageList(UINT size, ComPtr<IImageList>& images)
{
    UINT selectedSize = 0;
    for (int kind : {SHIL_SMALL, SHIL_LARGE, SHIL_EXTRALARGE, SHIL_JUMBO}) {
        ComPtr<IImageList> candidate;
        HRESULT hr = SHGetImageList(kind, IID_PPV_ARGS(candidate.GetAddressOf()));
        if (FAILED(hr)) return hr;
        int width = 0, height = 0;
        hr = candidate->GetIconSize(&width, &height);
        if (FAILED(hr)) return hr;
        if (width <= 0 || height <= 0) return WINCODEC_ERR_BADIMAGE;
        const UINT extent = static_cast<UINT>(std::max(width, height));
        // Prefer the smallest native canvas that covers the request, or the
        // largest available one. Query dimensions because list sizes vary by DPI.
        if (!images || (selectedSize < size && extent > selectedSize) ||
            (extent >= size && extent < selectedSize)) {
            images = candidate;
            selectedSize = extent;
        }
    }
    return S_OK;
}

HRESULT shellOverlay(const std::wstring& path, UINT size, OverlayPolicy policy, Icon& overlay)
{
    if (policy == OverlayPolicy::none) return S_OK;
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX))
        return E_FAIL;
    Icon decorated(info.hIcon);
    const unsigned index = static_cast<unsigned>(info.iIcon) >> 24;
    // Respect the user's Shell configuration, including disabled link badges.
    if (!index) return S_OK;
    if (policy == OverlayPolicy::noShortcut) {
        const int shortcut = SHGetIconOverlayIndexW(nullptr, IDO_SHGIOI_LINK);
        if (shortcut < 0) return E_FAIL;
        // A link may have a different Shell-selected status badge. Suppress
        // the shortcut overlay itself, not every overlay on a shortcut file.
        if (index == static_cast<unsigned>(shortcut)) return S_OK;
    }
    ComPtr<IImageList> images;
    HRESULT hr = systemImageList(size, images);
    if (FAILED(hr)) return hr;
    int image = 0;
    hr = images->GetOverlayImage(index, &image);
    if (FAILED(hr)) return hr;
    HICON raw = nullptr;
    hr = images->GetIcon(image, ILD_TRANSPARENT, &raw);
    overlay.reset(raw);
    return SUCCEEDED(hr) && !overlay ? E_FAIL : hr;
}

bool bufferSize(UINT width, UINT height, UINT& stride, UINT& bytes)
{
    // WIC raw buffer lengths are UINT. Only the encoded PNG crosses the SDK's
    // signed 32-bit length boundary; do not apply that limit to raw pixels.
    const auto count = static_cast<std::uint64_t>(width) * height;
    if (!width || !height || count > std::numeric_limits<UINT>::max() / 4)
        return false;
    stride = width * 4;
    bytes = static_cast<UINT>(count * 4);
    return true;
}

bool hasExtension(const std::wstring& path, const wchar_t* extension)
{
    const std::size_t length = std::wcslen(extension);
    return path.size() >= length &&
        CompareStringOrdinal(path.data() + path.size() - length, static_cast<int>(length),
                             extension, static_cast<int>(length), TRUE) == CSTR_EQUAL;
}

bool isIconFile(const std::wstring& path)
{
    return hasExtension(path, L".ico");
}

bool isBinaryIconFile(const std::wstring& path)
{
    return hasExtension(path, L".exe") || hasExtension(path, L".dll") ||
           hasExtension(path, L".icl") || hasExtension(path, L".cpl") ||
           hasExtension(path, L".ocx") || hasExtension(path, L".scr");
}

HRESULT icoCanvas(IWICImagingFactory* wic, const std::wstring& path, UINT& largest)
{
    largest = 0;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                               WICDecodeMetadataCacheOnDemand,
                                               decoder.GetAddressOf());
    if (FAILED(hr)) return hr;
    UINT count = 0;
    hr = decoder->GetFrameCount(&count);
    if (FAILED(hr)) return hr;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(i, frame.GetAddressOf());
        if (FAILED(hr)) return hr;
        UINT width = 0, height = 0;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr)) return hr;
        largest = std::max(largest, std::max(width, height));
    }
    return largest ? S_OK : WINCODEC_ERR_BADIMAGE;
}

HRESULT resourceCanvas(HMODULE module, LPCWSTR name, UINT& largest)
{
    HRSRC resource = FindResourceW(module, name, RT_GROUP_ICON);
    if (!resource) return HRESULT_FROM_WIN32(GetLastError());
    const DWORD bytes = SizeofResource(module, resource);
    HGLOBAL loaded = LoadResource(module, resource);
    const auto* data = static_cast<const unsigned char*>(LockResource(loaded));
    if (!data || bytes < 6 || data[0] || data[1] || data[2] != 1 || data[3])
        return WINCODEC_ERR_BADIMAGE;
    const unsigned count = data[4] | (unsigned(data[5]) << 8);
    if (!count || count > (bytes - 6) / 14) return WINCODEC_ERR_BADIMAGE;
    for (unsigned i = 0; i < count; ++i) {
        const auto* entry = data + 6 + i * 14;
        const UINT width = entry[0] ? entry[0] : 256;
        const UINT height = entry[1] ? entry[1] : 256;
        largest = std::max(largest, std::max(width, height));
    }
    return S_OK;
}

struct ResourceCanvas {
    int remaining;
    UINT largest = 0;
    HRESULT status = S_FALSE;
    bool found = false;
};

BOOL CALLBACK findIconResource(HMODULE module, LPCWSTR, LPWSTR name, LONG_PTR parameter)
{
    auto& canvas = *reinterpret_cast<ResourceCanvas*>(parameter);
    if (canvas.remaining-- > 0) return TRUE;
    canvas.found = true;
    canvas.status = resourceCanvas(module, name, canvas.largest);
    return FALSE;
}

HRESULT binaryCanvas(IWICImagingFactory* wic, const std::wstring& path, int index, UINT& largest)
{
    struct Module {
        HMODULE handle;
        ~Module() { if (handle) FreeLibrary(handle); }
    } module{LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
    if (!module.handle) {
        const DWORD error = GetLastError();
        if (error != ERROR_BAD_EXE_FORMAT) return HRESULT_FROM_WIN32(error);
        // Windows Installer advertised shortcuts reference icon files under
        // %WINDIR%\Installer that hold ICO data despite an .exe name.
        if (index) return S_FALSE;
        const HRESULT hr = icoCanvas(wic, path, largest);
        largest = SUCCEEDED(hr) ? largest : 0;
        return SUCCEEDED(hr) ? hr : S_FALSE;
    }
    if (index < 0) {
        const auto id = -static_cast<std::int64_t>(index);
        if (id > UINT16_MAX) return HRESULT_FROM_WIN32(ERROR_INVALID_INDEX);
        return resourceCanvas(module.handle, MAKEINTRESOURCEW(static_cast<WORD>(id)), largest);
    }
    ResourceCanvas canvas{index};
    const BOOL enumerated = EnumResourceNamesW(module.handle, RT_GROUP_ICON, findIconResource,
                                               reinterpret_cast<LONG_PTR>(&canvas));
    if (canvas.found) {
        largest = canvas.largest;
        return canvas.status;
    }
    const DWORD error = GetLastError();
    if (!enumerated && error != ERROR_RESOURCE_TYPE_NOT_FOUND &&
        error != ERROR_RESOURCE_DATA_NOT_FOUND)
        return HRESULT_FROM_WIN32(error);
    // Executables without icons use the Shell's default file icon.
    return S_FALSE;
}

HRESULT iconFileCanvas(IShellItem* item, IWICImagingFactory* wic,
                       const std::wstring& nativePath, bool directory, UINT& largest)
{
    largest = 0;
    if (!directory && isIconFile(nativePath))
        return icoCanvas(wic, nativePath, largest);
    ComPtr<IExtractIconW> location;
    constexpr IID extractIconIID = {0x000214fa, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    HRESULT hr = item->BindToHandler(nullptr, BHID_SFUIObject,
                                     extractIconIID,
                                     reinterpret_cast<void**>(location.GetAddressOf()));
    // Not every Shell handler exposes an underlying icon file. This is only
    // source-canvas metadata; GetImage remains the sole base-icon pixel extractor.
    if (hr == E_NOINTERFACE || hr == E_NOTIMPL || hr == MK_E_NOOBJECT)
        return S_FALSE;
    if (FAILED(hr)) return hr;
    std::vector<wchar_t> filename(32768, L'\0');
    int index = 0;
    UINT flags = 0;
    hr = location->GetIconLocation(0, filename.data(), static_cast<UINT>(filename.size()),
                                  &index, &flags);
    if (hr == S_FALSE) return S_FALSE;
    if (FAILED(hr)) return hr;
    if (flags & GIL_NOTFILENAME) {
        // Executable handlers can return a cache key rather than a resource
        // index. Recover dimensions from the file or the link's persisted
        // icon location, never interpret that cache key as a resource index.
        if (directory) return S_FALSE;
        index = 0;
        if (hasExtension(nativePath, L".lnk")) {
            ComPtr<IShellLinkW> link;
            hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(link.GetAddressOf()));
            if (FAILED(hr)) return hr;
            ComPtr<IPersistFile> persisted;
            hr = link.As(&persisted);
            if (FAILED(hr)) return hr;
            hr = persisted->Load(nativePath.c_str(), STGM_READ);
            if (FAILED(hr)) return hr;
            filename.assign(32768, L'\0');
            hr = link->GetIconLocation(filename.data(), static_cast<int>(filename.size()), &index);
            if (FAILED(hr)) return hr;
            if (!filename[0]) {
                index = 0;
                hr = link->GetPath(filename.data(), static_cast<int>(filename.size()),
                                   nullptr, SLGP_RAWPATH);
                if (FAILED(hr)) return hr;
                if (hr == S_FALSE || !filename[0]) return S_FALSE;
            }
        } else if (isBinaryIconFile(nativePath)) {
            filename.assign(nativePath.begin(), nativePath.end());
            filename.push_back(L'\0');
        } else {
            return S_FALSE;
        }
    }
    const DWORD needed = ExpandEnvironmentStringsW(filename.data(), nullptr, 0);
    if (!needed) return HRESULT_FROM_WIN32(GetLastError());
    std::vector<wchar_t> expanded(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(filename.data(), expanded.data(), needed);
    if (!written) return HRESULT_FROM_WIN32(GetLastError());
    if (written > needed) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    const std::wstring path(expanded.data());
    if (isIconFile(path))
        return icoCanvas(wic, path, largest);
    if (isBinaryIconFile(path))
        return binaryCanvas(wic, path, index, largest);
    return S_FALSE;
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

bool administratorToken()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD bytes = 0;
    const BOOL queried = GetTokenInformation(token, TokenElevationType, &type, sizeof(type), &bytes);
    CloseHandle(token);
    // A split (limited or elevated) token belongs to an administrator.
    return queried && type != TokenElevationTypeDefault;
}

bool runAsAdministratorLayer(const std::wstring& path)
{
    constexpr wchar_t layers[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        DWORD bytes = 0;
        if (RegGetValueW(root, layers, path.c_str(), RRF_RT_REG_SZ, nullptr, nullptr,
                         &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
            continue;
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, layers, path.c_str(), RRF_RT_REG_SZ, nullptr, value.data(),
                         &bytes) != ERROR_SUCCESS)
            continue;
        value.resize(std::wcslen(value.c_str()));
        std::size_t start = 0;
        while (start < value.size()) {
            const std::size_t end = std::min(value.find(L' ', start), value.size());
            constexpr wchar_t flag[] = L"RUNASADMIN";
            if (CompareStringOrdinal(value.data() + start, static_cast<int>(end - start), flag,
                                     static_cast<int>(std::size(flag) - 1), TRUE) == CSTR_EQUAL)
                return true;
            start = end + 1;
        }
    }
    return false;
}

enum class ExecutionLevel { unspecified, asInvoker, highestAvailable, requireAdministrator };

ExecutionLevel manifestLevel(const std::wstring& path)
{
    struct Module {
        HMODULE handle;
        ~Module() { if (handle) FreeLibrary(handle); }
    } module{LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
    if (!module.handle) return ExecutionLevel::unspecified;
    HRSRC resource = FindResourceW(module.handle,
                                   MAKEINTRESOURCEW(CREATEPROCESS_MANIFEST_RESOURCE_ID),
                                   RT_MANIFEST);
    if (!resource) return ExecutionLevel::unspecified;
    const DWORD bytes = SizeofResource(module.handle, resource);
    const auto* data = static_cast<const char*>(LockResource(LoadResource(module.handle, resource)));
    if (!data || !bytes) return ExecutionLevel::unspecified;
    std::string xml;
    if (bytes >= 2 && static_cast<unsigned char>(data[0]) == 0xFF &&
        static_cast<unsigned char>(data[1]) == 0xFE) {
        // UTF-16LE manifest; the markup of interest is ASCII.
        for (DWORD i = 2; i + 1 < bytes; i += 2)
            xml.push_back(data[i + 1] ? '?' : data[i]);
    } else {
        xml.assign(data, bytes);
    }
    const std::size_t element = xml.find("requestedExecutionLevel");
    if (element == std::string::npos) return ExecutionLevel::unspecified;
    const std::size_t close = xml.find('>', element);
    const std::string tag = xml.substr(element, close == std::string::npos ?
                                                    std::string::npos : close - element);
    std::size_t attribute = tag.find("level", sizeof("requestedExecutionLevel") - 1);
    while (attribute != std::string::npos) {
        std::size_t cursor = attribute + 5;
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])))
            ++cursor;
        if (cursor < tag.size() && tag[cursor] == '=') {
            ++cursor;
            while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])))
                ++cursor;
            if (cursor < tag.size() && (tag[cursor] == '"' || tag[cursor] == '\'')) {
                const std::size_t end = tag.find(tag[cursor], cursor + 1);
                const std::string level = tag.substr(cursor + 1, end == std::string::npos ?
                                                                    std::string::npos :
                                                                    end - cursor - 1);
                if (level == "requireAdministrator") return ExecutionLevel::requireAdministrator;
                if (level == "highestAvailable") return ExecutionLevel::highestAvailable;
                if (level == "asInvoker") return ExecutionLevel::asInvoker;
                return ExecutionLevel::unspecified;
            }
        }
        attribute = tag.find("level", attribute + 5);
    }
    return ExecutionLevel::unspecified;
}

bool executableRequiresElevation(const std::wstring& path)
{
    if (!hasExtension(path, L".exe")) return false;
    std::vector<wchar_t> full(32768, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()),
                                          full.data(), nullptr);
    if (length && length < full.size() && runAsAdministratorLayer(full.data())) return true;
    switch (manifestLevel(path)) {
    case ExecutionLevel::requireAdministrator: return true;
    case ExecutionLevel::highestAvailable: return administratorToken();
    default: return false;
    }
}

// Explorer draws the UAC shield itself; it is not part of the Shell icon or
// its overlay identifiers, so recover it from the same elevation metadata.
bool requiresElevation(const std::wstring& path)
{
    if (!uacEnabled()) return false;
    if (!hasExtension(path, L".lnk")) return executableRequiresElevation(path);
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.GetAddressOf()))))
        return false;
    ComPtr<IPersistFile> persisted;
    if (FAILED(link.As(&persisted)) || FAILED(persisted->Load(path.c_str(), STGM_READ)))
        return false;
    ComPtr<IShellLinkDataList> data;
    DWORD flags = 0;
    if (SUCCEEDED(link.As(&data)) && SUCCEEDED(data->GetFlags(&flags)) &&
        (flags & SLDF_RUNAS_USER))
        return true;
    std::vector<wchar_t> target(32768, L'\0');
    if (link->GetPath(target.data(), static_cast<int>(target.size()), nullptr,
                      SLGP_RAWPATH) != S_OK || !target[0])
        return false;
    const DWORD needed = ExpandEnvironmentStringsW(target.data(), nullptr, 0);
    if (!needed) return false;
    std::vector<wchar_t> expanded(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(target.data(), expanded.data(), needed);
    if (!written || written > needed) return false;
    return executableRequiresElevation(expanded.data());
}

HRESULT shieldIcon(UINT size, Icon& shield)
{
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    HRESULT hr = SHGetStockIconInfo(SIID_SHIELD, SHGSI_SYSICONINDEX, &info);
    if (FAILED(hr)) return hr;
    ComPtr<IImageList> images;
    hr = systemImageList(size, images);
    if (FAILED(hr)) return hr;
    HICON raw = nullptr;
    hr = images->GetIcon(info.iSysImageIndex, ILD_TRANSPARENT, &raw);
    shield.reset(raw);
    return SUCCEEDED(hr) && !shield ? E_FAIL : hr;
}

HRESULT bitmapPixels(IWICImagingFactory* wic, HBITMAP handle, UINT& width, UINT& height,
                     std::vector<unsigned char>& pixels)
{
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = wic->CreateBitmapFromHBITMAP(handle, nullptr, WICBitmapUseAlpha,
                                             bitmap.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = bitmap->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    UINT stride = 0, bytes = 0;
    if (!bufferSize(width, height, stride, bytes)) return WINCODEC_ERR_BADIMAGE;
    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = converter->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    pixels.resize(bytes);
    return converter->CopyPixels(nullptr, stride, bytes, pixels.data());
}

// When the source canvas is unknown, the Shell pads a small icon onto the
// requested canvas, adding a faint black shadow frame in the jumbo list.
// Recognize that padding only by an exact centered match with the Shell's own
// rendering at a smaller size; the content bounds merely select candidates.
HRESULT paddedCanvas(IShellItemImageFactory* factory, IWICImagingFactory* wic,
                     HBITMAP handle, Bitmap& native)
{
    UINT width = 0, height = 0;
    std::vector<unsigned char> large;
    HRESULT hr = bitmapPixels(wic, handle, width, height, large);
    if (FAILED(hr)) return hr;
    if (width != height || width <= 16) return S_OK;
    UINT left = width, top = height, right = 0, bottom = 0;
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) {
            const auto* pixel = large.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            if (!pixel[3] || (!pixel[0] && !pixel[1] && !pixel[2] && pixel[3] <= 64))
                continue;
            left = std::min(left, x); right = std::max(right, x);
            top = std::min(top, y); bottom = std::max(bottom, y);
        }
    if (right < left) return S_OK;
    for (UINT candidate : {16u, 20u, 24u, 32u, 40u, 48u, 64u, 72u, 80u, 96u, 128u, 256u}) {
        if (candidate >= width) break;
        const UINT offset = (width - candidate) / 2;
        if (left < offset || top < offset || right >= offset + candidate ||
            bottom >= offset + candidate)
            continue;
        HBITMAP raw = nullptr;
        hr = factory->GetImage(SIZE{static_cast<LONG>(candidate), static_cast<LONG>(candidate)},
                               SIIGBF_ICONONLY, &raw);
        Bitmap reduced(raw);
        if (FAILED(hr) || !reduced) continue;
        UINT smallWidth = 0, smallHeight = 0;
        std::vector<unsigned char> pixels;
        hr = bitmapPixels(wic, reduced.get(), smallWidth, smallHeight, pixels);
        if (FAILED(hr)) return hr;
        if (smallWidth != candidate || smallHeight != candidate) continue;
        bool same = true;
        for (UINT y = 0; same && y < candidate; ++y)
            same = std::memcmp(large.data() + ((static_cast<std::size_t>(offset) + y) * width +
                                               offset) * 4,
                               pixels.data() + static_cast<std::size_t>(y) * candidate * 4,
                               static_cast<std::size_t>(candidate) * 4) == 0;
        if (same) {
            native = std::move(reduced);
            return S_OK;
        }
    }
    return S_OK;
}

enum class Corner { bottomLeft, bottomRight };

HRESULT compositeOverlay(IWICImagingFactory* wic, HICON icon, UINT size, UINT box,
                         Corner corner, UINT stride, std::vector<unsigned char>& canvas)
{
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = wic->CreateBitmapFromHICON(icon, bitmap.GetAddressOf());
    if (FAILED(hr)) return hr;
    UINT width = 0, height = 0;
    hr = bitmap->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    UINT overlayStride = 0, overlayBytes = 0;
    if (!bufferSize(width, height, overlayStride, overlayBytes))
        return WINCODEC_ERR_BADIMAGE;

    // Badge artwork has its own resolution limit, independent of the base
    // icon. Keep its full transparent canvas, fit it in the corner box, and
    // anchor it at the requested bottom corner.
    ComPtr<IWICBitmapSource> fitted = bitmap;
    ComPtr<IWICFormatConverter> premultiplied;
    ComPtr<IWICBitmapScaler> scaler;
    box = std::max<UINT>(1, std::min(box, size));
    if (width > box || height > box) {
        const UINT extent = std::max(width, height);
        const UINT fitWidth = std::max<UINT>(1,
            static_cast<UINT>((static_cast<std::uint64_t>(width) * box + extent / 2) / extent));
        const UINT fitHeight = std::max<UINT>(1,
            static_cast<UINT>((static_cast<std::uint64_t>(height) * box + extent / 2) / extent));
        hr = wic->CreateFormatConverter(premultiplied.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = premultiplied->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0,
                                      WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return hr;
        hr = wic->CreateBitmapScaler(scaler.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = scaler->Initialize(premultiplied.Get(), fitWidth, fitHeight,
                                WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        fitted = scaler;
        width = fitWidth;
        height = fitHeight;
        if (!bufferSize(width, height, overlayStride, overlayBytes))
            return WINCODEC_ERR_BADIMAGE;
    }
    ComPtr<IWICFormatConverter> straight;
    hr = wic->CreateFormatConverter(straight.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = straight->Initialize(fitted.Get(), GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    std::vector<unsigned char> pixels(overlayBytes);
    hr = straight->CopyPixels(nullptr, overlayStride, overlayBytes, pixels.data());
    if (FAILED(hr)) return hr;
    const UINT originX = corner == Corner::bottomRight ? size - width : 0;
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) {
            const auto* pixel = pixels.data() + static_cast<std::size_t>(y) * overlayStride + x * 4;
            auto* base = canvas.data() + static_cast<std::size_t>(size - height + y) * stride +
                         static_cast<std::size_t>(originX + x) * 4;
            const unsigned alpha = pixel[3], baseAlpha = base[3];
            const unsigned combined = alpha * 255 + baseAlpha * (255 - alpha);
            if (!combined) continue;
            for (unsigned channel = 0; channel < 3; ++channel)
                base[channel] = static_cast<unsigned char>(
                    (pixel[channel] * alpha * 255 +
                     base[channel] * baseAlpha * (255 - alpha) + combined / 2) / combined);
            base[3] = static_cast<unsigned char>((combined + 127) / 255);
        }
    return S_OK;
}

Result encode(IWICImagingFactory* wic, HBITMAP handle, UINT size, HICON overlay, HICON shield)
{
    ComPtr<IWICBitmap> bitmap;
    // Shell image-factory output uses straight alpha, not PBGRA. In particular,
    // do not infer premultiplication from bmBitsPixel, or unpremultiply twice.
    // Controlled native Windows ICO/folder fixtures verify RGBA (191,95,31,128)
    // for source (192,96,32,128), not the premultiplied (96,48,16,128).
    // ISharedBitmap::GetFormat only reports
    // alpha presence; InitializeBitmap(UNKNOWN) adds no provenance information.
    HRESULT hr = wic->CreateBitmapFromHBITMAP(handle, nullptr, WICBitmapUseAlpha,
                                             bitmap.GetAddressOf());
    if (FAILED(hr)) return failure("CreateBitmapFromHBITMAP", hr);

    UINT width = 0, height = 0, sourceStride = 0, sourceBytes = 0;
    hr = bitmap->GetSize(&width, &height);
    if (FAILED(hr)) return failure("Bitmap GetSize", hr);
    if (!bufferSize(width, height, sourceStride, sourceBytes))
        return {{}, "Shell bitmap dimensions exceed WIC UINT buffer limits"};

    // Filter reductions in premultiplied colors; copy pixels for enlargement.
    // Scale the complete canvas, not the nontransparent bounding box: padding
    // inside legacy/custom icons is part of the artwork.
    UINT fitWidth = size, fitHeight = size;
    if (width > height)
        fitHeight = std::max<UINT>(1, static_cast<UINT>(
            (static_cast<std::uint64_t>(height) * size + width / 2) / width));
    else if (height > width)
        fitWidth = std::max<UINT>(1, static_cast<UINT>(
            (static_cast<std::uint64_t>(width) * size + height / 2) / height));

    ComPtr<IWICBitmapSource> fitted = bitmap;
    ComPtr<IWICFormatConverter> premultiplied;
    ComPtr<IWICBitmapScaler> scaler;
    if (width != fitWidth || height != fitHeight) {
        const bool enlarge = fitWidth > width || fitHeight > height;
        // Nearest-neighbor enlargement copies straight-alpha source pixels
        // exactly; only filtered reduction needs premultiplied colors.
        if (!enlarge) {
            hr = wic->CreateFormatConverter(premultiplied.GetAddressOf());
            if (FAILED(hr)) return failure("Create premultiplied converter", hr);
            hr = premultiplied->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0,
                                          WICBitmapPaletteTypeCustom);
            if (FAILED(hr)) return failure("Initialize premultiplied converter", hr);
        }
        hr = wic->CreateBitmapScaler(scaler.GetAddressOf());
        if (FAILED(hr)) return failure("CreateBitmapScaler", hr);
        IWICBitmapSource* source = bitmap.Get();
        if (!enlarge) source = premultiplied.Get();
        hr = scaler->Initialize(source,
                                fitWidth, fitHeight, enlarge ?
                                WICBitmapInterpolationModeNearestNeighbor :
                                WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return failure("Initialize bitmap scaler", hr);
        fitted = scaler;
    }
    ComPtr<IWICFormatConverter> straight;
    hr = wic->CreateFormatConverter(straight.GetAddressOf());
    if (FAILED(hr)) return failure("Create straight-alpha converter", hr);
    hr = straight->Initialize(fitted.Get(), GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return failure("Initialize straight-alpha converter", hr);

    UINT stride = 0, bytes = 0, fitStride = 0, fitBytes = 0;
    if (!bufferSize(size, size, stride, bytes) ||
        !bufferSize(fitWidth, fitHeight, fitStride, fitBytes))
        return {{}, "Output dimensions exceed WIC UINT buffer limits"};
    std::vector<unsigned char> canvas(bytes, 0), pixels(fitBytes);
    hr = straight->CopyPixels(nullptr, fitStride, fitBytes, pixels.data());
    if (FAILED(hr)) return failure("CopyPixels", hr);
    const UINT left = (size - fitWidth) / 2, top = (size - fitHeight) / 2;
    for (UINT y = 0; y < fitHeight; ++y)
        std::memcpy(canvas.data() + static_cast<std::size_t>(top + y) * stride +
                        left * 4,
                    pixels.data() + static_cast<std::size_t>(y) * fitStride,
                    fitStride);

    if (shield) {
        hr = compositeOverlay(wic, shield, size, size / 2, Corner::bottomRight, stride, canvas);
        if (FAILED(hr)) return failure("Composite UAC shield", hr);
    }
    if (overlay) {
        hr = compositeOverlay(wic, overlay, size, size, Corner::bottomLeft, stride, canvas);
        if (FAILED(hr)) return failure("Composite Shell icon overlay", hr);
    }

    ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf());
    if (FAILED(hr)) return failure("CreateStreamOnHGlobal", hr);
    ComPtr<IWICBitmapEncoder> encoder;
    hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
    if (FAILED(hr)) return failure("Create PNG encoder", hr);
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return failure("Initialize PNG encoder", hr);
    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
    if (FAILED(hr)) return failure("Create PNG frame", hr);
    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return failure("Initialize PNG frame", hr);
    hr = frame->SetSize(size, size);
    if (FAILED(hr)) return failure("Set PNG dimensions", hr);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return failure("Set PNG pixel format", hr);
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
        return {{}, "PNG encoder did not accept straight-alpha BGRA"};
    hr = frame->WritePixels(size, stride, bytes, canvas.data());
    if (FAILED(hr)) return failure("Write PNG pixels", hr);
    hr = frame->Commit();
    if (FAILED(hr)) return failure("Commit PNG frame", hr);
    hr = encoder->Commit();
    if (FAILED(hr)) return failure("Commit PNG encoder", hr);
    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr)) return failure("Get PNG stream length", hr);
    if (!stat.cbSize.QuadPart || stat.cbSize.QuadPart > INT32_MAX)
        return {{}, "PNG stream length exceeds signed 32-bit buffer limits"};
    LARGE_INTEGER start{};
    hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return failure("Rewind PNG stream", hr);
    Result result;
    result.png.resize(static_cast<std::size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    hr = stream->Read(result.png.data(), static_cast<ULONG>(result.png.size()), &read);
    if (FAILED(hr)) return failure("Read PNG stream", hr);
    if (read != result.png.size())
        return {{}, "PNG stream returned fewer bytes than its reported length"};
    return result;
}

Result extract(const std::u16string& path, std::int32_t size, OverlayPolicy overlays)
{
    Apartment apartment;
    if (apartment.status == RPC_E_CHANGED_MODE)
        return {{}, "Shell worker already has an incompatible COM apartment"};
    if (apartment.status != S_OK && apartment.status != S_FALSE)
        return failure("Initialize shell worker COM apartment", apartment.status);

    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows requires UTF-16");
    std::wstring nativePath(path.begin(), path.end());
    // Folder objects append a separator even when referencing a file or link.
    while (nativePath.size() > 1 && (nativePath.back() == L'\\' || nativePath.back() == L'/') &&
           nativePath[nativePath.size() - 2] != L':')
        nativePath.pop_back();
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return failure("GetFileAttributesW", HRESULT_FROM_WIN32(GetLastError()));
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    ComPtr<IShellItemImageFactory> imageFactory;
    HRESULT hr = SHCreateItemFromParsingName(nativePath.c_str(), nullptr,
                                             IID_PPV_ARGS(imageFactory.GetAddressOf()));
    if (FAILED(hr)) return failure("SHCreateItemFromParsingName", hr);
    HBITMAP raw = nullptr;
    hr = imageFactory->GetImage(SIZE{size, size},
        SIIGBF_ICONONLY, &raw);
    Bitmap bitmap(raw);
    if (FAILED(hr)) return failure("IShellItemImageFactory::GetImage", hr);
    if (!bitmap) return {{}, "Shell returned a null icon bitmap"};

    ComPtr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(wic.GetAddressOf()));
    if (FAILED(hr)) return failure("Create WIC imaging factory", hr);
    ComPtr<IShellItem> item;
    hr = imageFactory.As(&item);
    if (FAILED(hr)) return failure("Query Shell item", hr);
    UINT nativeCanvas = 0;
    hr = iconFileCanvas(item.Get(), wic.Get(), nativePath, directory, nativeCanvas);
    if (FAILED(hr)) return failure("Read Shell icon file canvas", hr);
    if (nativeCanvas && nativeCanvas < static_cast<UINT>(size)) {
        // Shell can return an oversized canvas for a legacy icon even without
        // SCALEUP. Re-extract at its real resource canvas size
        // and let WIC scale the whole canvas, including intentional margins.
        // Never derive that canvas from the nontransparent bounding box.
        raw = nullptr;
        hr = imageFactory->GetImage(SIZE{static_cast<LONG>(nativeCanvas),
                                        static_cast<LONG>(nativeCanvas)},
            SIIGBF_ICONONLY, &raw);
        Bitmap native(raw);
        if (FAILED(hr)) return failure("GetImage at native icon canvas size", hr);
        if (!native) return {{}, "Shell returned a null native icon bitmap"};
        bitmap = std::move(native);
    } else if (!nativeCanvas) {
        Bitmap native;
        hr = paddedCanvas(imageFactory.Get(), wic.Get(), bitmap.get(), native);
        if (FAILED(hr)) return failure("Detect Shell icon padding", hr);
        if (native) bitmap = std::move(native);
    }
    Icon overlay;
    hr = shellOverlay(nativePath, static_cast<UINT>(size), overlays, overlay);
    if (FAILED(hr)) return failure("Read Shell icon overlay", hr);
    Icon shield;
    if (overlays != OverlayPolicy::none && !directory && requiresElevation(nativePath)) {
        hr = shieldIcon(std::max<UINT>(1, static_cast<UINT>(size) / 2), shield);
        if (FAILED(hr)) return failure("Read UAC shield icon", hr);
    }
    return encode(wic.Get(), bitmap.get(), static_cast<UINT>(size), overlay.get(), shield.get());
}
} // namespace

Result load(const std::u16string& path, std::int32_t size, OverlayPolicy overlays)
{
    if (!validSize(size)) return {{}, "Invalid icon size"};
    if (path.empty() || path.find(u'\0') != std::u16string::npos)
        return {{}, "Path must be nonempty and contain no embedded NUL"};
    if (overlays != OverlayPolicy::system && overlays != OverlayPolicy::noShortcut &&
        overlays != OverlayPolicy::none)
        return {{}, "Invalid icon overlay policy"};
    try {
        Result result;
        // All Shell and WIC objects are created/released on this dedicated STA.
        // The caller's COM state (including an existing MTA) is left untouched.
        std::thread worker([&] {
            try {
                result = extract(path, size, overlays);
            } catch (const std::bad_alloc&) {
                result = {{}, "Insufficient memory while extracting icon"};
            } catch (const std::length_error&) {
                result = {{}, "Icon buffer exceeds container limits"};
            }
        });
        worker.join();
        return result;
    } catch (const std::system_error& error) {
        return {{}, std::string("Shell worker failure: ") + error.what()};
    } catch (const std::bad_alloc&) {
        return {{}, "Insufficient memory while starting shell worker"};
    }
}
} // namespace icons
