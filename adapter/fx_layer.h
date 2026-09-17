#pragma once

#include "raster.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fx {

struct Key {
    float t;
    float rgb[3];
    float a;
};

inline const Key *PlanaTint(int &count) {
    static const Key k[] = {
        {0.00f, {1.000f, 0.685f, 0.867f}, 1.0f},
        {0.27f, {1.000f, 0.721f, 0.908f}, 1.0f},
        {0.40f, {1.000f, 0.634f, 0.930f}, 1.0f},
        {0.53f, {1.000f, 0.412f, 0.942f}, 1.0f},
        {0.67f, {0.985f, 0.282f, 1.000f}, 1.0f},
        {1.00f, {0.985f, 0.282f, 1.000f}, 1.0f},
    };
    count = 6;
    return k;
}

inline const Key *AronaTint(int &count) {
    static const Key k[] = {
        {0.000f, {1.000f, 1.000f, 1.000f}, 1.0f},
        {0.356f, {1.000f, 1.000f, 1.000f}, 1.0f},
        {0.609f, {0.297f, 0.653f, 1.000f}, 1.0f},
        {1.000f, {0.297f, 0.653f, 1.000f}, 1.0f},
    };
    count = 4;
    return k;
}

inline const Key *TintFor(const std::string &skeleton, int &count) {
    return skeleton == "NP0035_spr" ? PlanaTint(count) : AronaTint(count);
}

inline const Key *TriKeys(int &count) {
    static const Key k[] = {
        {0.000f, {1.000f, 1.000f, 1.000f}, 1.0f},
        {0.182f, {1.000f, 1.000f, 1.000f}, 1.0f},
        {0.282f, {0.373f, 0.773f, 1.000f}, 0.0f},
        {0.462f, {0.373f, 0.773f, 1.000f}, 1.0f},
        {0.662f, {0.353f, 0.729f, 0.945f}, 0.0f},
        {0.826f, {0.373f, 0.773f, 1.000f}, 1.0f},
        {1.000f, {0.373f, 0.773f, 1.000f}, 1.0f},
    };
    count = 7;
    return k;
}

constexpr float kFlashLife = 0.50f;

constexpr float kExitToEnterLag = 0.400f;

inline void sampleKeys(const Key *keys, int n, float u, float rgb[3], float *a) {
    u = std::min(1.0f, std::max(0.0f, u));
    for (int i = 0; i < n - 1; i++) {
        const Key &p = keys[i], &q = keys[i + 1];
        if (u >= p.t && u <= q.t) {
            float f = (q.t == p.t) ? 0.0f : (u - p.t) / (q.t - p.t);
            for (int c = 0; c < 3; c++) rgb[c] = p.rgb[c] + (q.rgb[c] - p.rgb[c]) * f;
            if (a) *a = p.a + (q.a - p.a) * f;
            return;
        }
    }
    const Key &l = keys[n - 1];
    for (int c = 0; c < 3; c++) rgb[c] = l.rgb[c];
    if (a) *a = l.a;
}

inline float magnitude(float u) {
    return u <= 0.40f ? 1.0f : std::max(0.0f, 1.0f - (u - 0.40f) / 0.60f);
}

inline float sizeOverLife(float u) {
    return u <= 0.154f ? 1.0f : std::max(0.0f, 1.0f - (u - 0.154f) / 0.846f);
}

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed) {}
    float next() {
        s = s + 0x6D2B79F5u;
        uint32_t t = s;
        t = (t ^ (t >> 15)) * (1u | t);
        t = t + ((t ^ (t >> 7)) * (61u | t)) ^ t;
        return float((t ^ (t >> 14)) >> 0) / 4294967296.0f;
    }
};

struct Silhouette {
    int width = 0, height = 0;
    std::vector<float> alpha;
    std::vector<float> px, py;
    float ref = 0.0f;

    bool empty() const { return px.empty(); }

    void captureFrom(const sl::Target &t) {
        width = t.width; height = t.height;
        alpha.resize(size_t(width) * height);
        for (size_t i = 0, n = alpha.size(); i < n; i++) alpha[i] = t.rgba[i * 4 + 3];

        px.clear(); py.clear();
        const int sw = 110;
        const int sh = std::max(1, (int)std::lround(110.0 * height / std::max(1, width)));
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (int y = 0; y < sh; y++) {
            for (int x = 0; x < sw; x++) {

                int sx = std::min(width  - 1, (int)((x + 0.5f) / sw * width));
                int sy = std::min(height - 1, (int)((y + 0.5f) / sh * height));
                if (alpha[size_t(sy) * width + sx] <= 90.0f / 255.0f) continue;
                float nx = float(x) / sw, ny = float(y) / sh;
                px.push_back(nx); py.push_back(ny);
                minX = std::min(minX, nx); maxX = std::max(maxX, nx);
                minY = std::min(minY, ny); maxY = std::max(maxY, ny);
            }
        }
        if (px.empty()) { ref = 0.0f; return; }

        float bw = (maxX - minX) * width;
        float bh = (maxY - minY) * height;
        ref = std::max(bw, bh * 0.45f);
    }
};

struct Flash {
    float life     = kFlashLife;
    float gain     = 2.40f;
    float sizeMul  = 1.60f;
    float triAlpha = 0.25f;
    int   count    = 15;

    const Key *tint = nullptr;
    int tintCount = 0;

    bool  active = false;
    float t = 0.0f;

    Silhouette sil;

    struct Particle {
        float life, s0, rot, px, py;
        int tile;
    };
    std::vector<Particle> parts;

    Flash() { tint = PlanaTint(tintCount); }

    void Begin(const Silhouette &s, uint32_t seed) {
        sil = s;
        active = true;
        t = 0.0f;
        parts.clear();
        if (sil.empty()) return;

        float nearness  = std::min(1.0f, sil.ref / (sil.width * 0.16f));
        int   n         = std::max(4, (int)std::lround(count * nearness));
        float distScale = 0.40f + 0.60f * nearness;

        Rng rnd(seed);
        parts.reserve(n);
        for (int i = 0; i < n; i++) {
            Particle p;
            p.life = 0.5f + rnd.next() * 0.3f;
            p.tile = rnd.next() < 0.5f ? 0 : 1;

            float r1 = rnd.next(), r2 = rnd.next();
            p.s0  = (0.05f + r1 * r2 * 0.32f) * sil.ref * sizeMul * distScale;
            p.rot = rnd.next() * 6.2831853f;
            size_t k = (size_t)(rnd.next() * sil.px.size());
            if (k >= sil.px.size()) k = sil.px.size() - 1;
            p.px = sil.px[k]; p.py = sil.py[k];
            parts.push_back(p);
        }
    }

    void Step(float dt) {
        if (!active) return;
        t += dt;

        if (t > std::max(life, 0.85f)) active = false;
    }

    void Accumulate(std::vector<float> &add, int W, int H, const sl::TextureView &triTex) const {
        if (!active || sil.empty()) return;
        AccumulateGlow(add, W, H);
        AccumulateTriangles(add, W, H, triTex);
    }

    void AccumulateGlow(std::vector<float> &add, int W, int H) const {
        float u = t / life;
        float m = magnitude(u);
        if (m <= 0.004f) return;
        float c[3];
        sampleKeys(tint, tintCount, u, c, nullptr);

        float g[3];
        for (int i = 0; i < 3; i++) g[i] = c[i] * gain * m;

        if (sil.width != W || sil.height != H) return;
        for (size_t i = 0, n = size_t(W) * H; i < n; i++) {
            float a = sil.alpha[i];
            if (a <= 0.0f) continue;
            add[i * 3 + 0] += g[0] * a;
            add[i * 3 + 1] += g[1] * a;
            add[i * 3 + 2] += g[2] * a;
        }
    }

    void AccumulateTriangles(std::vector<float> &add, int W, int H,
                             const sl::TextureView &triTex) const {
        if (!triTex.rgba) return;
        int triKeyCount = 0;
        const Key *tk = TriKeys(triKeyCount);

        const float tileW = 0.5f;

        for (const Particle &p : parts) {
            if (t < 0.0f || t > p.life) continue;
            float u2 = t / p.life;
            float c[3], a;
            sampleKeys(tk, triKeyCount, u2, c, &a);
            float sz = p.s0 * sizeOverLife(u2);
            if (sz < 1.0f || a < 0.01f) continue;

            float draw = a * triAlpha;
            float col[3];
            for (int i = 0; i < 3; i++) col[i] = c[i] * 2.2f;

            float half = sz * 0.5f;
            float cx = p.px * W, cy = p.py * H;
            float cs = std::cos(p.rot), sn = std::sin(p.rot);

            float r = half * 1.4143f;
            int x0 = std::max(0, (int)std::floor(cx - r));
            int x1 = std::min(W - 1, (int)std::ceil(cx + r));
            int y0 = std::max(0, (int)std::floor(cy - r));
            int y1 = std::min(H - 1, (int)std::ceil(cy + r));

            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                    float lx =  dx * cs + dy * sn;
                    float ly = -dx * sn + dy * cs;
                    if (lx < -half || lx > half || ly < -half || ly > half) continue;
                    float uu = (lx / (half * 2.0f)) + 0.5f;
                    float vv = (ly / (half * 2.0f)) + 0.5f;
                    float texel[4];
                    sl::detail::sampleBilinear(triTex, (p.tile + uu) * tileW, vv, texel);
                    float w = texel[3] * draw;
                    if (w <= 0.0f) continue;
                    size_t i = (size_t(y) * W + x) * 3;
                    add[i + 0] += col[0] * w;
                    add[i + 1] += col[1] * w;
                    add[i + 2] += col[2] * w;
                }
            }
        }
    }
};

namespace detail {

inline void downsampleBox(const float *src, int sw, int sh, float *dst, int dw, int dh,
                          int factor, float threshold = 0.0f) {
    const float inv = 1.0f / float(factor * factor);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            float acc[3] = { 0, 0, 0 };
            for (int j = 0; j < factor; j++) {
                const int sy = std::min(sh - 1, y * factor + j);
                for (int i = 0; i < factor; i++) {
                    const int sx = std::min(sw - 1, x * factor + i);
                    const float *p = src + (size_t(sy) * sw + sx) * 3;
                    for (int c = 0; c < 3; c++) {
                        float v = p[c] - threshold;
                        acc[c] += v > 0.0f ? v : 0.0f;
                    }
                }
            }
            for (int c = 0; c < 3; c++) dst[(size_t(y) * dw + x) * 3 + c] = acc[c] * inv;
        }
    }
}

inline void blur5(std::vector<float> &buf, int w, int h, std::vector<float> &scratch) {
    static const float k[5] = { 1 / 16.0f, 4 / 16.0f, 6 / 16.0f, 4 / 16.0f, 1 / 16.0f };

    if (scratch.size() < buf.size()) scratch.resize(buf.size());
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                float s = 0;
                for (int t = -2; t <= 2; t++) {
                    int xx = std::min(w - 1, std::max(0, x + t));
                    s += buf[(size_t(y) * w + xx) * 3 + c] * k[t + 2];
                }
                scratch[(size_t(y) * w + x) * 3 + c] = s;
            }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                float s = 0;
                for (int t = -2; t <= 2; t++) {
                    int yy = std::min(h - 1, std::max(0, y + t));
                    s += scratch[(size_t(yy) * w + x) * 3 + c] * k[t + 2];
                }
                buf[(size_t(y) * w + x) * 3 + c] = s;
            }
}

inline void upsampleAdd(const std::vector<float> &src, int sw, int sh,
                        std::vector<float> &dst, int dw, int dh, float weight) {
    for (int y = 0; y < dh; y++) {
        float fy = (y + 0.5f) * sh / dh - 0.5f;
        int y0 = (int)std::floor(fy); float ay = fy - y0;
        int y1 = std::min(sh - 1, std::max(0, y0 + 1)); y0 = std::min(sh - 1, std::max(0, y0));
        for (int x = 0; x < dw; x++) {
            float fx = (x + 0.5f) * sw / dw - 0.5f;
            int x0 = (int)std::floor(fx); float ax = fx - x0;
            int x1 = std::min(sw - 1, std::max(0, x0 + 1)); x0 = std::min(sw - 1, std::max(0, x0));
            for (int c = 0; c < 3; c++) {
                float t = src[(size_t(y0) * sw + x0) * 3 + c] * (1 - ax) + src[(size_t(y0) * sw + x1) * 3 + c] * ax;
                float b = src[(size_t(y1) * sw + x0) * 3 + c] * (1 - ax) + src[(size_t(y1) * sw + x1) * 3 + c] * ax;
                dst[(size_t(y) * dw + x) * 3 + c] += (t + (b - t) * ay) * weight;
            }
        }
    }
}

}

struct Bloom {
    float strength  = 1.0f;

    float threshold = 0.75f;
    int   levels    = 4;

    int   baseFactor = 4;

    std::vector<float> scratch;
    std::vector<std::vector<float>> mip;
    std::vector<int> mipW, mipH;
    int builtW = 0, builtH = 0, builtLevels = 0;

    void Apply(std::vector<float> &add, int W, int H) {
        if (strength <= 0.0f || levels < 1) return;

        if (builtW != W || builtH != H || builtLevels != levels) {
            mip.clear(); mipW.clear(); mipH.clear();
            int sw = W / baseFactor, sh = H / baseFactor;
            for (int l = 0; l < levels; l++) {
                mip.emplace_back(size_t(sw) * sh * 3, 0.0f);
                mipW.push_back(sw); mipH.push_back(sh);
                if (sw <= 8 || sh <= 8) break;
                sw = std::max(1, sw / 2); sh = std::max(1, sh / 2);
            }
            builtW = W; builtH = H; builtLevels = levels;
        }
        if (mip.empty()) return;

        const float *src = add.data();
        int sw = W, sh = H;
        for (size_t l = 0; l < mip.size(); l++) {
            detail::downsampleBox(src, sw, sh, mip[l].data(), mipW[l], mipH[l],
                                  l == 0 ? baseFactor : 2, l == 0 ? threshold : 0.0f);
            detail::blur5(mip[l], mipW[l], mipH[l], scratch);
            src = mip[l].data(); sw = mipW[l]; sh = mipH[l];
        }

        for (size_t l = mip.size() - 1; l > 0; l--)
            detail::upsampleAdd(mip[l], mipW[l], mipH[l],
                                mip[l - 1], mipW[l - 1], mipH[l - 1], 1.0f);
        detail::upsampleAdd(mip[0], mipW[0], mipH[0], add, W, H,
                            strength / float(mip.size()));
    }
};

enum class Phase {
    Idle,
    Reacting,
    Exiting,
    Entering,
    Present,
};

inline const char *PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle:     return "idle";
    case Phase::Reacting: return "reacting";
    case Phase::Exiting:  return "exiting";
    case Phase::Entering: return "entering";
    case Phase::Present:  return "present";
    }
    return "?";
}

constexpr float kReferenceHideAfterExit = 0.060f;

}
