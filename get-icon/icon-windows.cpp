#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "icon.h"

#include <windows.h>
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

Result encode(IWICImagingFactory* wic, HBITMAP handle, UINT size)
{
    ComPtr<IWICBitmap> bitmap;
    // Shell image-factory output uses straight alpha, not PBGRA. In particular,
    // do not infer premultiplication from bmBitsPixel, or unpremultiply twice.
    // Microsoft's PowerToys ShellItemImageFactoryIconExtractor uses the same
    // straight-alpha interpretation (commit 6061fcaeef2430a3d52e056468c9ecf9dbde953f).
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
