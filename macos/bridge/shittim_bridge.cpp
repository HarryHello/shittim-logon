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
#include "fx_layer.h"
#include "drift.h"

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

// The wake's flash, ported from 1.3.0's fx_layer.h. There is no white
// silhouette anywhere in it: a flash is an additive glow in the figure's
// TINT colour (Plana pink->magenta, Arona white->blue -- the upstream
// PlanaTint/AronaTint keys, the recording's "each with its own tint")
// accumulated over the figure's silhouette and run through a bloom, plus
// a burst of the FX_TEX_Triangle outlines spawned from the silhouette's
// own points. The figure is drawn in full colour underneath; the glow
// decays over kFlashLife and she resolves out of her own light.
//
// The glow runs on the sink (Metal): it samples the silhouette's alpha
// and adds tint * gain * magnitude. What the bridge owns is the state --
// the three fx::Flash instances with their per-figure tints, the frozen
// batch snapshot each Begin takes (the exit dissolves a frozen image, not
// a live one), and the triangle burst geometry.
fx::Flash g_exitFlash, g_enterFlash, g_arriveFlash;
std::string g_roomName, g_charName;
int g_renderW = 0, g_renderH = 0;   // last sb_render size; Begin needs it

// The drifting particle layer (1.3.0's fx::Drift): pastel solid triangles
// with a soft procedural halo, tumbling in two axes, spawning from the
// screen edges. Starts when the foreground enters, like upstream.
fx::Drift g_drift;
bool g_driftOn = false;
sl::Image g_halo;

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
// The flash-burst sprite texture: FX_TEX_Triangle_02_1.png (two outline
// triangles, one half each of a 256x128 sheet), loaded as an EXTRA page
// after the room's and the character's atlas pages. The bursts spawn from
// the flashing figure's silhouette points, like upstream's
// Flash::AccumulateTriangles.
//
// The Drift layer's halo is generated here (fx::MakeHalo's 3-triangle
// distance field, rebuilt with white RGB so the sink can tint it) and
// registered as one more page.
std::unique_ptr<sl::Image> g_fx;

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

// One skeleton, one pass, into the given sink arrays. `split` selects which
// half of the ROOM draws -- 0 the stage (characters muted), 1 the characters
// (stage muted), 2 everything (the foreground; no split). spine merges
// render commands by texture+blend+colour, so the room's characters cannot
// be separated from the backdrop inside one command list; the passes do it
// by muting the other side's slots to alpha 0 per pass.
//
// Each call save/restores what it mutates, so the pose state survives.
// The pass alpha is applied ONCE, through the slot colours (render() folds
// it into the per-vertex colours); the vertex alpha is NOT multiplied by it
// again -- that squared every fade (the hair-ornament shimmer bug). Straight
// rgb too: the shader premultiplies by the final fragment alpha.
static void emitInto(std::vector<SBBatchVertex>& V, std::vector<unsigned short>& I,
                     std::vector<SBBatch>& B, SbSlot& s, const scene::Viewport& vp,
                     float alpha, int split, int width, int height, int passTag) {
    struct SavedColor { spine::Slot* slot; float a, r, g, b; };
    std::vector<SavedColor> saved;
    const scene::Classification cls = scene::classify(*s.skeleton);
    const bool room = (&s == &g_room);

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
            if (split == 0 && isChar) aMul = 0.0f;    // stage pass: she is off
            if (split == 1 && !isChar) aMul = 0.0f;   // exit pass: stage is off
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

    for (spine::RenderCommand* cmd = s.renderer->render(*s.skeleton); cmd; cmd = cmd->next) {
        if (!cmd->texture || !cmd->numVertices) continue;
        const int pageOf = [&] {
            int i = 0;
            for (const auto& p : g_room.loader->pages) {
                if (p.get() == cmd->texture) return i;
                ++i;
            }
            i = int(g_room.loader->pages.size());
            if (g_hasChar && g_char.loader) {
                for (const auto& p : g_char.loader->pages) {
                    if (p.get() == cmd->texture) return i;
                    ++i;
                }
            }
            return -1;
        }();
        if (pageOf < 0) continue;

        SBBatch batch;
        batch.vertexOffset = int(V.size());
        batch.vertexCount = cmd->numVertices;
        batch.indexOffset = int(I.size());
        batch.indexCount = cmd->numIndices;
        batch.page = pageOf;
        batch.blend = int(cmd->blendMode);
        batch.pass = passTag;

        V.resize(batch.vertexOffset + cmd->numVertices);
        for (int i = 0; i < cmd->numVertices; ++i) {
            SBBatchVertex& v = V[batch.vertexOffset + i];
            v.x = (cmd->positions[i * 2] - vp.left) * sx;
            v.y = (top - cmd->positions[i * 2 + 1]) * sy;
            v.u = cmd->uvs[i * 2];
            v.v = cmd->uvs[i * 2 + 1];
            const uint32_t c = cmd->colors[i];   // packed ARGB
            const float va = ((c >> 24) & 0xFF) / 255.0f;
            v.r = uint8_t((c >> 16) & 0xFF);
            v.g = uint8_t((c >> 8) & 0xFF);
            v.b = uint8_t(c & 0xFF);
            v.a = uint8_t(va * 255.0f + 0.5f);
        }
        for (int i = 0; i < cmd->numIndices; ++i)
            I.push_back(unsigned(cmd->indices[i]) + unsigned(batch.vertexOffset));
        B.push_back(batch);
    }
    for (auto& e : saved) {
        spine::Color& c = e.slot->getColor();
        c.a = e.a; c.r = e.r; c.g = e.g; c.b = e.b;
    }
}

// ------------------------------------------------------- the flash (1.3.0)

// The silhouette point cloud, taken from the live pose the way upstream's
// Silhouette::captureFrom takes it from the rendered char layer -- one
// point per 110-wide grid cell over the figure's art. The skeleton's own
// attachment vertices ARE the silhouette here (the rasterised outline and
// the mesh vertices bound the same art); upstream samples the rendered
// alpha at >90/255, which is what these vertices draw.
static void collectPoints(SbSlot& s, const scene::Viewport& vp, bool charsOnly,
                          int W, int H, fx::Silhouette& sil) {
    sil.width = W; sil.height = H;
    sil.alpha.clear();
    sil.px.clear(); sil.py.clear();
    const scene::Classification cls = charsOnly ? scene::classify(*s.skeleton)
                                                : scene::Classification();
    const float sx = float(W) / vp.width;
    const float sy = float(H) / vp.height;
    const float top = vp.bottom + vp.height;
    spine::Vector<float> wv;

    const int gridW = 110;
    const int gridH = std::max(1, int(std::lround(110.0 * H / std::max(1, W))));
    std::vector<bool> taken(size_t(gridW) * gridH, false);
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;

    for (size_t i = 0; i < s.skeleton->getSlots().size(); ++i) {
        if (charsOnly) {
            if (i >= cls.isChar.size() || !cls.isChar[i]) continue;
        }
        spine::Slot* slot = s.skeleton->getSlots()[i];
        spine::Attachment* att = slot->getAttachment();
        if (!att) continue;
        int n = 0;
        if (auto* r = dynamic_cast<spine::RegionAttachment*>(att)) {
            wv.setSize(8, 0); r->computeWorldVertices(*slot, wv, 0, 2); n = 4;
        } else if (auto* m = dynamic_cast<spine::MeshAttachment*>(att)) {
            n = int(m->getWorldVerticesLength()) / 2;
            wv.setSize(n * 2, 0); m->computeWorldVertices(*slot, 0, n * 2, wv, 0, 2);
        } else continue;
        for (int k = 0; k < n; ++k) {
            const float nx = ((wv[k * 2] - vp.left) * sx + 0.5f) / float(W);
            const float ny = ((top - wv[k * 2 + 1]) * sy + 0.5f) / float(H);
            if (nx < 0 || nx > 1 || ny < 0 || ny > 1) continue;
            const int cx = std::min(gridW - 1, int(nx * gridW));
            const int cy = std::min(gridH - 1, int(ny * gridH));
            if (taken[size_t(cy) * gridW + cx]) continue;
            taken[size_t(cy) * gridW + cx] = true;
            sil.px.push_back(nx); sil.py.push_back(ny);
            bx0 = std::min(bx0, nx); bx1 = std::max(bx1, nx);
            by0 = std::min(by0, ny); by1 = std::max(by1, ny);
        }
    }
    if (sil.px.empty()) { sil.ref = 0.0f; return; }
    const float bw = (bx1 - bx0) * float(W);
    const float bh = (by1 - by0) * float(H);
    sil.ref = std::max(bw, bh * 0.45f);
}

static fx::Flash* flashFor(int kind) {
    switch (kind) {
    case 0: return &g_exitFlash;
    case 1: return &g_enterFlash;
    case 2: return &g_arriveFlash;
    }
    return nullptr;
}

// Batch geometry frozen at Begin, rendered by the sink once into a
// texture and then dissolved by the flash envelope.
struct SnapStore {
    std::vector<SBBatchVertex> verts;
    std::vector<unsigned short> indices;
    std::vector<SBBatch> batches;
    bool valid = false;
};
SnapStore g_snapRoom, g_snapChar;
SnapStore g_silRoom;   // the room's CHARACTERS only -- the exit glow's silhouette

// The frozen image a flash dissolves: the same emitInto, at full alpha,
// into a snapshot store the sink renders once into a texture. kind 0 = the
// whole room (the dissolve image), 1 = the foreground, 2 = the room's
// characters only (the room's glow silhouette).
static void captureStore(SnapStore& snap, int kind) {
    const int W = g_renderW, H = g_renderH;
    if (kind == 0) {
        const scene::Viewport rvp = scene::roomViewport(W, H);
        emitInto(snap.verts, snap.indices, snap.batches, g_room, rvp, 1.0f, 0, W, H, 0);
        emitInto(snap.verts, snap.indices, snap.batches, g_room, rvp, 1.0f, 1, W, H, 1);
    } else if (kind == 1 && g_hasChar && g_charViewportValid) {
        scene::Viewport cvp = g_charViewport;
        cvp.width = cvp.height * float(W) / float(H);
        cvp.left = g_charCenterX - kCharXFrac * cvp.width;
        emitInto(snap.verts, snap.indices, snap.batches, g_char, cvp, 1.0f, 2, W, H, 2);
    } else if (kind == 2) {
        const scene::Viewport rvp = scene::roomViewport(W, H);
        emitInto(snap.verts, snap.indices, snap.batches, g_room, rvp, 1.0f, 1, W, H, 1);
    } else {
        return;
    }
    snap.valid = true;
}

// The flash burst for every active flash: upstream's
// Flash::AccumulateTriangles as direct quads on the FX_TEX page. Each
// particle: position from a silhouette point, size over life, the TriKeys
// colour at twice the glow's brightness, additive -- the only light the
// burst ever adds.
static void emitBursts(std::vector<SBBatchVertex>& V, std::vector<unsigned short>& I,
                       std::vector<SBBatch>& B, int W, int H) {
    if (!g_fx) return;
    const int fxPage = int(g_room.loader->pages.size())
        + (g_hasChar && g_char.loader ? int(g_char.loader->pages.size()) : 0);
    int triKeyCount = 0;
    const fx::Key* tk = fx::TriKeys(triKeyCount);

    const fx::Flash* kinds[3] = { &g_exitFlash, &g_enterFlash, &g_arriveFlash };
    for (const fx::Flash* f : kinds) {
        if (!f->active) continue;
        for (const fx::Flash::Particle& p : f->parts) {
            if (f->t < 0.0f || f->t > p.life) continue;
            const float u2 = f->t / p.life;
            float c[3], a;
            fx::sampleKeys(tk, triKeyCount, u2, c, &a);
            const float sz = p.s0 * fx::sizeOverLife(u2);
            if (sz < 1.0f || a < 0.01f) continue;

            const uint8_t ca = uint8_t(a * f->triAlpha * 255.0f + 0.5f);
            const float half = sz * 0.5f;
            const float cx = p.px * float(W), cy = p.py * float(H);
            const float cs = std::cos(p.rot), sn = std::sin(p.rot);
            const float u0 = (p.tile + 0) * 0.5f, u1 = (p.tile + 1) * 0.5f;

            // rotated square corners
            const float ox[4] = { -half, half, -half, half };
            const float oy[4] = { -half, -half, half, half };
            SBBatch batch;
            batch.vertexOffset = int(V.size());
            batch.vertexCount = 4;
            batch.indexOffset = int(I.size());
            batch.indexCount = 6;
            batch.page = fxPage;
            batch.blend = 1;                 // additive
            batch.pass = 3;                  // flash burst
            V.resize(batch.vertexOffset + 4);
            const uint8_t cr = uint8_t(std::min(255.0f, c[0] * 2.2f * 255.0f + 0.5f));
            const uint8_t cg = uint8_t(std::min(255.0f, c[1] * 2.2f * 255.0f + 0.5f));
            const uint8_t cb = uint8_t(std::min(255.0f, c[2] * 2.2f * 255.0f + 0.5f));
            for (int k = 0; k < 4; ++k) {
                SBBatchVertex& v = V[batch.vertexOffset + k];
                v.x = cx + ox[k] * cs - oy[k] * sn;
                v.y = cy + ox[k] * sn + oy[k] * cs;
                v.u = (k & 1) ? u1 : u0;
                v.v = (k & 2) ? 1.0f : 0.0f;
                v.r = cr; v.g = cg; v.b = cb; v.a = ca;
            }
            const unsigned short base = (unsigned short)batch.vertexOffset;
            const unsigned short qi[6] = { base, (unsigned short)(base + 1),
                                           (unsigned short)(base + 2),
                                           (unsigned short)(base + 1),
                                           (unsigned short)(base + 3),
                                           (unsigned short)(base + 2) };
            for (int k = 0; k < 6; ++k) I.push_back(qi[k]);
            B.push_back(batch);
        }
    }
}

// The Drift layer, after everything: for each live particle a soft halo
// (textured quad on the generated halo page, normal blend = upstream's
// over) and the solid triangle body (three vertices, no texture detail).
static void emitDrift(std::vector<SBBatchVertex>& V, std::vector<unsigned short>& I,
                      std::vector<SBBatch>& B, int W, int H) {
    if (!g_driftOn) return;
    const int fxPage = int(g_room.loader->pages.size())
        + (g_hasChar && g_char.loader ? int(g_char.loader->pages.size()) : 0);
    const int haloPage = fxPage + (g_fx ? 1 : 0);
    if (!g_halo.width) return;

    std::vector<fx::Drift::Halo> hs;
    std::vector<fx::Drift::Body> bs;
    g_drift.Shapes(hs, bs);
    for (size_t idx = 0; idx < bs.size(); ++idx) {
        const fx::Drift::Halo& h = hs[idx];
        const fx::Drift::Body& b = bs[idx];
        const uint8_t cr = uint8_t(b.r * 255.0f + 0.5f);
        const uint8_t cg = uint8_t(b.g * 255.0f + 0.5f);
        const uint8_t cb = uint8_t(b.b * 255.0f + 0.5f);

        // halo: a rotated rectangle sampling the halo texture; the shader's
        // premultiplied normal blend reproduces upstream's over() with the
        // coverage in the texture's own alpha.
        {
            const uint8_t ca = uint8_t(std::min(1.0f, h.a) * 255.0f + 0.5f);
            const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
            SBBatch batch;
            batch.vertexOffset = int(V.size());
            batch.vertexCount = 4;
            batch.indexOffset = int(I.size());
            batch.indexCount = 6;
            batch.page = haloPage;
            batch.blend = 0;                 // normal: the halo composites over
            batch.pass = 4;                  // drift halo
            V.resize(batch.vertexOffset + 4);
            for (int k = 0; k < 4; ++k) {
                SBBatchVertex& v = V[batch.vertexOffset + k];
                const float lx = corners[k][0] * h.halfX, ly = corners[k][1] * h.halfY;
                v.x = h.cx + lx * h.cos - ly * h.sin;
                v.y = h.cy + lx * h.sin + ly * h.cos;
                v.u = (corners[k][0] + 1) * 0.5f;
                v.v = (corners[k][1] + 1) * 0.5f;
                v.r = cr; v.g = cg; v.b = cb; v.a = ca;
            }
            const unsigned short base = (unsigned short)batch.vertexOffset;
            const unsigned short qi[6] = { base, (unsigned short)(base + 1),
                                           (unsigned short)(base + 2),
                                           (unsigned short)(base + 1),
                                           (unsigned short)(base + 3),
                                           (unsigned short)(base + 2) };
            for (int k = 0; k < 6; ++k) I.push_back(qi[k]);
            B.push_back(batch);
        }

        // body: the solid pastel triangle itself.
        {
            const uint8_t ca = uint8_t(std::min(1.0f, b.a) * 255.0f + 0.5f);
            SBBatch batch;
            batch.vertexOffset = int(V.size());
            batch.vertexCount = 3;
            batch.indexOffset = int(I.size());
            batch.indexCount = 3;
            batch.page = haloPage;           // sampled at an opaque texel
            batch.blend = 0;
            batch.pass = 5;                  // drift body
            V.resize(batch.vertexOffset + 3);
            for (int k = 0; k < 3; ++k) {
                SBBatchVertex& v = V[batch.vertexOffset + k];
                v.x = b.x[k]; v.y = b.y[k];
                v.u = 0.5f; v.v = 0.5f;      // the halo's centre is fully opaque
                v.r = cr; v.g = cg; v.b = cb; v.a = ca;
            }
            I.push_back((unsigned short)batch.vertexOffset);
            I.push_back((unsigned short)(batch.vertexOffset + 1));
            I.push_back((unsigned short)(batch.vertexOffset + 2));
            B.push_back(batch);
        }
    }
}

} // namespace

// ---------------------------------------------------------------- lifecycle

int sb_load_fx(const char* pngPath) {
    auto img = std::make_unique<sl::Image>(sl::loadPng(pngPath));
    if (!img || img->width <= 0 || img->height <= 0) return 1;
    g_fx = std::move(img);

    // The drift halo: fx::MakeHalo's triangle distance field, but with
    // white RGB and the falloff only in alpha -- the sink's shader
    // multiplies the texture by the vertex colour, so the tint must come
    // from the vertex colour and the coverage from the alpha channel.
    g_halo = fx::MakeHalo();
    for (size_t i = 0; i < g_halo.rgba.size(); i += 4) {
        g_halo.rgba[i + 0] = 255;
        g_halo.rgba[i + 1] = 255;
        g_halo.rgba[i + 2] = 255;
    }
    return 0;
}

int sb_load_room(const char* assetsDir, const char* roomName) {
    g_roomName = roomName;
    g_room = SbSlot();
    return setupSlot(g_room, assetsDir, roomName) ? 0 : 1;
}

int sb_load_character(const char* assetsDir, const char* characterName) {
    SbSlot incoming;
    g_charName = characterName;
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

// Duration of a room animation in seconds, 0 when it does not exist. The
// wake's exit timing hangs off the Touch animations' own lengths, so the
// engine asks rather than hard-coding.
float sb_animation_duration(const char* name) {
    if (!name) return 0.0f;
    spine::Animation* a = g_room.data->findAnimation(name);
    return a ? a->getDuration() : 0.0f;
}

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
    g_exitFlash.Step(step);
    g_enterFlash.Step(step);
    g_arriveFlash.Step(step);
    if (g_driftOn) g_drift.Step(step, g_renderW > 0 ? g_renderW : 1920,
                                 g_renderH > 0 ? g_renderH : 1080);
}

void sb_set_character_alpha(float alpha) {
    g_charAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

void sb_set_room_chars_alpha(float alpha) {
    g_roomCharsAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
}

bool g_armed[3] = { false, false, false };

void sb_fx_arm(int kind) {
    fx::Flash* f = flashFor(kind);
    if (!f || kind < 0 || kind > 2) return;
    const int W = g_renderW, H = g_renderH;
    if (W <= 0) return;
    fx::Silhouette sil;

    if (kind == 0) {
        const scene::Viewport rvp = scene::roomViewport(W, H);
        collectPoints(g_room, rvp, true, W, H, sil);
        g_snapRoom = SnapStore();
        captureStore(g_snapRoom, 0);
        g_silRoom = SnapStore();
        captureStore(g_silRoom, 2);
    } else if (kind == 1 && g_hasChar && g_charViewportValid) {
        scene::Viewport cvp = g_charViewport;
        cvp.width = cvp.height * float(W) / float(H);
        cvp.left = g_charCenterX - kCharXFrac * cvp.width;
        collectPoints(g_char, cvp, false, W, H, sil);
        g_snapChar = SnapStore();
        captureStore(g_snapChar, 1);
    } else {
        // kind 2 (arrival/boot): no snapshot -- its glow tracks the live,
        // still-ramping characters, re-rendered by the sink each frame.
    }

    fx::Flash fresh;
    fresh.tint = fx::TintFor(kind == 1 ? g_charName : g_roomName, fresh.tintCount);
    fresh.Begin(sil, kind == 0 ? 0x5EED0001u : kind == 1 ? 0x5EED0002u : 0x5EED0003u);
    fresh.active = false;   // armed: captured, not yet lit
    *f = std::move(fresh);
    g_armed[kind] = true;
}

void sb_fx_begin(int kind) {
    // An armed flash (sb_fx_arm) starts from its frozen capture; the pose
    // that was armed is the one that dissolves, NOT the current one -- the
    // Touch reaction keeps running live and hidden underneath.
    if (kind >= 0 && kind <= 2 && g_armed[kind]) {
        fx::Flash* f = flashFor(kind);
        f->active = true;
        f->t = 0.0f;
        g_armed[kind] = false;
        return;
    }
    fx::Flash* f = flashFor(kind);
    if (!f || g_renderW <= 0) return;
    const int W = g_renderW, H = g_renderH;
    fx::Silhouette sil;

    if (kind == 0) {
        const scene::Viewport rvp = scene::roomViewport(W, H);
        collectPoints(g_room, rvp, true, W, H, sil);
        // Two frozen images, as upstream stages them: the exitSnap is the
        // WHOLE room at full alpha (stage included, so the desk keeps
        // occluding her while it dissolves), and the GLOW's silhouette is
        // the characters alone -- the glow must not take its alpha from
        // the walls.
        g_snapRoom = SnapStore();
        captureStore(g_snapRoom, 0);
        g_silRoom = SnapStore();
        captureStore(g_silRoom, 2);
    } else if (kind == 1 && g_hasChar && g_charViewportValid) {
        scene::Viewport cvp = g_charViewport;
        cvp.width = cvp.height * float(W) / float(H);
        cvp.left = g_charCenterX - kCharXFrac * cvp.width;
        collectPoints(g_char, cvp, false, W, H, sil);
        g_snapChar = SnapStore();
        captureStore(g_snapChar, 1);
    } else {
        // kind 2 (arrival/boot): no snapshot -- its glow tracks the live,
        // still-ramping characters, re-rendered by the sink each frame.
    }

    const uint32_t seed = kind == 0 ? 0x5EED0001u : kind == 1 ? 0x5EED0002u : 0x5EED0003u;
    fx::Flash fresh;
    fresh.tint = fx::TintFor(kind == 1 ? g_charName : g_roomName, fresh.tintCount);
    fresh.Begin(sil, seed);
    *f = std::move(fresh);
}

// "Active" for the sink and engine means the glow ENVELOPE is lit:
// upstream keeps Flash::active until max(life, 0.85s) so the burst
// particles can outlive the glow, but magnitude reaches 0 at t == life.
int sb_fx_active(int kind) {
    fx::Flash* f = flashFor(kind);
    return f && f->active && f->t < f->life ? 1 : 0;
}

float sb_fx_mag(int kind) {
    fx::Flash* f = flashFor(kind);
    if (!f || !f->active) return 0.0f;
    float u = f->t / f->life;
    if (u > 1.0f) u = 1.0f;
    return fx::magnitude(u);
}

void sb_fx_color(int kind, float out[3]) {
    fx::Flash* f = flashFor(kind);
    if (!f || !f->active || f->t >= f->life) { out[0] = out[1] = out[2] = 0.0f; return; }
    float c[3];
    fx::sampleKeys(f->tint, f->tintCount, f->t / f->life, c, nullptr);
    const float g = f->gain * fx::magnitude(f->t / f->life);
    for (int i = 0; i < 3; ++i) out[i] = c[i] * g;
}

void sb_fx_reset(void) {
    g_exitFlash = fx::Flash{};
    g_enterFlash = fx::Flash{};
    g_arriveFlash = fx::Flash{};
    g_drift = fx::Drift();
    g_driftOn = false;
    g_snapRoom = SnapStore();
    g_snapChar = SnapStore();
    g_silRoom = SnapStore();
    for (bool& b : g_armed) b = false;
}

int sb_snapshot(int kind,
                const SBBatchVertex** outVertices, int* outVertexCount,
                const unsigned short** outIndices, int* outIndexCount,
                const SBBatch** outBatches, int* outBatchCount) {
    const SnapStore& s = kind == 0 ? g_snapRoom : kind == 1 ? g_snapChar : g_silRoom;
    if (!s.valid || s.batches.empty()) {
        *outVertices = nullptr; *outIndices = nullptr; *outBatches = nullptr;
        *outVertexCount = 0; *outIndexCount = 0; *outBatchCount = 0;
        return 1;
    }
    *outVertices = s.verts.data();
    *outVertexCount = int(s.verts.size());
    *outIndices = s.indices.data();
    *outIndexCount = int(s.indices.size());
    *outBatches = s.batches.data();
    *outBatchCount = int(s.batches.size());
    return 0;
}

void sb_drift_start(void) {
    if (g_driftOn) return;
    g_drift = fx::Drift();   // fresh field: the pattern re-seeds each wake
    g_driftOn = true;
}

int sb_render(int width, int height,
              const SBBatchVertex** outVertices, int* outVertexCount,
              const unsigned short** outIndices, int* outIndexCount,
              const SBBatch** outBatches, int* outBatchCount) {
    g_renderW = width;
    g_renderH = height;
    g_vertices.clear();
    g_indices.clear();
    g_batches.clear();

    //   room pass 0  the room's non-character stage (walls, sea, desks,
    //                dressing). mute_patterns and the protect rects mute
    //                here; the system UI reads through.
    //   room pass 1  the room's own characters (the engine mutes them to 0
    //                while the exit's frozen snapshot replaces them).
    //   char pass 2  the foreground figure.
    //   pass 3       the flashes' triangle burst; 4/5 the Drift layer.
    //
    // The glow itself runs on the sink: it samples the flash's frozen (or
    // live) silhouette and adds tint * gain * magnitude, which is where
    // upstream's AccumulateGlow lands once the bloom approximation sits in
    // the pipeline.
    scene::Viewport rvp = scene::roomViewport(width, height);
    emitInto(g_vertices, g_indices, g_batches, g_room, rvp, 1.0f, 0,
             width, height, 0);
    emitInto(g_vertices, g_indices, g_batches, g_room, rvp, g_roomCharsAlpha, 1,
             width, height, 1);

    if (g_hasChar && g_charViewportValid && g_charAlpha > 0.0f) {
        // Re-derive the camera for THIS target: width follows the real
        // aspect so sx == sy (uniform scale), and her centre is planted at
        // kCharXFrac of the current width. The stored viewport contributes
        // only its height and bottom.
        scene::Viewport cvp = g_charViewport;
        cvp.width = cvp.height * float(width) / float(height);
        cvp.left = g_charCenterX - kCharXFrac * cvp.width;
        emitInto(g_vertices, g_indices, g_batches, g_char, cvp, g_charAlpha, 2,
                 width, height, 2);
    }

    emitBursts(g_vertices, g_indices, g_batches, width, height);
    emitDrift(g_vertices, g_indices, g_batches, width, height);

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
    if (g_fx) ++n;              // the burst sprite (FX_TEX_Triangle_02_1)
    if (g_halo.width) ++n;      // the Drift halo
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
    } else if (g_halo.width && index == roomPages + charPages + (g_fx ? 1 : 0)) {
        img = &g_halo;
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
