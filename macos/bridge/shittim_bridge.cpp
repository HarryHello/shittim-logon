// macos/bridge/shittim_bridge.cpp
//
// The C ABI implementation over the mechanism layer and spine-cpp. Ported
// from adapter/render_still.cpp (the known-good worked example) with two
// changes: it renders into caller-owned batches instead of the software
// rasteriser, and it carries a second slot for the foreground character
// (phase B), framed by scene::frameCharacter.
//
// Blend indices passed through to the sink: 0 normal, 1 additive,
// 2 multiply, 3 screen (spine's own order).

#include "shittim_bridge.h"

#include "../../adapter/image.h"
#include "../../adapter/raster.h"
#include "../../adapter/scene.h"
#include "../../adapter/scenes.h"

#include <spine/spine.h>
#include <spine/SkeletonRenderer.h>

#include <memory>
#include <string>
#include <vector>

using namespace spine;

// spine-cpp requires the host to supply its allocator.
spine::SpineExtension* spine::getDefaultExtension() {
    return new spine::DefaultSpineExtension();
}

namespace {

struct SbSlot {
    std::unique_ptr<sl::Image> ownedPage;
    class Loader : public spine::TextureLoader {
    public:
        void load(spine::AtlasPage& page, const spine::String& path) override {
            auto img = std::make_unique<sl::Image>(sl::loadPng(path.buffer()));
            page.width = img->width;
            page.height = img->height;
            page.texture = img.get();
            pages.push_back(std::move(img));
        }
        void unload(void*) override {}
        std::vector<std::unique_ptr<sl::Image>> pages;
    };
    std::unique_ptr<Loader> loader;
    std::unique_ptr<Atlas> atlas;
    std::unique_ptr<AtlasAttachmentLoader> attachmentLoader;
    std::unique_ptr<SkeletonBinary> binary;
    std::unique_ptr<SkeletonData> data;
    std::unique_ptr<Skeleton> skeleton;
    std::unique_ptr<AnimationStateData> stateData;
    std::unique_ptr<AnimationState> state;
    std::unique_ptr<SkeletonRenderer> renderer;
};

SbSlot g_room;
SbSlot g_char;
bool g_hasChar = false;
float g_charAlpha = 0.0f;
scene::Viewport g_charViewport;
bool g_charViewportValid = false;

std::vector<SBBatchVertex> g_vertices;
std::vector<unsigned short> g_indices;
std::vector<SBBatch> g_batches;

bool setupSlot(SbSlot& s, const char* assets, const char* name) {
    try {
        s.loader = std::make_unique<SbSlot::Loader>();
        s.atlas = std::make_unique<Atlas>(
            (std::string(assets) + "/" + name + ".atlas").c_str(), s.loader.get());
        if (s.atlas->getPages().size() == 0) return false;
        s.attachmentLoader = std::make_unique<AtlasAttachmentLoader>(s.atlas.get());
        s.binary = std::make_unique<SkeletonBinary>(s.attachmentLoader.get());
        s.data = std::unique_ptr<SkeletonData>(s.binary->readSkeletonDataFile(
            (std::string(assets) + "/" + name + ".skel").c_str()));
        if (!s.data) return false;
        s.skeleton = std::make_unique<Skeleton>(s.data.get());
        s.stateData = std::make_unique<AnimationStateData>(s.data.get());
        s.state = std::make_unique<AnimationState>(s.stateData.get());
        s.renderer = std::make_unique<SkeletonRenderer>();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

int animationCount(const SbSlot& s) { return int(s.data->getAnimations().size()); }
const char* animationName(const SbSlot& s, int index) {
    if (index < 0 || index >= animationCount(s)) return "";
    return s.data->getAnimations()[index]->getName().buffer();
}

// spine-cpp aborts the process on an unknown animation name. On a logon
// screen that abort lands where nobody can see it, so every play goes through
// a lookup that refuses instead (adapter/render_still.cpp's rule).
int playIfPresentGuarded(SbSlot& s, int track, const char* name, int loop) {
    if (!s.data->findAnimation(name)) return 1;
    s.state->setAnimation(track, name, loop != 0);
    return 0;
}

} // namespace

// ---------------------------------------------------------------- lifecycle

int sb_load_room(const char* assetsDir, const char* roomName) {
    g_room = SbSlot();
    return setupSlot(g_room, assetsDir, roomName) ? 0 : 1;
}

int sb_load_character(const char* assetsDir, const char* characterName) {
    SbSlot incoming;
    if (!setupSlot(incoming, assetsDir, characterName)) return 1;

    // Frame the foreground camera once, at setup pose. frameCharacter cannot
    // clip by construction (it measures through the geometry box), so one
    // solve is enough; there is no iteration to diverge.
    sl::Target scratch;
    scratch.resize(480, 270);
    SkeletonRenderer probe;
    float visibleW = 0, visibleH = 0;
    scene::Viewport vp = scene::frameCharacter(scratch, *incoming.skeleton, probe,
                                               0.38f /*xFrac: character sits left of centre*/,
                                               0.62f /*visibleFrac: crops low body, reads as foreground*/,
                                               0.06f /*topMargin*/,
                                               &visibleW, &visibleH);
    if (vp.width <= 0) return 2;

    g_char = std::move(incoming);
    g_charViewport = vp;
    g_charViewportValid = true;
    g_hasChar = true;
    g_charAlpha = 0.0f;
    return 0;
}

int sb_room_play(int track, const char* animation, int loop) {
    return playIfPresentGuarded(g_room, track, animation, loop);
}

int sb_character_play(int track, const char* animation, int loop) {
    if (!g_hasChar) return 1;
    return playIfPresentGuarded(g_char, track, animation, loop);
}

int sb_room_animation_count(void) { return animationCount(g_room); }
int sb_character_animation_count(void) { return g_hasChar ? animationCount(g_char) : 0; }
const char* sb_room_animation_name(int index) { return animationName(g_room, index); }
const char* sb_character_animation_name(int index) { return animationName(g_char, index); }

// -------------------------------------------------------------------- frame

void sb_tick(float dt) {
    if (dt > 0.1f) dt = 0.1f;   // display-link spikes must not jump the pose
    const float step = dt;
    g_room.state->update(step);
    g_room.state->apply(*g_room.skeleton);
    g_room.skeleton->update(step);
    g_room.skeleton->updateWorldTransform(spine::Physics_Update);
    if (g_hasChar && g_charAlpha > 0.0f) {
        g_char.state->update(step);
        g_char.state->apply(*g_char.skeleton);
        g_char.skeleton->update(step);
        g_char.skeleton->updateWorldTransform(spine::Physics_Update);
    }
}

void sb_set_character_alpha(float alpha) {
    g_charAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

int sb_render(int width, int height,
              const SBBatchVertex** outVertices, int* outVertexCount,
              const unsigned short** outIndices, int* outIndexCount,
              const SBBatch** outBatches, int* outBatchCount) {
    g_vertices.clear();
    g_indices.clear();
    g_batches.clear();

    // The room renders in the skeleton's own draw order (scene.h: the
    // background is underneath everything and occlusion is natural in a
    // single pass when the canvas is ours). The foreground character renders
    // above it through its own camera.
    auto emit = [&](SbSlot& s, const scene::Viewport& vp, float alpha) {
        // Overlay mode (g_roomCharOnly): the room skeleton renders its
        // CHARACTER slots only -- the static room backdrop comes from the
        // wallpaper beneath, so nothing here can cover the system clock or
        // password field.
        scene::Classification cls = scene::classify(*s.skeleton);
        std::vector<std::pair<spine::Slot*, float>> passSaved;
        for (spine::RenderCommand* cmd = s.renderer->render(*s.skeleton); cmd; cmd = cmd->next) {
            if (!cmd->texture || !cmd->numVertices) continue;
            if (&s == &g_room) {
                for (size_t i = 0; i < s.skeleton->getSlots().size(); ++i) {
                    if (scene::hiddenInPass(cls, i, scene::Pass::CharOnly)) {
                        spine::Slot* slot = s.skeleton->getSlots()[i];
                        passSaved.push_back({slot, slot->getColor().a});
                        slot->getColor().a = 0;
                    }
                }
            }

            const int pageOf = [&] {
                int i = 0;
                for (const auto& p : g_room.loader->pages) {
                    if (p.get() == cmd->texture) return i;
                    ++i;
                }
                i = int(g_room.loader->pages.size());
                for (const auto& p : g_char.loader->pages) {
                    if (p.get() == cmd->texture) return i;
                    ++i;
                }
                return -1;
            }();
            if (pageOf < 0) continue;

            SBBatch batch;
            batch.vertexOffset = int(g_vertices.size());
            batch.vertexCount = cmd->numVertices;
            batch.indexOffset = int(g_indices.size());
            batch.indexCount = cmd->numIndices;
            batch.page = pageOf;
            batch.blend = int(cmd->blendMode);

            const float sx = float(width) / vp.width;
            const float sy = float(height) / vp.height;
            const float top = vp.bottom + vp.height;
            g_vertices.resize(batch.vertexOffset + cmd->numVertices);
            for (int i = 0; i < cmd->numVertices; ++i) {
                SBBatchVertex& v = g_vertices[batch.vertexOffset + i];
                v.x = (cmd->positions[i * 2] - vp.left) * sx;
                v.y = (top - cmd->positions[i * 2 + 1]) * sy;
                v.u = cmd->uvs[i * 2];
                v.v = cmd->uvs[i * 2 + 1];
                const uint32_t c = cmd->colors[i];   // packed ARGB
                const float va = ((c >> 24) & 0xFF) / 255.0f * alpha;
                v.r = uint8_t(((c >> 16) & 0xFF) * va + 0.5f);
                v.g = uint8_t(((c >> 8) & 0xFF) * va + 0.5f);
                v.b = uint8_t((c & 0xFF) * va + 0.5f);
                v.a = uint8_t(va * 255.0f + 0.5f);
            }
            for (int i = 0; i < cmd->numIndices; ++i)
                g_indices.push_back(unsigned(cmd->indices[i]) + unsigned(batch.vertexOffset));
            g_batches.push_back(batch);
        }
        for (auto& e : passSaved) e.first->getColor().a = e.second;
    };

    emit(g_room, scene::roomViewport(width, height), 1.0f);
    if (g_hasChar && g_charViewportValid && g_charAlpha > 0.0f)
        emit(g_char, g_charViewport, g_charAlpha);

    *outVertices = g_vertices.data();
    *outVertexCount = int(g_vertices.size());
    *outIndices = g_indices.data();
    *outIndexCount = int(g_indices.size());
    *outBatches = g_batches.data();
    *outBatchCount = int(g_batches.size());
    return int(g_batches.size());
}

// -------------------------------------------------------------------- pages

int sb_page_count(void) {
    // The character slot may never have been loaded (config without a
    // character): its loader is a null unique_ptr, not an empty vector.
    int n = int(g_room.loader->pages.size());
    if (g_hasChar && g_char.loader)
        n += int(g_char.loader->pages.size());
    return n;
}

int sb_page(int index, const uint8_t** outRgba, int* outW, int* outH) {
    const sl::Image* img = nullptr;
    if (index < int(g_room.loader->pages.size())) {
        img = g_room.loader->pages[index].get();
    } else if (g_hasChar && index < int(g_room.loader->pages.size() + g_char.loader->pages.size())) {
        img = g_char.loader->pages[index - int(g_room.loader->pages.size())].get();
    }
    if (!img) return 1;
    *outRgba = img->rgba.data();
    *outW = img->width;
    *outH = img->height;
    return 0;
}

void sb_shutdown(void) {
    g_char = SbSlot();
    g_room = SbSlot();
    g_hasChar = false;
    g_charViewportValid = false;
}
