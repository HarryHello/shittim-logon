#pragma once

#include "fx_layer.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fx {

inline const float *DriftTints(int &count, bool day = false) {
    static const float kNight[3][3] = {
        { 0xEC / 255.0f, 0xC3 / 255.0f, 0xFF / 255.0f },
        { 0xFF / 255.0f, 0xC0 / 255.0f, 0xD8 / 255.0f },
        { 0xF6 / 255.0f, 0xE8 / 255.0f, 0xFF / 255.0f },
    };
    static const float kDay[3][3] = {
        { 0xD5 / 255.0f, 0xE1 / 255.0f, 0xEA / 255.0f },
        { 0xCE / 255.0f, 0xD6 / 255.0f, 0xE5 / 255.0f },
        { 0xA4 / 255.0f, 0xC1 / 255.0f, 0xD9 / 255.0f },
    };
    static_assert(sizeof(kDay) == sizeof(kNight), "both sets must stay three colours");
    count = 3;
    return day ? &kDay[0][0] : &kNight[0][0];
}

inline sl::Image MakeHalo(int size = 128, float inset = 0.46f, float softness = 0.26f) {

    sl::Image img;
    img.width = size;
    img.height = size;
    img.rgba.assign(size_t(size) * size * 4, (uint8_t)0);

    const float px[3] = {  0.0f, -0.85f, 0.85f };
    const float py[3] = { -1.0f,  0.85f, 0.85f };

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {

            const float sx = ((x + 0.5f) / size * 2.0f - 1.0f) / inset;
            const float sy = ((y + 0.5f) / size * 2.0f - 1.0f) / inset;

            bool inside = true;
            float best = 1e9f;
            for (int k = 0; k < 3; k++) {
                const int k2 = (k + 1) % 3;
                const float ex = px[k2] - px[k], ey = py[k2] - py[k];
                const float len = std::sqrt(ex * ex + ey * ey);

                const float e = ((sx - px[k]) * ey - (sy - py[k]) * ex) / len;
                if (-e > 0.0f) inside = false;

                const float ax = sx - px[k], ay = sy - py[k];
                float h = (ax * ex + ay * ey) / (ex * ex + ey * ey);
                h = h < 0.0f ? 0.0f : (h > 1.0f ? 1.0f : h);
                const float dx = ax - ex * h, dy = ay - ey * h;
                const float seg = std::sqrt(dx * dx + dy * dy);
                if (seg < best) best = seg;
            }
            const float d = inside ? 0.0f : best;
            const float a = d <= 0.0f ? 1.0f : std::exp(-d / softness);
            const uint8_t v = a < 0.0025f ? 0 : (uint8_t)(a * 255.0f + 0.5f);
            const size_t i = (size_t(y) * size + x) * 4;
            img.rgba[i + 0] = v;
            img.rgba[i + 1] = v;
            img.rgba[i + 2] = v;
            img.rgba[i + 3] = v;
        }
    }
    return img;
}

struct Drift {
    struct P {
        bool  live = false;
        float x = 0, y = 0;
        float vx = 0, vy = 0;
        float rotZ = 0, vrotZ = 0;
        float rotY = 0, vrotY = 0;
        float scale = 1.0f;
        float age = 0, life = 0;
        int   tint = 0;
    };

    int   cap         = 30;

    int   seeded      = 14;
    float spawnPerSec = 0.2f * 60.0f;
    float sizePx      = 20.0f;
    float scaleMin    = 0.4f;
    float scaleMax    = 2.4f;
    float driftPx     = 0.75f * 60.0f;
    float spinRad     = 1.0f * 60.0f * 3.14159265f / 180.0f;
    float lifeMin     = 250.0f / 60.0f;
    float lifeVar     = 150.0f / 60.0f;
    float fadeIn      = 30.0f / 60.0f;
    float peakAlpha   = 0.8f;
    float edgePx      = 100.0f;
    float marginPx    = 200.0f;

    bool  day         = false;

    float tumble      = 0.85f;

    float haloScale   = 2.2f;
    float haloAlpha   = 0.55f;

    float gain        = 1.0f;

    std::vector<P> parts;
    Rng   rng{ 0x5EED0004u };
    float spawnAcc = 0.0f;
    bool  seededYet = false;

    float alphaAt(const P &p) const {
        if (p.age < fadeIn) return (p.age / fadeIn) * peakAlpha;
        const float rest = p.life - fadeIn;
        if (rest <= 0.0f) return 0.0f;
        const float a = ((p.life - p.age) / rest) * peakAlpha;
        return a < 0.0f ? 0.0f : a;
    }

    void Spawn(int W, int H, bool insideScreen) {
        P p;
        p.live = true;
        if (insideScreen) {

            p.x = rng.next() * W;
            p.y = rng.next() * H;
            p.age = fadeIn * rng.next();
        } else {
            switch (int(rng.next() * 4.0f) & 3) {
            case 0: p.x = rng.next() * (W + edgePx * 2) - edgePx; p.y = -edgePx;      break;
            case 1: p.x = W + edgePx; p.y = rng.next() * (H + edgePx * 2) - edgePx;   break;
            case 2: p.x = rng.next() * (W + edgePx * 2) - edgePx; p.y = H + edgePx;   break;
            default: p.x = -edgePx;   p.y = rng.next() * (H + edgePx * 2) - edgePx;   break;
            }
        }
        p.vx    = (rng.next() - 0.5f) * 2.0f * driftPx;
        p.vy    = (rng.next() - 0.5f) * 2.0f * driftPx;
        p.rotZ  = rng.next() * 6.2831853f;
        p.rotY  = rng.next() * 6.2831853f;
        p.vrotZ = (rng.next() - 0.5f) * 2.0f * spinRad;
        p.vrotY = (rng.next() - 0.5f) * 2.0f * spinRad;
        p.scale = scaleMin + rng.next() * (scaleMax - scaleMin);
        p.life  = lifeMin + rng.next() * lifeVar;
        int n = 0; DriftTints(n, day);
        p.tint = int(rng.next() * float(n)) % n;
        parts.push_back(p);
    }

    void Step(float dt, int W, int H) {
        if (!seededYet) {
            seededYet = true;
            for (int i = 0; i < seeded; i++) Spawn(W, H, true);
        }

        for (size_t i = 0; i < parts.size();) {
            P &p = parts[i];
            p.x    += p.vx * dt;
            p.y    += p.vy * dt;
            p.rotZ += p.vrotZ * dt;
            p.rotY += p.vrotY * dt;
            p.age  += dt;
            const bool out = p.x < -marginPx || p.x > W + marginPx ||
                             p.y < -marginPx || p.y > H + marginPx;
            if (out || p.age >= p.life) {
                parts[i] = parts.back();
                parts.pop_back();
            } else {
                i++;
            }
        }

        spawnAcc += spawnPerSec * dt;
        while (spawnAcc >= 1.0f) {
            spawnAcc -= 1.0f;
            if ((int)parts.size() < cap) Spawn(W, H, false);
        }
    }

    float squash(const P &p) const {
        return 1.0f - tumble * (1.0f - std::fabs(std::cos(p.rotY)));
    }

    struct Body {
        float x[3], y[3];
        float r, g, b, a;
    };
    struct Halo {
        float cx, cy, halfX, halfY, cos, sin;
        float r, g, b, a;
    };

    void Shapes(std::vector<Halo> &halos, std::vector<Body> &bodies) const {
        int n = 0;

        const float *tints = DriftTints(n, day);

        static const float kPx[3] = {   0.0f, -17.0f, 17.0f };
        static const float kPy[3] = { -20.0f,  17.0f, 17.0f };

        for (const P &p : parts) {
            const float a = alphaAt(p) * gain;
            if (a <= 0.002f) continue;
            const float sq = squash(p);
            const float cs = std::cos(p.rotZ), sn = std::sin(p.rotZ);
            const float *c = tints + p.tint * 3;

            Halo h;
            h.cx = p.x; h.cy = p.y;
            h.halfX = 20.0f * p.scale * sq * haloScale;
            h.halfY = 20.0f * p.scale * haloScale;
            h.cos = cs; h.sin = sn;
            h.r = c[0]; h.g = c[1]; h.b = c[2];
            h.a = a * haloAlpha;
            halos.push_back(h);

            Body b;
            for (int k = 0; k < 3; k++) {
                const float lx = kPx[k] * p.scale * sq;
                const float ly = kPy[k] * p.scale;
                b.x[k] = p.x + lx * cs - ly * sn;
                b.y[k] = p.y + lx * sn + ly * cs;
            }
            b.r = c[0]; b.g = c[1]; b.b = c[2];
            b.a = a;
            bodies.push_back(b);
        }
    }

    void Composite(sl::Target &dst, const sl::TextureView &halo) const {
        std::vector<Halo> hs; std::vector<Body> bs;
        Shapes(hs, bs);
        const int W = dst.width, H = dst.height;

        auto over = [&](int x, int y, const float rgb[3], float a) {
            if (a <= 0.0f) return;
            if (a > 1.0f) a = 1.0f;
            const size_t i = (size_t(y) * W + x) * 4;
            for (int k = 0; k < 3; k++)
                dst.rgba[i + k] = rgb[k] * a + dst.rgba[i + k] * (1.0f - a);
            dst.rgba[i + 3] = a + dst.rgba[i + 3] * (1.0f - a);
        };

        for (size_t idx = 0; idx < bs.size(); idx++) {

            if (halo.rgba) {
                const Halo &q = hs[idx];
                const float rgb[3] = { q.r, q.g, q.b };
                const float rad = std::sqrt(q.halfX * q.halfX + q.halfY * q.halfY);
                const int x0 = (std::max)(0, (int)std::floor(q.cx - rad));
                const int x1 = (std::min)(W - 1, (int)std::ceil(q.cx + rad));
                const int y0 = (std::max)(0, (int)std::floor(q.cy - rad));
                const int y1 = (std::min)(H - 1, (int)std::ceil(q.cy + rad));
                for (int y = y0; y <= y1; y++) {
                    for (int x = x0; x <= x1; x++) {
                        const float dx = x + 0.5f - q.cx, dy = y + 0.5f - q.cy;
                        const float lx =  dx * q.cos + dy * q.sin;
                        const float ly = -dx * q.sin + dy * q.cos;
                        if (lx < -q.halfX || lx > q.halfX || ly < -q.halfY || ly > q.halfY) continue;
                        float texel[4];
                        sl::detail::sampleBilinear(halo, lx / (q.halfX * 2.0f) + 0.5f,
                                                   ly / (q.halfY * 2.0f) + 0.5f, texel);

                        const float lum = (texel[0] + texel[1] + texel[2]) * (1.0f / 3.0f);
                        over(x, y, rgb, lum * q.a);
                    }
                }
            }

            const Body &b = bs[idx];
            const float rgb[3] = { b.r, b.g, b.b };
            float bx0 = b.x[0], bx1 = b.x[0], by0 = b.y[0], by1 = b.y[0];
            for (int k = 1; k < 3; k++) {
                bx0 = (std::min)(bx0, b.x[k]); bx1 = (std::max)(bx1, b.x[k]);
                by0 = (std::min)(by0, b.y[k]); by1 = (std::max)(by1, b.y[k]);
            }
            const int px0 = (std::max)(0, (int)std::floor(bx0) - 1);
            const int px1 = (std::min)(W - 1, (int)std::ceil(bx1) + 1);
            const int py0 = (std::max)(0, (int)std::floor(by0) - 1);
            const int py1 = (std::min)(H - 1, (int)std::ceil(by1) + 1);

            const float area = (b.x[1] - b.x[0]) * (b.y[2] - b.y[0]) -
                               (b.x[2] - b.x[0]) * (b.y[1] - b.y[0]);
            const float sgn = area < 0.0f ? 1.0f : -1.0f;

            for (int y = py0; y <= py1; y++) {
                for (int x = px0; x <= px1; x++) {
                    const float sx = x + 0.5f, sy = y + 0.5f;
                    float cover = 1.0f;
                    for (int k = 0; k < 3 && cover > 0.0f; k++) {
                        const int k2 = (k + 1) % 3;
                        const float ex = b.x[k2] - b.x[k], ey = b.y[k2] - b.y[k];
                        const float len = std::sqrt(ex * ex + ey * ey);
                        if (len <= 0.0f) { cover = 0.0f; break; }
                        const float d = sgn * ((sx - b.x[k]) * ey - (sy - b.y[k]) * ex) / len;
                        const float e = d + 0.5f;
                        cover = (std::min)(cover, e <= 0.0f ? 0.0f : (e >= 1.0f ? 1.0f : e));
                    }
                    if (cover > 0.0f) over(x, y, rgb, cover * b.a);
                }
            }
        }
    }
};

}
