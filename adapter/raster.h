// shittim-logon / render / raster.h
//
// A software rasteriser for textured, alpha-blended triangle lists.
//
// Spine's runtime does not draw anything itself: it poses the skeleton and hands
// back batches of triangles -- positions, UVs, per-vertex colours, indices, a
// texture and a blend mode. Supplying the rasteriser is the whole of the
// integration, so this is deliberately written against that shape and nothing else.
//
// Why software rather than D3D, for now:
//
//   * It cannot fail for GPU reasons, which matters because the lab VM has no GPU
//     acceleration at all. A renderer that only works on the host would be
//     untestable in exactly the environment built to test it.
//   * The first deliverable is a still frame. Nothing here needs to be real-time.
//   * It isolates the question being answered -- do these assets load and compose
//     correctly on Windows -- from every question about the graphics stack.
//
// Stage 3 will need a D3D path for animation. The parts above this line (atlas,
// skeleton, track setup) carry over unchanged; only the triangle sink is replaced.
//
// Conventions: straight (non-premultiplied) alpha throughout, because the atlas
// pages are straight-alpha masters. Bilinear sampling, because both atlases ask for
// `filter: Linear,Linear` and the logon screen is a minification case.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sl {

struct Vertex {
    float x, y;        // target pixels
    float u, v;        // 0..1 texture space
    float r, g, b, a;  // 0..1, straight alpha, multiplied into the sample
};

enum class Blend { Normal, Additive, Multiply, Screen };

// Source texture: straight-alpha RGBA8, as produced by loadPng.
struct TextureView {
    const uint8_t* rgba = nullptr;
    int width  = 0;
    int height = 0;
};

// Accumulation target. Float, because a scene this layered blends hundreds of
// times per pixel and 8-bit rounding accumulates visible banding in the gradients
// these backgrounds are made of.
struct Target {
    int width  = 0;
    int height = 0;
    std::vector<float> rgba;   // straight alpha

    void resize(int w, int h) {
        width = w; height = h;
        rgba.assign(size_t(w) * h * 4, 0.0f);
    }
    void clear(float r, float g, float b, float a) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            rgba[i] = r; rgba[i+1] = g; rgba[i+2] = b; rgba[i+3] = a;
        }
    }
};

namespace detail {

inline void sampleBilinear(const TextureView& t, float u, float v, float out[4]) {
    // Clamp rather than wrap: every atlas here is `repeat: none`, and wrapping
    // would pull neighbouring regions into a mesh's edge pixels.
    float fx = u * t.width  - 0.5f;
    float fy = v * t.height - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float ax = fx - x0, ay = fy - y0;

    auto texel = [&](int x, int y, float o[4]) {
        x = std::min(std::max(x, 0), t.width  - 1);
        y = std::min(std::max(y, 0), t.height - 1);
        const uint8_t* p = t.rgba + (size_t(y) * t.width + x) * 4;
        o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f;
        o[2] = p[2] / 255.0f; o[3] = p[3] / 255.0f;
    };

    float c00[4], c10[4], c01[4], c11[4];
    texel(x0,     y0,     c00);
    texel(x0 + 1, y0,     c10);
    texel(x0,     y0 + 1, c01);
    texel(x0 + 1, y0 + 1, c11);

    for (int i = 0; i < 4; i++) {
        float top    = c00[i] + (c10[i] - c00[i]) * ax;
        float bottom = c01[i] + (c11[i] - c01[i]) * ax;
        out[i] = top + (bottom - top) * ay;
    }
}

inline void blendPixel(float* dst, const float src[4], float alpha, Blend mode) {
    const float a = alpha;
    switch (mode) {
    case Blend::Normal:
        for (int i = 0; i < 3; i++) dst[i] = src[i] * a + dst[i] * (1.0f - a);
        dst[3] = a + dst[3] * (1.0f - a);
        break;
    case Blend::Additive:
        for (int i = 0; i < 3; i++) dst[i] = std::min(1.0f, dst[i] + src[i] * a);
        dst[3] = std::min(1.0f, dst[3] + a);
        break;
    case Blend::Multiply:
        for (int i = 0; i < 3; i++) dst[i] = src[i] * dst[i] * a + dst[i] * (1.0f - a);
        dst[3] = a + dst[3] * (1.0f - a);
        break;
    case Blend::Screen:
        for (int i = 0; i < 3; i++)
            dst[i] = dst[i] + (1.0f - dst[i]) * src[i] * a;
        dst[3] = a + dst[3] * (1.0f - a);
        break;
    }
}

} // namespace detail

// One triangle. Barycentric, with a bounding box and no top-left tie-breaking:
// Spine meshes are continuous strips, so the duplicate-edge artefacts a fill rule
// prevents show up as invisible half-pixel seams rather than anything structural,
// and the cost of getting it exactly right is not worth paying in a still.
inline void drawTriangle(Target& target, const TextureView& tex,
                         const Vertex& v0, const Vertex& v1, const Vertex& v2,
                         Blend mode) {
    float minXf = std::min({v0.x, v1.x, v2.x});
    float maxXf = std::max({v0.x, v1.x, v2.x});
    float minYf = std::min({v0.y, v1.y, v2.y});
    float maxYf = std::max({v0.y, v1.y, v2.y});

    int minX = std::max(0, (int)std::floor(minXf));
    int maxX = std::min(target.width  - 1, (int)std::ceil(maxXf));
    int minY = std::max(0, (int)std::floor(minYf));
    int maxY = std::min(target.height - 1, (int)std::ceil(maxYf));
    if (minX > maxX || minY > maxY) return;

    const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
    if (std::fabs(area) < 1e-8f) return;          // degenerate
    const float inv = 1.0f / area;

    for (int y = minY; y <= maxY; y++) {
        const float py = y + 0.5f;
        for (int x = minX; x <= maxX; x++) {
            const float px = x + 0.5f;

            float w0 = ((v1.x - px) * (v2.y - py) - (v2.x - px) * (v1.y - py)) * inv;
            float w1 = ((v2.x - px) * (v0.y - py) - (v0.x - px) * (v2.y - py)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            float u = w0 * v0.u + w1 * v1.u + w2 * v2.u;
            float v = w0 * v0.v + w1 * v1.v + w2 * v2.v;

            float texel[4];
            detail::sampleBilinear(tex, u, v, texel);

            float cr = w0 * v0.r + w1 * v1.r + w2 * v2.r;
            float cg = w0 * v0.g + w1 * v1.g + w2 * v2.g;
            float cb = w0 * v0.b + w1 * v1.b + w2 * v2.b;
            float ca = w0 * v0.a + w1 * v1.a + w2 * v2.a;

            float src[4] = { texel[0] * cr, texel[1] * cg, texel[2] * cb, 0.0f };
            float alpha  = texel[3] * ca;
            if (alpha <= 0.0f) continue;

            detail::blendPixel(&target.rgba[(size_t(y) * target.width + x) * 4], src, alpha, mode);
        }
    }
}

inline void drawTriangles(Target& target, const TextureView& tex,
                          const Vertex* verts, const uint16_t* indices, size_t indexCount,
                          Blend mode) {
    for (size_t i = 0; i + 2 < indexCount; i += 3) {
        drawTriangle(target, tex, verts[indices[i]], verts[indices[i+1]], verts[indices[i+2]], mode);
    }
}

} // namespace sl
