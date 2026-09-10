// shittim-logon / render / image_mac.h
//
// macOS backend of sl::Image / loadPng / savePng — the ImageIO counterpart of
// image.h's WIC implementation. Same API, same straight-alpha RGBA8 contract.
//
// Why the unpremultiply step: CoreGraphics bitmap contexts only support
// premultiplied alpha, so the decode path is RGBA-premultiplied and the result
// is converted back to straight alpha in place. Spine atlases are straight-
// alpha masters; getting this wrong shows up as dark fringes on every
// semi-transparent edge.
//
// The context is drawn with a vertical flip because CGContextDrawImage works
// bottom-up while sl::Image is row-major top-down (matching WIC's CopyPixels).

#pragma once

#include <ApplicationServices/ApplicationServices.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace sl {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // straight alpha, 4 bytes per pixel, row-major

    const uint8_t* pixel(int x, int y) const { return &rgba[(size_t(y) * width + x) * 4]; }
    uint8_t*       pixel(int x, int y)       { return &rgba[(size_t(y) * width + x) * 4]; }
};

namespace detail {

inline CFURLRef makeFileURL(const std::string& path) {
    return CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path.c_str()), path.size(), false);
}

inline void unpremultiplyInPlace(std::vector<uint8_t>& px) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const unsigned a = px[i + 3];
        if (a == 0) {
            px[i] = px[i + 1] = px[i + 2] = 0;
        } else if (a != 255) {
            for (int c = 0; c < 3; ++c)
                px[i + c] = uint8_t((unsigned(px[i + c]) * 255u + a / 2u) / a);
        }
    }
}

} // namespace detail

inline Image loadPng(const std::string& path) {
    using namespace detail;
    CFURLRef url = makeFileURL(path);
    if (!url) throw std::runtime_error("could not open " + path);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (!src) throw std::runtime_error("could not open " + path);
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) throw std::runtime_error("no frame 0 in " + path);

    const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    Image out;
    out.width = int(w);
    out.height = int(h);
    out.rgba.resize(w * h * 4);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), w, h, 8, w * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        CGImageRelease(img);
        throw std::runtime_error("could not create bitmap context for " + path);
    }
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextTranslateCTM(ctx, 0, CGFloat(h));
    CGContextScaleCTM(ctx, 1.0, -1.0);          // bottom-up draw -> top-down rows
    CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), img);
    CGContextRelease(ctx);
    CGImageRelease(img);

    detail::unpremultiplyInPlace(out.rgba);
    return out;
}

inline void savePng(const Image& img, const std::string& path) {
    using namespace detail;
    CFURLRef url = makeFileURL(path);
    if (!url) throw std::runtime_error("could not open " + path + " for writing");
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url,
        CFSTR("public.png"), 1, nullptr);
    CFRelease(url);
    if (!dst) throw std::runtime_error("could not create PNG destination for " + path);

    const size_t w = size_t(img.width), h = size_t(img.height);
    std::vector<uint8_t> premul = img.rgba;
    for (size_t i = 0; i + 3 < premul.size(); i += 4) {
        const unsigned a = premul[i + 3];
        if (a != 255)
            for (int c = 0; c < 3; ++c)
                premul[i + c] = uint8_t((unsigned(premul[i + c]) * a + 127u) / 255u);
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(premul.data(), w, h, 8, w * 4, cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    CGImageRef out = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!out) {
        CFRelease(dst);
        throw std::runtime_error("could not create CGImage for " + path);
    }
    CGImageDestinationAddImage(dst, out, nullptr);
    const bool ok = CGImageDestinationFinalize(dst);
    CGImageRelease(out);
    CFRelease(dst);
    if (!ok) throw std::runtime_error("could not write " + path);
}

} // namespace sl
