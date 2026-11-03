#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "icon.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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

bool bufferSize(UINT width, UINT height, UINT& stride, UINT& bytes)
{
    const auto count = static_cast<std::uint64_t>(width) * height;
    if (!width || !height || count > static_cast<UINT>(INT32_MAX) / 4)
        return false;
    stride = width * 4;
    bytes = static_cast<UINT>(count * 4);
    return true;
}

bool isIconFile(const std::wstring& path)
{
    return path.size() >= 4 &&
        CompareStringOrdinal(path.data() + path.size() - 4, 4, L".ico", 4, TRUE) == CSTR_EQUAL;
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
    // source-canvas metadata; GetImage remains the sole pixel extractor.
    if (hr == E_NOINTERFACE || hr == E_NOTIMPL || hr == MK_E_NOOBJECT)
        return S_FALSE;
    if (FAILED(hr)) return hr;
    std::vector<wchar_t> filename(32768, L'\0');
    int index = 0;
    UINT flags = 0;
    hr = location->GetIconLocation(0, filename.data(), static_cast<UINT>(filename.size()),
                                  &index, &flags);
    if (hr == S_FALSE || (SUCCEEDED(hr) && (flags & GIL_NOTFILENAME)))
        return S_FALSE;
    if (FAILED(hr)) return hr;
    const DWORD needed = ExpandEnvironmentStringsW(filename.data(), nullptr, 0);
    if (!needed) return HRESULT_FROM_WIN32(GetLastError());
    std::vector<wchar_t> expanded(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(filename.data(), expanded.data(), needed);
    if (!written) return HRESULT_FROM_WIN32(GetLastError());
    if (written > needed) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    const std::wstring path(expanded.data());
    if (!isIconFile(path))
        return S_FALSE;
    return icoCanvas(wic, path, largest);
}

Result encode(IWICImagingFactory* wic, HBITMAP handle, UINT size)
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
        return {{}, "Shell bitmap dimensions exceed signed 32-bit buffer limits"};

    // Resample premultiplied colors, then convert back to straight PNG colors.
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
        hr = wic->CreateFormatConverter(premultiplied.GetAddressOf());
        if (FAILED(hr)) return failure("Create premultiplied converter", hr);
        hr = premultiplied->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA,
                                      WICBitmapDitherTypeNone, nullptr, 0,
                                      WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return failure("Initialize premultiplied converter", hr);
        hr = wic->CreateBitmapScaler(scaler.GetAddressOf());
        if (FAILED(hr)) return failure("CreateBitmapScaler", hr);
        hr = scaler->Initialize(premultiplied.Get(), fitWidth, fitHeight,
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
        return {{}, "Output dimensions exceed signed 32-bit buffer limits"};
    std::vector<unsigned char> canvas(bytes, 0), pixels(fitBytes);
    hr = straight->CopyPixels(nullptr, fitStride, fitBytes, pixels.data());
    if (FAILED(hr)) return failure("CopyPixels", hr);
    const UINT left = (size - fitWidth) / 2, top = (size - fitHeight) / 2;
    for (UINT y = 0; y < fitHeight; ++y)
        std::memcpy(canvas.data() + static_cast<std::size_t>(top + y) * stride +
                        left * 4,
                    pixels.data() + static_cast<std::size_t>(y) * fitStride,
                    fitStride);

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

Result extract(const std::u16string& path, std::int32_t size, Kind kind)
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
        static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_SCALEUP), &raw);
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
        // On Windows, SCALEUP can return an oversized, partly uninitialized
        // bitmap for a legacy ICO. Re-extract at its real resource canvas size
        // and let WIC scale the whole canvas, including intentional margins.
        // Never derive that canvas from the nontransparent bounding box.
        raw = nullptr;
        hr = imageFactory->GetImage(SIZE{static_cast<LONG>(nativeCanvas),
                                        static_cast<LONG>(nativeCanvas)},
            static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_SCALEUP), &raw);
        Bitmap native(raw);
        if (FAILED(hr)) return failure("GetImage at native icon canvas size", hr);
        if (!native) return {{}, "Shell returned a null native icon bitmap"};
        bitmap = std::move(native);
    }
    return encode(wic.Get(), bitmap.get(), static_cast<UINT>(size));
}
} // namespace

Result load(const std::u16string& path, std::int32_t size, Kind kind)
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
                result = extract(path, size, kind);
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
