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

#include "image.h"
#include "raster.h"
#include "scene.h"
#include "scenes.h"
#include "scene.h"

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

// Opacity of the characters that live INSIDE the room (the seated figure at
// her desk, the companion standing outside). On wake the foreground figure
// appears and these leave: the engine ramps this down from 1 as the
// foreground fades in, so she "walks out of the scene" instead of there
// being two of her.
float g_roomCharsAlpha = 1.0f;

// The foreground camera is stored as a height plus her centre X, never as a
// full viewport: its width must follow the aspect of the CURRENT target or
// the character is drawn with sx != sy -- measured on this display, a camera
// solved for the 16:9 scratch renders on the 1.54:1 lock screen at 0.865x
// horizontal scale, visibly squashed and pushed sideways. frameCharacter
// solved on the scratch aspect because that is all it knew; the emit re-derives.
constexpr float kCharXFrac = 0.31f;   // her centre sits left of the middle
float g_charCenterX = 0.0f;           // world x of her scan-box centre

// The wake's tint flash: the material's own effect. FINDINGS records the
// reference's keyframe #23 with the characters flashing white -- each with
// its own tint -- and the exit carrying a silhouette-cut flash, so both the
// entrance and the exit read as a white-silhouette materialise, never a
// plain crossfade. 0 = normal colours, 1 = pure white silhouette.
float g_charFlash = 0.0f;       // the foreground figure's flash
float g_roomCharsFlash = 0.0f;  // the room characters' flash as they leave

// The flash's light, matched against the in-game recording: the figure is
// swallowed by a blown-out blob of light, not outlined as a white shape --
// at the peak no component edge survives. One additive rim cannot do that;
// stacking puffed copies of the silhouette can. The core (blend 4) supplies
// the shape; each glow layer (blend 5) re-draws it scaled around its
// centroid, additively. The stack saturates the centre into the clamp --
// which is what erases the per-attachment edges -- and feathers the
// falloff outward into the recording's soft halo.
constexpr int   kGlowLayers = 3;
constexpr float kGlowScale[kGlowLayers] = {1.14f, 1.32f, 1.55f};
constexpr float kGlowAlpha[kGlowLayers] = {0.45f, 0.28f, 0.16f};

std::vector<SBBatchVertex> g_vertices;

// Protect rects (fractions of the screen): anything whose screen
// bounds intersect them is muted for the frame so the system clock
// and password field read through unoccluded. The room backdrop
// spans the screen and is muted by the same rule -- "the classroom
// background is abandoned, the desks/chairs/characters float" is
// this rule plus nothing else.
struct ProtectRects {
    bool enabled = false;
    float clock[4] = {0.24f, 0.04f, 0.52f, 0.22f};
    float pwd[4]   = {0.32f, 0.40f, 0.36f, 0.18f};
};
ProtectRects g_protect;

// Room backdrop pieces whose (lowercased) path contains any of these
// substrings are muted: the floor and its water-light effect. The walls,
// windows, sea and sky render.
static std::vector<std::string> g_mutePatterns = {"floor", "waterlight", "bg_sky", "starsource", "bg/sea", "sea"};

static std::string lowerAscii(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
static bool pathMuted(const std::string& path) {
    const std::string low = lowerAscii(path);
    for (const auto& p : g_mutePatterns)
        if (low.find(p) != std::string::npos) return true;
    return false;
}
std::vector<unsigned short> g_indices;
std::vector<SBBatch> g_batches;

// ---------------------------------------------------------------- fullscreen fx
//
// The drifting triangle particles (scenes.h's fullscreenFx, recorded in the
// reference's steady state): translucent triangle outlines scattered over the
// whole screen, falling slowly, swaying, and flickering in and out. One
// sprite texture (FX_TEX_Triangle_02_1.png -- two outline triangles, one
// half each of a 256x128 sheet) is loaded as an EXTRA page after the
// room's and the character's atlas pages; the particles are emitted as
// direct quads on that page, additively, so they only ever add light.
//
// Positions are FRACTIONS of the target, so the layer survives any screen
// size; sizes scale with height against a 1440-tall reference.
struct FxParticle {
    float x, y;      // position, fractions of the target
    float size;      // px at a 1440-tall reference
    float fall;      // y fraction per second
    float sway;      // x fraction of sway amplitude
    float phase;     // sway/flicker phase
    float flicker;   // flicker rate, rad/s
    bool  flip;      // which of the sheet's two triangles
};
std::unique_ptr<sl::Image> g_fx;
std::vector<FxParticle> g_fxParts;

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

int sb_load_fx(const char* pngPath) {
    auto img = std::make_unique<sl::Image>(sl::loadPng(pngPath));
    if (!img || img->width <= 0 || img->height <= 0) return 1;
    g_fx = std::move(img);
    if (g_fxParts.empty()) {
        std::srand(20260914);
        for (int i = 0; i < 34; ++i) {
            FxParticle p;
            p.x = (std::rand() % 1000) / 1000.0f;
            p.y = (std::rand() % 1000) / 1000.0f;
            p.size = 16.0f + (std::rand() % 1000) / 1000.0f * 44.0f;
            p.fall = 0.010f + (std::rand() % 1000) / 1000.0f * 0.022f;
            p.sway = 0.006f + (std::rand() % 1000) / 1000.0f * 0.014f;
            p.phase = (std::rand() % 628) / 100.0f;
            p.flicker = 0.8f + (std::rand() % 1000) / 1000.0f * 1.4f;
            p.flip = (std::rand() % 2) == 0;
            g_fxParts.push_back(p);
        }
    }
    return 0;
}

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
    //
    // THIS MEASUREMENT IS ONLY A FALLBACK. This skeleton's setup pose is not
    // a standing figure -- it is a flat spread (bounds y -190..207, 397 units
    // tall) while the animation's pose stands ~2130 units tall. A camera
    // solved on the setup pose magnifies the live pose ~5x, pushes the body
    // off-screen, and leaves her headband poking into a corner. The engine
    // calls sb_character_reframe after choosing the animation, which
    // re-solves on the settled animated pose.
    sl::Target scratch;
    scratch.resize(480, 270);
    SkeletonRenderer probe;
    float visibleW = 0, visibleH = 0;
    scene::Viewport vp = scene::frameCharacter(scratch, *incoming.skeleton, probe,
                                               kCharXFrac /*her centre left of centre*/,
                                               0.62f /*visibleFrac: crops low body, reads as foreground*/,
                                               0.06f /*topMargin*/,
                                               &visibleW, &visibleH);
    if (vp.width <= 0) return 2;

    g_char = std::move(incoming);
    g_charViewport = vp;
    g_charCenterX = vp.left + kCharXFrac * vp.width;
    g_charViewportValid = true;
    g_hasChar = true;
    g_charAlpha = 0.0f;
    return 0;
}

// Re-solve the foreground camera on the pose that will actually be on
// stage: settle ~2 s of mixes and physics (the same tick sequence the
// live loop uses), then measure. Called by the engine right after the
// character's animation is chosen, because the setup-pose framing of
// sb_load_character is only a fallback -- see the comment there.
int sb_character_reframe(void) {
    if (!g_hasChar) return 1;
    const float step = 1.0f / 60.0f;
    for (float t = 0.0f; t < 2.0f; t += step) {
        g_char.state->update(step);
        g_char.state->apply(*g_char.skeleton);
        g_char.skeleton->update(step);
        g_char.skeleton->updateWorldTransform(spine::Physics_Update);
    }
    sl::Target scratch;
    scratch.resize(480, 270);
    SkeletonRenderer probe;
    float visibleW = 0, visibleH = 0;
    scene::Viewport vp = scene::frameCharacter(scratch, *g_char.skeleton, probe,
                                               kCharXFrac, 0.62f, 0.06f,
                                               &visibleW, &visibleH);
    if (vp.width <= 0) return 2;
    g_charViewport = vp;
    g_charCenterX = vp.left + kCharXFrac * vp.width;
    g_charViewportValid = true;
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
    // Fullscreen fx: fall, wrap at the bottom edge, sway and flicker.
    for (auto& p : g_fxParts) {
        p.y += p.fall * step;
        if (p.y > 1.1f) {
            p.y = -0.1f;
            p.x = (std::rand() % 1000) / 1000.0f;
        }
        p.phase += p.flicker * step;
    }
}

void sb_set_character_alpha(float alpha) {
    g_charAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

void sb_set_room_chars_alpha(float alpha) {
    g_roomCharsAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

// The wake's white-silhouette flash, per figure. 1 = pure white silhouette,
// 0 = normal colours.
void sb_set_character_flash(float flash) {
    g_charFlash = flash < 0.0f ? 0.0f : (flash > 1.0f ? 1.0f : flash);
}
void sb_set_room_chars_flash(float flash) {
    g_roomCharsFlash = flash < 0.0f ? 0.0f : (flash > 1.0f ? 1.0f : flash);
}

int sb_render(int width, int height,
              const SBBatchVertex** outVertices, int* outVertexCount,
              const unsigned short** outIndices, int* outIndexCount,
              const SBBatch** outBatches, int* outBatchCount) {
    g_vertices.clear();
    g_indices.clear();
    g_batches.clear();

    //   room pass 0  the room's non-character stage (walls, sea, desks,
    //                dressing). mute_patterns and the protect rects mute
    //                here; the system UI reads through.
    //   room pass 1  the room's own characters, fading with their exit.
    //   char pass 2  the foreground figure, fading in.
    //
    // The white passes (style 1/2) re-emit the same slots through the
    // white-silhouette pipeline (blend 4: fsWhite keeps the silhouette's
    // alpha and writes premultiplied white; blend 5: the same, additive).
    // The flash is the recording's light-blob, not a white outline: the
    // core layer runs translucent and the kGlow* layers stack puffed
    // additive copies on top, so the centre clamps to pure white -- which
    // is what hides the per-attachment edges -- and only the falloff shows
    // the silhouette's broad shape. Additive white cannot occlude, it only
    // adds light.
    //
    // The white layer's alpha is the flash amount (scaled per layer), never
    // multiplied by the body's own alpha: she appears AS the light and
    // resolves into her colours.
    //
    // spine merges render commands by texture+blend+colour, so the room's
    // characters cannot be separated from the backdrop inside one command
    // list; the two passes do it by muting the other side's slots to alpha
    // 0 per pass. The characters now draw after the stage, which during the
    // exit puts the companion briefly in front of the wall -- accepted: she
    // is fading out under the glow at the same time.
    //
    // Each pass save/restores what it mutates, so the pose state survives.
    struct SavedColor { spine::Slot* slot; float a, r, g, b; };
    auto emit = [&](SbSlot& s, const scene::Viewport& vp, float alpha,
                    int pass, int style, float puffK = 1.0f) {
        // style 0 = normal colours; 1 = white core (normal blend);
        // 2 = white glow (additive, puffed by puffK around the silhouette).
        std::vector<SavedColor> saved;
        const scene::Classification cls = scene::classify(*s.skeleton);
        const bool room = &s == &g_room;

        const float sx = float(width) / vp.width;
        const float sy = float(height) / vp.height;
        const float top = vp.bottom + vp.height;
        spine::Vector<float> wv;
        for (size_t i = 0; i < s.skeleton->getSlots().size(); ++i) {
            spine::Slot* slot = s.skeleton->getSlots()[i];
            spine::Attachment* att = slot->getAttachment();
            if (!att) continue;

            float aMul = alpha;
            if (room) {
                const bool isChar = cls.isChar[i];
                if (pass == 0 && isChar) aMul = 0.0f;    // stage pass: she is off
                if (pass == 1 && !isChar) aMul = 0.0f;   // exit pass: stage is off
            }

            bool muted = room && pathMuted(lowerAscii(scene::attachmentPath(att)));
            if (!muted && room && g_protect.enabled) {
                int n = 0;
                if (auto* r = dynamic_cast<spine::RegionAttachment*>(att)) {
                    wv.setSize(8, 0); r->computeWorldVertices(*slot, wv, 0, 2); n = 4;
                } else if (auto* m = dynamic_cast<spine::MeshAttachment*>(att)) {
                    n = (int)m->getWorldVerticesLength() / 2;
                    wv.setSize(n * 2, 0); m->computeWorldVertices(*slot, 0, n * 2, wv, 0, 2);
                } else continue;
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                for (int k = 0; k < n; ++k) {
                    const float px = (wv[k * 2] - vp.left) * sx;
                    const float py = (top - wv[k * 2 + 1]) * sy;
                    minX = std::min(minX, px); maxX = std::max(maxX, px);
                    minY = std::min(minY, py); maxY = std::max(maxY, py);
                }
                auto hits = [&](const float* r) {
                    return minX < (r[0] + r[2]) * float(width) &&
                           maxX > r[0] * float(width) &&
                           minY < (r[1] + r[3]) * float(height) &&
                           maxY > r[1] * float(height);
                };
                muted = hits(g_protect.clock) || hits(g_protect.pwd);
            }
            if (muted) aMul = 0.0f;

            if (aMul != slot->getColor().a) {
                spine::Color& c = slot->getColor();
                saved.push_back({slot, c.a, c.r, c.g, c.b});
                c.a = aMul;
            }
        }

        // The glow layer puffs the silhouette around its own centre. Two
        // sweeps over the commands: the first measures the alpha-weighted
        // extent in screen pixels, the second emits puffed around it.
        const bool puff = style == 2;
        float cx = 0, cy = 0;
        std::vector<spine::RenderCommand*> cmds;
        for (spine::RenderCommand* cmd = s.renderer->render(*s.skeleton); cmd; cmd = cmd->next) {
            if (!cmd->texture || !cmd->numVertices) continue;
            if (puff) {
                float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
                for (int i = 0; i < cmd->numVertices; ++i) {
                    if (((cmd->colors[i] >> 24) & 0xFF) == 0) continue;
                    const float px = (cmd->positions[i * 2] - vp.left) * sx;
                    const float py = (top - cmd->positions[i * 2 + 1]) * sy;
                    bx0 = std::min(bx0, px); bx1 = std::max(bx1, px);
                    by0 = std::min(by0, py); by1 = std::max(by1, py);
                }
                if (bx1 > bx0 && by1 > by0) {
                    cx = (cx * 0.5f) + ((bx0 + bx1) * 0.5f) * 0.5f;
                    cy = (cy * 0.5f) + ((by0 + by1) * 0.5f) * 0.5f;
                }
            }
            cmds.push_back(cmd);
        }

        for (spine::RenderCommand* cmd : cmds) {
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
            // Blend 4 = white core, blend 5 = white glow (both fsWhite; the
            // glow reuses the additive pipeline). 0..3 the spine blend modes.
            batch.blend = style == 1 ? 4 : (style == 2 ? 5 : int(cmd->blendMode));

            g_vertices.resize(batch.vertexOffset + cmd->numVertices);
            for (int i = 0; i < cmd->numVertices; ++i) {
                SBBatchVertex& v = g_vertices[batch.vertexOffset + i];
                float px = (cmd->positions[i * 2] - vp.left) * sx;
                float py = (top - cmd->positions[i * 2 + 1]) * sy;
                if (puff) {
                    px = cx + (px - cx) * puffK;
                    py = cy + (py - cy) * puffK;
                }
                v.x = px;
                v.y = py;
                v.u = cmd->uvs[i * 2];
                v.v = cmd->uvs[i * 2 + 1];
                const uint32_t c = cmd->colors[i];   // packed ARGB
                // The pass alpha already lives in the slot colours (the
                // muting block above), and render() folded it into these
                // per-vertex colours. Multiplying by `alpha` here as well
                // would square every fade. Straight rgb too: the shader
                // premultiplies by the final fragment alpha, and doubling
                // that turns every animation-keyed slot-alpha dip -- the
                // hair ornament and clothes shimmer -- into an obvious
                // brightness flicker.
                const float va = ((c >> 24) & 0xFF) / 255.0f;
                v.r = uint8_t((c >> 16) & 0xFF);
                v.g = uint8_t((c >> 8) & 0xFF);
                v.b = uint8_t(c & 0xFF);
                v.a = uint8_t(va * 255.0f + 0.5f);
            }
            for (int i = 0; i < cmd->numIndices; ++i)
                g_indices.push_back(unsigned(cmd->indices[i]) + unsigned(batch.vertexOffset));
            g_batches.push_back(batch);
        }
        for (auto& e : saved) {
            spine::Color& c = e.slot->getColor();
            c.a = e.a; c.r = e.r; c.g = e.g; c.b = e.b;
        }
    };

    scene::Viewport rvp = scene::roomViewport(width, height);
    const float roomWhite = g_roomCharsAlpha > 0.0f ? g_roomCharsFlash : 0.0f;
    const float charWhite = (g_hasChar && g_charViewportValid && g_charAlpha > 0.0f)
        ? g_charFlash : 0.0f;
    emit(g_room, rvp, 1.0f, 0, 0);                 // the stage
    emit(g_room, rvp, g_roomCharsAlpha, 1, 0);     // her, and the companion, leaving
    if (roomWhite > 0.0f) {
        emit(g_room, rvp, roomWhite * 0.80f, 1, 1);   // white core, translucent
        for (int l = 0; l < kGlowLayers; ++l)         // stacked bloom: the light
            emit(g_room, rvp, roomWhite * kGlowAlpha[l], 1, 2, kGlowScale[l]);
    }

    if (g_hasChar && g_charViewportValid && g_charAlpha > 0.0f) {
        // Re-derive the camera for THIS target: width follows the real
        // aspect so sx == sy (uniform scale), and her centre is planted at
        // kCharXFrac of the current width. The stored viewport contributes
        // only its height and bottom.
        scene::Viewport cvp = g_charViewport;
        cvp.width = cvp.height * float(width) / float(height);
        cvp.left = g_charCenterX - kCharXFrac * cvp.width;
        emit(g_char, cvp, g_charAlpha, 2, 0);          // arriving, in colour
        if (charWhite > 0.0f) {
            emit(g_char, cvp, charWhite * 0.80f, 2, 1); // white core, translucent
            for (int l = 0; l < kGlowLayers; ++l)       // stacked bloom: the light
                emit(g_char, cvp, charWhite * kGlowAlpha[l], 2, 2, kGlowScale[l]);
        }
    }

    // Fullscreen fx: the drifting triangles, after everything else.
    if (g_fx) {
        const int fxPage = int(g_room.loader->pages.size())
            + (g_hasChar && g_char.loader ? int(g_char.loader->pages.size()) : 0);
        const float hScale = float(height) / 1440.0f;
        for (size_t i = 0; i < g_fxParts.size(); ++i) {
            const FxParticle& p = g_fxParts[i];
            const float a = 0.5f + 0.5f * std::sin(p.phase);
            if (a < 0.03f) continue;
            const float px = (p.x + p.sway * std::sin(p.phase * 0.7f)) * float(width);
            const float py = p.y * float(height);
            const float sz = p.size * hScale;
            const float u0 = p.flip ? 0.0f : 0.5f;
            const float u1 = p.flip ? 0.5f : 1.0f;
            SBBatch batch;
            batch.vertexOffset = int(g_vertices.size());
            batch.vertexCount = 4;
            batch.indexOffset = int(g_indices.size());
            batch.indexCount = 6;
            batch.page = fxPage;
            batch.blend = 1;   // additive: the outlines only ever add light
            g_vertices.resize(batch.vertexOffset + 4);
            const SBBatchVertex q[4] = {
                { px - sz, py - sz, u0, 0, 255, 255, 255, (uint8_t)(a * 255.0f + 0.5f) },
                { px + sz, py - sz, u1, 0, 255, 255, 255, (uint8_t)(a * 255.0f + 0.5f) },
                { px - sz, py + sz, u0, 1, 255, 255, 255, (uint8_t)(a * 255.0f + 0.5f) },
                { px + sz, py + sz, u1, 1, 255, 255, 255, (uint8_t)(a * 255.0f + 0.5f) },
            };
            for (int k = 0; k < 4; ++k) g_vertices[batch.vertexOffset + k] = q[k];
            const unsigned short base = (unsigned short)batch.vertexOffset;
            const unsigned short qi[6] = { base, (unsigned short)(base + 1), (unsigned short)(base + 2),
                                           (unsigned short)(base + 1), (unsigned short)(base + 3),
                                           (unsigned short)(base + 2) };
            for (int k = 0; k < 6; ++k) g_indices.push_back(qi[k]);
            g_batches.push_back(batch);
        }
    }

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
    if (g_fx) ++n;
    return n;
}

// Pages that belong to the ROOM. The sink uses the boundary to slot the
// in-scene clock between the room's batches and the foreground figure's,
// so she occludes the digits the way the reference stages it.
int sb_room_page_count(void) {
    return g_room.loader ? int(g_room.loader->pages.size()) : 0;
}

int sb_page(int index, const uint8_t** outRgba, int* outW, int* outH) {
    const sl::Image* img = nullptr;
    const int roomPages = int(g_room.loader->pages.size());
    const int charPages = (g_hasChar && g_char.loader) ? int(g_char.loader->pages.size()) : 0;
    if (index < roomPages) {
        img = g_room.loader->pages[index].get();
    } else if (g_hasChar && index < roomPages + charPages) {
        img = g_char.loader->pages[index - roomPages].get();
    } else if (g_fx && index == roomPages + charPages) {
        img = g_fx.get();
    }
    if (!img) return 1;
    *outRgba = img->rgba.data();
    *outW = img->width;
    *outH = img->height;
    return 0;
}

void sb_set_protect_rects(float clockX, float clockY, float clockW, float clockH,
                          float pwdX, float pwdY, float pwdW, float pwdH) {
    g_protect.enabled = true;
    g_protect.clock[0] = clockX; g_protect.clock[1] = clockY;
    g_protect.clock[2] = clockW; g_protect.clock[3] = clockH;
    g_protect.pwd[0] = pwdX; g_protect.pwd[1] = pwdY;
    g_protect.pwd[2] = pwdW; g_protect.pwd[3] = pwdH;
}

void sb_set_mute_patterns(const char* csv) {
    g_mutePatterns.clear();
    if (!csv) return;
    std::string item;
    for (const char* p = csv; ; ++p) {
        if (*p == ',' || *p == 0) {
            if (!item.empty()) g_mutePatterns.push_back(lowerAscii(item));
            item.clear();
            if (*p == 0) break;
        } else item.push_back(*p);
    }
}

void sb_shutdown(void) {
    g_char = SbSlot();
    g_room = SbSlot();
    g_hasChar = false;
    g_charViewportValid = false;
}
