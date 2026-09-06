// shittim-logon / render / render_still.cpp
//
// Renders one frame of the Shittim Chest room to a PNG.
//
// This is the Stage 1 background: the room only. The foreground character is the top layer of the
// Stage 3 composition and does not belong in a still that Windows will draw the
// credential UI on top of.
//
// The track layout is transcribed from the asset notes, which are themselves
// which is a port of the Shittim Chest wallpaper. Every animation name used here
// appears in the verified inventory -- spine-cpp asserts on an unknown name rather
// than returning an error, and the animations are looked up before use for that
// reason.
//
// Output targets what docs/stage1-findings.md measured: native resolution, full
// frame, 1:1. Windows crops nothing and letterboxes nothing.

#include "image.h"
#include "raster.h"
#include "scene.h"
#include "scenes.h"

#include <spine/spine.h>
#include <spine/Extension.h>
#include <spine/SkeletonRenderer.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace sl;

// spine-cpp requires the host to supply its allocator.
spine::SpineExtension *spine::getDefaultExtension() {
    return new spine::DefaultSpineExtension();
}

namespace {

// Atlas pages become plain CPU images; the rasteriser samples them directly.
class WicTextureLoader : public spine::TextureLoader {
public:
    void load(spine::AtlasPage &page, const spine::String &path) override {
        auto img = std::make_unique<Image>(loadPng(path.buffer()));
        page.width  = img->width;
        page.height = img->height;
        std::printf("    page %-44s %d x %d\n",
                    page.name.buffer(), img->width, img->height);
        page.texture = img.get();
        _owned.push_back(std::move(img));
    }
    void unload(void *) override { /* freed with this loader */ }

private:
    std::vector<std::unique_ptr<Image>> _owned;
};

struct Viewport {
    // The wallpaper's camera: ortho2d(-1440, 90, 2880, 1620), Spine's own y-up world.
    float left = -1440.0f, bottom = 90.0f, width = 2880.0f, height = 1620.0f;
};

Blend toBlend(spine::BlendMode m) {
    switch (m) {
    case spine::BlendMode_Additive: return Blend::Additive;
    case spine::BlendMode_Multiply: return Blend::Multiply;
    case spine::BlendMode_Screen:   return Blend::Screen;
    default:                        return Blend::Normal;
    }
}

// Refuses rather than asserts. spine-cpp's setAnimation aborts the process on an
// unknown name, which on the real target would be a crash on the logon screen.
bool playIfPresent(spine::AnimationState &state, spine::SkeletonData &data,
                   int track, const char *name, bool loop) {
    if (!data.findAnimation(name)) {
        std::printf("    !! '%s' is not in this skeleton - skipped\n", name);
        return false;
    }
    state.setAnimation(track, name, loop);
    std::printf("    track %d  %-24s %s\n", track, name, loop ? "loop" : "once");
    return true;
}

void drawSkeleton(Target &target, spine::Skeleton &skeleton,
                  spine::SkeletonRenderer &renderer, const Viewport &vp) {
    const float sx = target.width  / vp.width;
    const float sy = target.height / vp.height;
    const float top = vp.bottom + vp.height;

    std::vector<Vertex> verts;
    size_t commands = 0, triangles = 0;

    for (spine::RenderCommand *cmd = renderer.render(skeleton); cmd; cmd = cmd->next) {
        const Image *tex = static_cast<const Image *>(cmd->texture);
        if (!tex) continue;

        verts.resize(cmd->numVertices);
        for (int i = 0; i < cmd->numVertices; i++) {
            const float wx = cmd->positions[i * 2];
            const float wy = cmd->positions[i * 2 + 1];
            // Spine is y-up, the target is y-down.
            verts[i].x = (wx - vp.left) * sx;
            verts[i].y = (top - wy) * sy;
            verts[i].u = cmd->uvs[i * 2];
            verts[i].v = cmd->uvs[i * 2 + 1];

            const uint32_t c = cmd->colors[i];      // packed ARGB
            verts[i].a = ((c >> 24) & 0xFF) / 255.0f;
            verts[i].r = ((c >> 16) & 0xFF) / 255.0f;
            verts[i].g = ((c >>  8) & 0xFF) / 255.0f;
            verts[i].b = ( c        & 0xFF) / 255.0f;
        }

        TextureView view{ tex->rgba.data(), tex->width, tex->height };
        drawTriangles(target, view, verts.data(), cmd->indices,
                      (size_t)cmd->numIndices, toBlend(cmd->blendMode));
        commands++;
        triangles += (size_t)cmd->numIndices / 3;
    }
    std::printf("    %zu batches, %zu triangles\n", commands, triangles);
}

// `gain` scales colour and leaves alpha alone. It is the scene's own gain from the
// table -- the day room is held 5% down because it reads overexposed at 1.0 -- and
// the overlay applies the identical number to its live room. If these two ever
// disagree the handover steps in brightness, which is exactly the class of bug that
// put a night classroom over every day scene.
Image toImage(const Target &t, float gain = 1.0f) {
    Image img;
    img.width = t.width;
    img.height = t.height;
    img.rgba.resize(size_t(t.width) * t.height * 4);
    for (size_t i = 0; i < img.rgba.size(); i++) {
        float v = t.rgba[i];
        if ((i & 3) != 3) v *= gain;
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        img.rgba[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
    return img;
}

const char *argValue(int argc, char **argv, const char *flag, const char *fallback) {
    for (int i = 1; i + 1 < argc; i++) if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return fallback;
}

} // namespace

int main(int argc, char **argv) {
    // --list-scenes prints the table and exits. It exists so that
    // lab\Bake-SceneStills.ps1 does not have to keep its own copy of the scene list:
    // it kept one, it went stale after the C++ table was corrected, and the stills it
    // baked were of animations the reference never plays. A PowerShell script and a
    // C++ header cannot share a constant -- but they can share a process.
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--list-scenes") != 0) continue;
        for (const scenes::Entryv &e : scenes::Table())
            std::printf("%s\n", e.name);
        return 0;
    }

    const std::string assets = argValue(argc, argv, "--assets",
                                        "./assets");
    const std::string out    = argValue(argc, argv, "--out",
                                        "./room.png");
    const int   width  = std::atoi(argValue(argc, argv, "--width",  "1920"));
    const int   height = std::atoi(argValue(argc, argv, "--height", "1080"));
    const int   idle   = std::atoi(argValue(argc, argv, "--idle",   "1"));
    const float atTime = (float)std::atof(argValue(argc, argv, "--time", "2.0"));
    // --companion 0 | 11 | 12, and "1" still means Idle_11 so old commands mean what
    // they meant. The reference plays a companion in exactly two states -- night
    // Idle_01 with one companion, and day Idle_00 with the other as either Idle_11 or Idle_12 --
    // so which one, or none, is a property of the scene rather than a switch.
    const char *companionArg = argValue(argc, argv, "--companion", "1");
    const int   companion = (std::strcmp(companionArg, "1") == 0) ? 11 : std::atoi(companionArg);
    // Off by default: this still is the layer *under* the overlay.
    const bool  character = std::strcmp(argValue(argc, argv, "--character", "0"), "1") == 0;
    const bool  probe     = std::strcmp(argValue(argc, argv, "--probe", "0"), "1") == 0;

    // --room NAME, because there are two of them now and the slot classification in
    // scene.h was written by looking at only one. The day room has fewer bones, four
    // main idles instead of five and two companions instead of one, so "the rules
    // hold" is a claim that has to be checked against it rather than inherited.
    std::string base = argValue(argc, argv, "--room", "room_night");
    int   idleN = idle;
    int   compN = companion;
    float gain  = 1.0f;

    // --scene NAME says all four of those at once, from the one table that knows how
    // they go together. Preferred over spelling out --room/--idle/--companion: the
    // combinations are not arithmetic -- the companion appears in exactly two of the
    // nine scenes -- and every caller that reconstructed them by hand has got them
    // wrong at least once. The individual flags stay for probing a combination that
    // is not a scene.
    const char *sceneArg = argValue(argc, argv, "--scene", "");
    if (sceneArg[0]) {
        const scenes::Resolved r = scenes::Resolve(sceneArg);
        if (!r.ok) {
            std::printf("FAILED: '%s' is not a scene this build has\n", sceneArg);
            return 1;
        }
        base  = r.room;
        idleN = r.idle;
        compN = r.companion;
        gain  = r.gain;
        std::printf("[0] scene %s -> room %s, Idle_%02d, companion %d, gain %.2f\n",
                    r.name.c_str(), r.room.c_str(), r.idle, r.companion, gain);
    }

    try {
        std::printf("[1] atlas\n");
        WicTextureLoader loader;
        spine::Atlas atlas((assets + "/" + base + ".atlas").c_str(), &loader);
        if (atlas.getPages().size() == 0) throw std::runtime_error("atlas has no pages");

        std::printf("[2] skeleton\n");
        spine::AtlasAttachmentLoader attachmentLoader(&atlas);
        spine::SkeletonBinary binary(&attachmentLoader);
        spine::SkeletonData *data = binary.readSkeletonDataFile((assets + "/" + base + ".skel").c_str());
        if (!data) throw std::runtime_error(std::string("skeleton: ") + binary.getError().buffer());
        std::printf("    %d bones, %d slots, %d animations\n",
                    (int)data->getBones().size(), (int)data->getSlots().size(),
                    (int)data->getAnimations().size());

        spine::Skeleton skeleton(data);
        spine::AnimationStateData stateData(data);
        spine::AnimationState state(&stateData);

        std::printf("[3] tracks\n");
        char idleName[32];
        std::snprintf(idleName, sizeof(idleName), "Idle_%02d", idleN);
        playIfPresent(state, *data, 0, "Idle_background_00", true);
        playIfPresent(state, *data, 1, idleName, true);
        if (compN) {
            char nm[24];
            std::snprintf(nm, sizeof(nm), "Idle_%02d", compN);
            if (!playIfPresent(state, *data, 4, nm, true))
                std::printf("    (no %s in this room)\n", nm);
        }

        std::printf("[4] posing at t=%.2fs\n", atTime);
        // Stepped rather than jumped: mixes and any physics settle the way they
        // would during playback, so the still matches a frame the animation
        // actually passes through.
        const float step = 1.0f / 60.0f;
        for (float t = 0.0f; t < atTime; t += step) {
            state.update(step);
            state.apply(skeleton);
            skeleton.update(step);
            skeleton.updateWorldTransform(spine::Physics_Update);
        }

        // The same camera the overlay will use at this size. Baking the still with
        // any other rule is how stage 1 and stage 3 stop lining up the moment the
        // screen is not 16:9. See scene::roomViewport.
        scene::Viewport sceneVp = scene::roomViewport(width, height);
        if (probe) {
            std::printf("[4b] slot classification\n");
            scene::describeClassification(skeleton, std::printf, &sceneVp, width, height);
        }

        std::printf("[5] rasterising %d x %d\n", width, height);
        Target target;
        target.resize(width, height);
        target.clear(0.0f, 0.0f, 0.0f, 1.0f);

        spine::SkeletonRenderer renderer;
        if (character) {
            Viewport v;
            v.left  = sceneVp.left;  v.bottom = sceneVp.bottom;
            v.width = sceneVp.width; v.height = sceneVp.height;
            drawSkeleton(target, skeleton, renderer, v);
        } else {
            // Room only. The overlay draws the character live, at this exact
            // camera, so baking her in as well means two of her on screen -- and
            // the baked one does not leave when the wake fires, which is precisely
            // what the first live capture showed: she walks off into the
            // foreground and stays sitting at her desk at the same time.
            scene::drawFiltered(target, skeleton, renderer, sceneVp, false);
        }

        std::printf("[6] writing %s\n", out.c_str());
        savePng(toImage(target, gain), out);

        delete data;
        std::printf("OK\n");
        return 0;
    } catch (const std::exception &e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
