#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace subtitle {

inline constexpr float kReferenceHeight = 1051.0f;

inline constexpr float kPanelWidthFrac  = 530.0f / kReferenceHeight;

inline constexpr float kPanelHeightFrac = 112.0f / kReferenceHeight;
inline constexpr float kMarginXFrac     =  18.0f / kReferenceHeight;
inline constexpr float kMarginYFrac     =  14.0f / kReferenceHeight;
inline constexpr float kFontFrac        =  24.0f / kReferenceHeight;
inline constexpr float kFontMinFrac     =  16.0f / kReferenceHeight;

inline constexpr float kTopMarginFrac   =  24.0f / kReferenceHeight;

inline constexpr float kLineHeightRatio = 1.25f;
inline constexpr float kCharsPerSecond  = 34.0f;

inline constexpr float kMinHold         = 0.40f;

inline constexpr float kFadeSeconds     = 0.12f;

inline constexpr float kScrimAlpha = 0.55f;

inline constexpr float kTextR = 0.98f, kTextG = 0.96f, kTextB = 1.00f, kTextA = 0.96f;

struct Rgba {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;

    bool invisible() const { return a <= 0.004f; }
};

inline std::string ToText(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", (double)v);
    std::string s = buf;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.push_back('0');
    return s;
}

inline std::string ToHex(const Rgba &c) {
    static const char *d = "0123456789ABCDEF";
    const float v[4] = { c.r, c.g, c.b, c.a };
    std::string out = "#";
    for (int i = 0; i < 4; i++) {
        float f = v[i];
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        const int n = (int)std::lround(f * 255.0f);
        out.push_back(d[(n >> 4) & 0xF]);
        out.push_back(d[n & 0xF]);
    }
    return out;
}

inline constexpr float kScaleMin = 0.75f;
inline constexpr float kScaleMax = 1.25f;

enum class Language { Chinese, Japanese };

inline const char *LanguageKey(Language lang) {
    return lang == Language::Japanese ? "ja" : "zh";
}

inline const wchar_t *LocaleName(Language lang) {
    return lang == Language::Japanese ? L"ja-jp" : L"";
}

inline constexpr float kScaleMaxJapanese = 1.05f;

inline float ScaleMax(Language lang) {
    return lang == Language::Japanese ? kScaleMaxJapanese : kScaleMax;
}

inline constexpr const char *kFontFactory = "factory";
inline constexpr const char *kFontSystem  = "system";
inline constexpr const char *kFontCustom  = "custom";

struct Style {
    Rgba  background{ 0.0f, 0.0f, 0.0f, kScrimAlpha };

    Rgba  border{ 0.0f, 0.0f, 0.0f, 0.0f };
    Rgba  text{ kTextR, kTextG, kTextB, kTextA };

    float borderWidth = 1.0f;
    float corner = 0.0f;
    float scale = 1.0f;

    std::string font = kFontFactory;
};

inline float ScaleFor(const Style &s, Language lang) {
    const float hi = ScaleMax(lang);
    return s.scale > hi ? hi : s.scale;
}

inline bool FontIsFile(const std::string &want) {
    static const char *kExt[] = { ".ttf", ".otf", ".ttc", ".otc" };
    for (const char *e : kExt) {
        const size_t n = std::char_traits<char>::length(e);
        if (want.size() < n) continue;
        bool same = true;
        for (size_t i = 0; i < n && same; i++) {
            const char a = want[want.size() - n + i];
            const char lower = (a >= 'A' && a <= 'Z') ? (char)(a - 'A' + 'a') : a;
            same = lower == e[i];
        }
        if (same) return true;
    }
    return false;
}

enum class FontSource { Factory, System, Custom, Family, File };

inline FontSource FontSourceOf(const std::string &want) {
    std::string s;
    for (char c : want) s.push_back((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);

    if (s.empty() || s == kFontFactory || s == "vendor" || s == "default")
        return FontSource::Factory;
    if (s == kFontSystem) return FontSource::System;
    if (s == kFontCustom || s == "own" || s == "user") return FontSource::Custom;
    return FontIsFile(want) ? FontSource::File : FontSource::Family;
}

inline constexpr float kCutGapFrac = kMarginXFrac;

inline constexpr float kAnchorDownFrac = 0.55f;

struct Box {
    int left = 0, top = 0, right = 0, bottom = 0;
    int  w() const { return right - left; }
    int  h() const { return bottom - top; }
    bool empty() const { return w() <= 0 || h() <= 0; }
    bool intersects(const Box &o) const {
        return left < o.right && o.left < right && top < o.bottom && o.top < bottom;
    }
};

using Measure = int (*)(const char *text, float fontPx, float maxWidthPx, void *ctx);

inline float CodepointWidth(uint32_t cp, float fontPx) {
    if (cp <= 32) return fontPx * 0.35f;
    if ((cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFFEF))
        return fontPx;
    if (cp >= 33 && cp <= 126) return fontPx * 0.58f;
    return fontPx * 0.9f;
}

inline std::vector<uint32_t> Decode(const char *text) {
    std::vector<uint32_t> out;
    if (!text) return out;
    for (const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t cp = *p;
        int extra = 0;
        if (cp >= 0xF0)      { cp &= 0x07u; extra = 3; }
        else if (cp >= 0xE0) { cp &= 0x0Fu; extra = 2; }
        else if (cp >= 0xC0) { cp &= 0x1Fu; extra = 1; }
        p++;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++, p++)
            cp = (cp << 6) | (uint32_t)(*p & 0x3F);
        out.push_back(cp);
    }
    return out;
}

inline int Estimate(const char *text, float fontPx, float maxWidthPx, void * = nullptr) {
    int lines = 1;
    float width = 0.0f;
    for (uint32_t cp : Decode(text)) {
        if (cp == 10) { lines++; width = 0.0f; continue; }
        if (cp == 13) continue;
        const float cw = CodepointWidth(cp, fontPx);
        if (width > 0.0f && width + cw > maxWidthPx) { lines++; width = cw; }
        else width += cw;
    }
    return lines;
}

inline int Length(const char *text) { return (int)Decode(text).size(); }

struct Solved {
    float fontPx    = 0.0f;
    int   lines     = 1;
    int   panelW    = 0;
    int   panelH    = 0;

    bool  clipped   = false;
    bool  topAlign  = false;
};

inline Solved Solve(const char *text, int stageH,
                    Measure measure = &Estimate, void *ctx = nullptr,
                    float scale = 1.0f) {
    Solved s;
    const float H = (float)stageH;
    s.panelW = (int)std::lround(kPanelWidthFrac * H);
    const int baseH   = (int)std::lround(kPanelHeightFrac * H);
    const int marginY = (int)std::lround(kMarginYFrac * H);
    const int marginX = (int)std::lround(kMarginXFrac * H);
    const int maxH    = (std::max)(baseH, stageH - (int)std::lround(kTopMarginFrac * H));
    const float maxW  = (float)(s.panelW - marginX * 2);

    const float sc = (scale > 0.0f && scale == scale) ? scale : 1.0f;
    const float top = kFontFrac * H * sc, bottom = kFontMinFrac * H * sc;
    s.panelH = (int)std::ceil(bottom * kLineHeightRatio) + marginY * 2;
    s.fontPx = bottom;
    s.lines  = 1;

    for (int px = (int)std::lround(top); px >= (int)std::lround(bottom); px--) {
        const int lines = (std::max)(1, measure(text, (float)px, maxW, ctx));
        const int need = (int)std::ceil((float)lines * (float)px * kLineHeightRatio) + marginY * 2;
        const bool last = px == (int)std::lround(bottom);
        if (need <= maxH || last) {
            s.fontPx  = (float)px;
            s.lines   = lines;
            s.panelH  = (std::min)(need, maxH);
            s.clipped = need > maxH;
            break;
        }
    }
    s.topAlign = s.clipped;
    return s;
}

struct Placement {
    Box  panel;
    bool visible = false;

    bool movedForCut = false;

    bool movedForFace = false;
};

inline Placement Place(const Solved &s, int anchorX, int anchorY, const Box &stage,
                       const Box *cuts, int cutCount, const Box *face = nullptr) {
    Placement p;
    const int H   = stage.h();
    const int gap = (int)std::lround(kCutGapFrac * (float)H);
    const int edge = (int)std::lround(kMarginXFrac * (float)H);

    Box b;
    b.left   = anchorX - s.panelW / 2;
    b.right  = b.left + s.panelW;
    b.top    = anchorY - s.panelH / 2;
    b.bottom = b.top + s.panelH;

    if (face && !face->empty() && b.intersects(*face)) {
        const int d = (face->bottom + gap) - b.top;
        if (d > 0) { b.top += d; b.bottom += d; p.movedForFace = true; }
    }

    auto clampToStage = [&](Box &r) {
        if (r.right > stage.right - edge) { const int d = r.right - (stage.right - edge); r.left -= d; r.right -= d; }
        if (r.left < stage.left + edge)   { const int d = (stage.left + edge) - r.left;   r.left += d; r.right += d; }
        if (r.bottom > stage.bottom - edge) { const int d = r.bottom - (stage.bottom - edge); r.top -= d; r.bottom -= d; }
        if (r.top < stage.top + edge)     { const int d = (stage.top + edge) - r.top;     r.top += d; r.bottom += d; }
    };
    auto hits = [&](const Box &r) {
        for (int i = 0; i < cutCount; i++) if (r.intersects(cuts[i])) return i;
        return -1;
    };

    clampToStage(b);
    if (hits(b) < 0) { p.panel = b; p.visible = true; return p; }

    Box side = b;
    for (int pass = 0; pass <= cutCount; pass++) {
        const int i = hits(side);
        if (i < 0) break;
        const int shift = side.right - (cuts[i].left - gap);
        side.left -= shift;
        side.right -= shift;
        if (side.left < stage.left + edge) break;
    }
    if (side.left >= stage.left + edge && hits(side) < 0) {
        p.panel = side; p.visible = true; p.movedForCut = true; return p;
    }

    Box up = b;
    for (int pass = 0; pass <= cutCount; pass++) {
        const int i = hits(up);
        if (i < 0) break;
        const int shift = up.bottom - (cuts[i].top - gap);
        up.top -= shift;
        up.bottom -= shift;
        if (up.top < stage.top + edge) break;
    }
    if (up.top >= stage.top + edge && hits(up) < 0) {
        p.panel = up; p.visible = true; p.movedForCut = true; return p;
    }

    return p;
}

struct Typewriter {
    int   total   = 0;
    float elapsed = 0.0f;
    float hold    = 0.0f;
    bool  done    = false;

    void Begin(const char *text, float audioSeconds) {
        total   = Length(text);
        elapsed = 0.0f;
        done    = false;

        const float typing = (float)total / kCharsPerSecond;
        hold = (std::max)(kMinHold, audioSeconds - typing);
    }

    void Step(float dt) {
        elapsed += dt;
        if (elapsed >= (float)total / kCharsPerSecond + hold) done = true;
    }

    int visible() const {
        const int n = (int)std::floor(elapsed * kCharsPerSecond);
        return n < 0 ? 0 : (n > total ? total : n);
    }

    float alpha() const {
        if (elapsed < kFadeSeconds) return elapsed / kFadeSeconds;
        if (!done) return 1.0f;
        const float since = elapsed - ((float)total / kCharsPerSecond + hold);
        const float a = 1.0f - since / kFadeSeconds;
        return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    }

    bool gone() const { return done && alpha() <= 0.0f; }

    void Cancel() {
        if (done) return;
        done = true;
        elapsed = (float)total / kCharsPerSecond + hold;
    }
};

}
