// macos/bridge-test.cpp
//
// Spine-free pipeline verification on macOS. Proves, before any spine-cpp is
// involved, that the mechanism layer compiles and behaves here:
//   1. scenes.h  -- config parsing and scene resolve
//   2. image.h   -- the ImageIO backend (image_mac.h): decode + roundtrip a
//                   real atlas page, straight-alpha contract intact
//   3. raster.h  -- textured triangle rasterisation to a PNG
//
// Usage: bridge-test [assetsDir]   (assetsDir defaults to ../assets)

#include "../../adapter/raster.h"
#include "../../adapter/scenes.h"
#include "../../adapter/image.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::string assets = argc > 1 ? argv[1] : "../assets";
    try {
        // 1. config parsing and resolve
        auto cfg = scenes::ParseConfig("# comment\n+night_1\nnight_2\n");
        int active = 0;
        for (auto& e : cfg) if (e.active) active++;
        std::printf("config: %zu entries, %d active\n", cfg.size(), active);

        auto st = scenes::ParseSettings("force_24_hour=false\nclock_offset_y=0.25\n");
        std::printf("settings: 24h=%d offsetY=%.2f\n",
                    int(st.force24Hour), st.clockOffsetY);

        auto r = scenes::Resolve("night_3");
        std::printf("resolve night_3: ok=%d room=%s idle=%d companion=%d gain=%.2f\n",
                    int(r.ok), r.room.c_str(), r.idle, r.companion, r.gain);

        // 2. PNG decode roundtrip against a real atlas page
        const std::string png = assets + "/arona_spr.png";
        auto img = sl::loadPng(png);
        std::printf("png: %s -> %dx%d, %zu bytes\n",
                    png.c_str(), img.width, img.height, img.rgba.size());
        // straight-alpha contract: a fully-transparent pixel must have
        // zeroed colour channels (premultiplied leakage shows up here)
        const uint8_t* p = img.pixel(0, 0);
        std::printf("pixel(0,0): %u %u %u %u\n", p[0], p[1], p[2], p[3]);
        sl::savePng(img, "build/png_roundtrip.png");
        std::printf("roundtrip: wrote build/png_roundtrip.png\n");

        // 3. textured triangle through the software rasteriser
        sl::Target t;
        t.resize(640, 360);
        t.clear(0.05f, 0.05f, 0.08f, 1.0f);
        sl::Image tex;
        tex.width = 4;
        tex.height = 4;
        tex.rgba = { 255, 60, 60, 255,   60, 255, 60, 255,
                      60, 60, 255, 255,  255, 255, 60, 255 };
        sl::TextureView tv{ tex.rgba.data(), tex.width, tex.height };
        sl::Vertex verts[3] = {
            { 100, 320, 0.0f, 1.0f, 1, 1, 1, 1 },
            { 320,  40, 0.5f, 0.0f, 1, 1, 1, 1 },
            { 540, 320, 1.0f, 1.0f, 1, 1, 1, 1 },
        };
        uint16_t idx[3] = { 0, 1, 2 };
        sl::drawTriangles(t, tv, verts, idx, 3, sl::Blend::Normal);

        sl::Image out;
        out.width = t.width;
        out.height = t.height;
        out.rgba.resize(t.rgba.size());
        for (size_t i = 0; i < t.rgba.size(); i += 4) {
            out.rgba[i + 0] = uint8_t(t.rgba[i + 0] * 255.0f + 0.5f);
            out.rgba[i + 1] = uint8_t(t.rgba[i + 1] * 255.0f + 0.5f);
            out.rgba[i + 2] = uint8_t(t.rgba[i + 2] * 255.0f + 0.5f);
            out.rgba[i + 3] = 255;
        }
        sl::savePng(out, "build/bridge_triangle.png");
        std::printf("raster: wrote build/bridge_triangle.png\n");
        std::printf("OK\n");
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    return 0;
}
