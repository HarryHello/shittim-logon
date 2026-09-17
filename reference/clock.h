#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include "localeclock.h"

#pragma comment(lib, "dwrite.lib")

struct ClockPatch {
    float left = 580.0f, top = 150.0f, right = 1340.0f, bottom = 430.0f;
};

inline D2D1_COLOR_F ApplyLogonDim(BYTE r, BYTE g, BYTE b) {
    auto f = [](BYTE v) {
        float x = 0.8248f * v - 4.11f;
        if (x < 0) x = 0;
        if (x > 255) x = 255;
        return x / 255.0f;
    };
    return D2D1::ColorF(f(r), f(g), f(b), 1.0f);
}

inline void DrawReplacementClock(ID2D1DeviceContext *d2d,
                                 IDWriteFactory *dw,
                                 ID2D1Bitmap1 *bgBitmap,
                                 const ClockPatch &patch,
                                 const wchar_t *fontFamily,
                                 void (*log)(const wchar_t *, ...)) {
    using Microsoft::WRL::ComPtr;

    if (bgBitmap) {
        D2D1_RECT_F dst = D2D1::RectF(patch.left, patch.top, patch.right, patch.bottom);
        const float cx = (patch.left + patch.right) * 0.5f;
        const float cy = (patch.top + patch.bottom) * 0.5f;
        const float rx = (patch.right - patch.left) * 0.5f;
        const float ry = (patch.bottom - patch.top) * 0.5f;

        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[3] = {
            { 0.00f, D2D1::ColorF(D2D1::ColorF::White, 1.0f) },
            { 0.86f, D2D1::ColorF(D2D1::ColorF::White, 1.0f) },
            { 1.00f, D2D1::ColorF(D2D1::ColorF::White, 0.0f) },
        };
        ComPtr<ID2D1RadialGradientBrush> mask;
        if (SUCCEEDED(d2d->CreateGradientStopCollection(gs, 3, &stops))) {
            d2d->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy),
                                                    D2D1::Point2F(0, 0), rx, ry),
                stops.Get(), &mask);
        }

        if (mask) {
            d2d->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), 1.0f, mask.Get()),
                           nullptr);
        }

        d2d->DrawBitmap(bgBitmap, dst, 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, dst);
        if (mask) d2d->PopLayer();

        log(L"clock: patched %d,%d %dx%d from the background (feathered)",
            (int)patch.left, (int)patch.top,
            (int)(patch.right - patch.left), (int)(patch.bottom - patch.top));
    }

    wchar_t timeText[64] = L"";
    wchar_t dateText[128] = L"";
    localeclock::Format(timeText, ARRAYSIZE(timeText), dateText, ARRAYSIZE(dateText),
                         false,  false, log);

    auto draw = [&](const wchar_t *text, float size, DWRITE_FONT_WEIGHT weight,
                    float top, float height, float alpha) {
        ComPtr<IDWriteTextFormat> fmt;
        HRESULT hr = dw->CreateTextFormat(fontFamily, nullptr, weight,
                                          DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
        if (FAILED(hr)) {
            log(L"clock: CreateTextFormat('%s') failed 0x%08lX", fontFamily, (unsigned long)hr);
            return;
        }
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        D2D1_RECT_F box = D2D1::RectF(patch.left, top, patch.right, top + height);

        ComPtr<ID2D1SolidColorBrush> shadow, fill;
        d2d->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f * alpha), &shadow);
        d2d->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha), &fill);
        if (shadow) {
            D2D1_RECT_F s = box;
            s.left += 3; s.right += 3; s.top += 3; s.bottom += 3;
            d2d->DrawText(text, (UINT32)wcslen(text), fmt.Get(), s, shadow.Get());
        }
        if (fill) d2d->DrawText(text, (UINT32)wcslen(text), fmt.Get(), box, fill.Get());
    };

    draw(timeText, 130.0f, DWRITE_FONT_WEIGHT_BOLD,   patch.top + 20.0f,  150.0f, 1.0f);
    draw(dateText, 30.0f,  DWRITE_FONT_WEIGHT_NORMAL, patch.top + 175.0f, 45.0f,  0.92f);

    log(L"clock: drew '%s' / '%s' in '%s'", timeText, dateText, fontFamily);
}
