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
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>

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

HRESULT shortcutOverlay(IShellItem* item, const std::wstring& path, Icon& overlay)
{
    SFGAOF attributes = 0;
    HRESULT hr = item->GetAttributes(SFGAO_LINK, &attributes);
    if (FAILED(hr) || !(attributes & SFGAO_LINK)) return hr;
    SHFILEINFOW info{};
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX))
        return E_FAIL;
    Icon decorated(info.hIcon);
    const unsigned index = static_cast<unsigned>(info.iIcon) >> 24;
    // Respect the user's Shell configuration, including disabled link badges.
    if (!index) return S_OK;
    ComPtr<IImageList> images;
    hr = SHGetImageList(SHIL_LARGE, IID_PPV_ARGS(images.GetAddressOf()));
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

HRESULT binaryCanvas(const std::wstring& path, int index, UINT& largest)
{
    struct Module {
        HMODULE handle;
        ~Module() { if (handle) FreeLibrary(handle); }
    } module{LoadLibraryExW(path.c_str(), nullptr,
                           LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)};
    if (!module.handle) {
        const DWORD error = GetLastError();
        return error == ERROR_BAD_EXE_FORMAT ? S_FALSE : HRESULT_FROM_WIN32(error);
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
                       const std::wstring& nativePath, Kind kind, UINT& largest)
{
    largest = 0;
    if (kind == Kind::file && isIconFile(nativePath))
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
        if (kind != Kind::file) return S_FALSE;
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
        return binaryCanvas(path, index, largest);
    return S_FALSE;
}

HRESULT compositeOverlay(IWICImagingFactory* wic, HICON icon, UINT size,
                         UINT stride, UINT bytes, std::vector<unsigned char>& canvas)
{
    ComPtr<IWICBitmap> bitmap;
    HRESULT hr = wic->CreateBitmapFromHICON(icon, bitmap.GetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapScaler> scaler;
    hr = wic->CreateBitmapScaler(scaler.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = scaler->Initialize(bitmap.Get(), size, size, WICBitmapInterpolationModeNearestNeighbor);
    if (FAILED(hr)) return hr;
    ComPtr<IWICFormatConverter> straight;
    hr = wic->CreateFormatConverter(straight.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = straight->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;
    std::vector<unsigned char> pixels(bytes);
    hr = straight->CopyPixels(nullptr, stride, bytes, pixels.data());
    if (FAILED(hr)) return hr;
    for (std::size_t i = 0; i < canvas.size(); i += 4) {
        const unsigned alpha = pixels[i + 3], baseAlpha = canvas[i + 3];
        const unsigned combined = alpha * 255 + baseAlpha * (255 - alpha);
        if (!combined) continue;
        for (unsigned channel = 0; channel < 3; ++channel)
            canvas[i + channel] = static_cast<unsigned char>(
                (pixels[i + channel] * alpha * 255 +
                 canvas[i + channel] * baseAlpha * (255 - alpha) + combined / 2) / combined);
        canvas[i + 3] = static_cast<unsigned char>((combined + 127) / 255);
    }
    return S_OK;
}

Result encode(IWICImagingFactory* wic, HBITMAP handle, UINT size, HICON overlay)
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

    if (overlay) {
        hr = compositeOverlay(wic, overlay, size, stride, bytes, canvas);
        if (FAILED(hr)) return failure("Composite shortcut overlay", hr);
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

Result extract(const std::u16string& path, std::int32_t size, Kind kind, bool hideShortcut)
{
    Apartment apartment;
    if (apartment.status == RPC_E_CHANGED_MODE)
        return {{}, "Shell worker already has an incompatible COM apartment"};
    if (apartment.status != S_OK && apartment.status != S_FALSE)
        return failure("Initialize shell worker COM apartment", apartment.status);

    static_assert(sizeof(wchar_t) == sizeof(char16_t), "Windows requires UTF-16");
    const std::wstring nativePath(path.begin(), path.end());
    const DWORD attributes = GetFileAttributesW(nativePath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return failure("GetFileAttributesW", HRESULT_FROM_WIN32(GetLastError()));
    if (((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != (kind == Kind::folder))
        return {{}, "Path is not the requested file/folder kind"};

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
    hr = iconFileCanvas(item.Get(), wic.Get(), nativePath, kind, nativeCanvas);
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
    }
    Icon overlay;
    if (!hideShortcut) {
        hr = shortcutOverlay(item.Get(), nativePath, overlay);
        if (FAILED(hr)) return failure("Read Shell shortcut overlay", hr);
    }
    return encode(wic.Get(), bitmap.get(), static_cast<UINT>(size), overlay.get());
}
} // namespace

Result load(const std::u16string& path, std::int32_t size, Kind kind, bool hideShortcut)
{
    if (!validSize(size)) return {{}, "Invalid icon size"};
    if (path.empty() || path.find(u'\0') != std::u16string::npos)
        return {{}, "Path must be nonempty and contain no embedded NUL"};
    if (kind != Kind::file && kind != Kind::folder)
        return {{}, "Invalid icon kind"};
    try {
        Result result;
        // All Shell and WIC objects are created/released on this dedicated STA.
        // The caller's COM state (including an existing MTA) is left untouched.
        std::thread worker([&] {
            try {
                result = extract(path, size, kind, hideShortcut);
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
