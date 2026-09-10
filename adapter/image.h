// shittim-logon / render / image.h
//
// PNG load and save through WIC, the imaging component that ships with Windows.
//
// No third-party image library is used on purpose. The Spine atlas pages are
// straight-alpha masters -- The source art was exported such that Xcode's COMPRESS_PNG_FILES
// silently premultiplied them and corrupted every semi-transparent edge -- so the
// decode path has to be one whose alpha handling is known and controllable.
// WIC's 32bppRGBA is straight alpha by definition; 32bppPRGBA is the premultiplied
// one. Asking for the former explicitly means the question never comes up again.
//
// Windows-only, which is the whole point: everything downstream of this is a
// credential provider and a secure-desktop renderer.

#pragma once

// Platform split: the WIC backend below is Windows-only. On other platforms
// image_mac.h provides the same sl::Image / loadPng / savePng API over
// ImageIO, with the same straight-alpha RGBA8 contract.
#if defined(_WIN32)

// NOMINMAX before windows.h, always. Without it windows.h defines min and max as
// function-like macros, and every std::min / std::max further down the include
// graph fails with C4002 and a cascade of syntax errors that point at the *user*
// code rather than at the header that broke it. raster.h is the victim here
// because image.h is what drags windows.h in.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace sl {

struct Image {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // straight alpha, 4 bytes per pixel, row-major

    const uint8_t* pixel(int x, int y) const { return &rgba[(size_t(y) * width + x) * 4]; }
    uint8_t*       pixel(int x, int y)       { return &rgba[(size_t(y) * width + x) * 4]; }
};

namespace detail {

// COM is initialised once per process. Renderers that already did it get
// RPC_E_CHANGED_MODE back, which is not a failure for our purposes.
inline void initCom() {
    static bool done = false;
    if (done) return;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("CoInitializeEx failed");
    }
    done = true;
}

inline IWICImagingFactory* factory() {
    static IWICImagingFactory* f = nullptr;
    if (!f) {
        initCom();
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&f));
        if (FAILED(hr)) throw std::runtime_error("could not create the WIC factory");
    }
    return f;
}

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

template <class T> struct Released {
    T* p = nullptr;
    ~Released() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

} // namespace detail

inline Image loadPng(const std::string& path) {
    using namespace detail;
    auto* fac = factory();
    std::wstring wpath = widen(path);

    Released<IWICBitmapDecoder> dec;
    if (FAILED(fac->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &dec)))
        throw std::runtime_error("could not open " + path);

    Released<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame)))
        throw std::runtime_error("no frame 0 in " + path);

    // 32bppRGBA is straight alpha. Never ask for 32bppPRGBA here.
    Released<IWICFormatConverter> conv;
    if (FAILED(fac->CreateFormatConverter(&conv)))
        throw std::runtime_error("could not create a WIC format converter");
    if (FAILED(conv->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        throw std::runtime_error("could not convert " + path + " to 32bppRGBA");

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);

    Image img;
    img.width  = (int)w;
    img.height = (int)h;
    img.rgba.resize(size_t(w) * h * 4);

    const UINT stride = w * 4;
    if (FAILED(conv->CopyPixels(nullptr, stride, (UINT)img.rgba.size(), img.rgba.data())))
        throw std::runtime_error("could not read pixels from " + path);

    return img;
}

inline void savePng(const Image& img, const std::string& path) {
    using namespace detail;
    auto* fac = factory();
    std::wstring wpath = widen(path);

    Released<IWICStream> stream;
    if (FAILED(fac->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)))
        throw std::runtime_error("could not open " + path + " for writing");

    Released<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.p, WICBitmapEncoderNoCache)))
        throw std::runtime_error("could not create a PNG encoder");

    Released<IWICBitmapFrameEncode> frame;
    IPropertyBag2* props = nullptr;
    if (FAILED(enc->CreateNewFrame(&frame, &props)))
        throw std::runtime_error("could not create an encoder frame");
    if (props) { frame->Initialize(props); props->Release(); }

    frame->SetSize((UINT)img.width, (UINT)img.height);

    // WIC's PNG encoder does not accept 32bppRGBA. SetPixelFormat negotiates to the
    // nearest format it does support and writes the choice back, which for a
    // straight-alpha 32-bit request is 32bppBGRA -- so the channel order has to be
    // swapped on the way out. Asking for BGRA directly makes that explicit rather
    // than discovering it from a failed comparison.
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (fmt != GUID_WICPixelFormat32bppBGRA)
        throw std::runtime_error("the PNG encoder would not take 32bppBGRA");

    std::vector<uint8_t> bgra(img.rgba.size());
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        bgra[i + 0] = img.rgba[i + 2];
        bgra[i + 1] = img.rgba[i + 1];
        bgra[i + 2] = img.rgba[i + 0];
        bgra[i + 3] = img.rgba[i + 3];
    }

    const UINT stride = (UINT)img.width * 4;
    if (FAILED(frame->WritePixels((UINT)img.height, stride,
                                  (UINT)bgra.size(), bgra.data())))
        throw std::runtime_error("could not write pixels to " + path);

    if (FAILED(frame->Commit()) || FAILED(enc->Commit()))
        throw std::runtime_error("could not commit " + path);
}

} // namespace sl

#else
#include "image_mac.h"
#endif
