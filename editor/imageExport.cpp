#include "imageExport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace jitnodes::editor {

namespace {

using Microsoft::WRL::ComPtr;

std::string failure(const char* step, HRESULT hr) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "export failed in %s (0x%08lX)", step, static_cast<unsigned long>(hr));
    return buf;
}

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // RPC_E_CHANGED_MODE means the thread already runs COM in another mode, which is fine to use.
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

}  // namespace

std::string exportImage(const Image& image, const std::string& path, int jpegQuality) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.size() != static_cast<size_t>(image.width) * static_cast<size_t>(image.height))
        return "there is no image to export";
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool jpeg = ext == ".jpg" || ext == ".jpeg";
    if (!jpeg && ext != ".png") return "export needs a .png, .jpg or .jpeg file name";

    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLength <= 0) return "the file name is not valid text";
    std::wstring widePath(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath.data(), wideLength);

    // Both encoders take opaque 24 bit BGR.
    const UINT width = static_cast<UINT>(image.width), height = static_cast<UINT>(image.height);
    const UINT stride = width * 3;
    std::vector<BYTE> bgr(static_cast<size_t>(stride) * height);
    for (size_t i = 0; i < image.pixels.size(); ++i) {
        const uint32_t px = image.pixels[i];
        bgr[i * 3] = static_cast<BYTE>(px & 255u);
        bgr[i * 3 + 1] = static_cast<BYTE>((px >> 8) & 255u);
        bgr[i * 3 + 2] = static_cast<BYTE>((px >> 16) & 255u);
    }

    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return failure("creating the imaging factory", hr);
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return failure("opening the file", hr);
    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return failure("creating the encoder", hr);
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    hr = encoder->CreateNewFrame(&frame, &options);
    if (FAILED(hr)) return failure("creating the frame", hr);
    if (jpeg) {
        PROPBAG2 option = {};
        wchar_t name[] = L"ImageQuality";
        option.pstrName = name;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(std::clamp(jpegQuality, 1, 100)) / 100.0f;
        options->Write(1, &option, &value);
    }
    hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(height, stride, static_cast<UINT>(bgr.size()), bgr.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) return failure("writing the pixels", hr);
    return {};
}

}  // namespace jitnodes::editor
