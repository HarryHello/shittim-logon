// shittim-logon / render / scene.h
//
// The pieces of scene handling that the offline renderer and the secure-desktop
// overlay both need: the camera, the Spine-to-rasteriser bridge, and the rules for
// telling the character apart from the room she is standing in.
//
// It lives in render/ rather than overlay/ because this is where it can be run
// against a file and looked at. The overlay includes it so that what ships is the
// same code that produced the frames someone actually inspected -- a second
// implementation for the "real" target is how the two quietly drift apart.

#pragma once

#include "image.h"
#include "raster.h"

#include <spine/spine.h>
#include <spine/SkeletonRenderer.h>

#include <cctype>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

namespace scene {

// The wallpaper's camera: ortho2d(-1440, 90, 2880, 1620), in Spine's own y-up
// world. The defaults are the room's; the foreground character is framed
// separately, because a camera that suits a wallpaper puts her feet well below a
// 16:9 logon screen.
struct Viewport {
    float left = -1440.0f, bottom = 90.0f, width = 2880.0f, height = 1620.0f;
};

// The room's camera for a target of W x H, as a uniform cover of the world rect.
//
// The world above is authored 16:9, and drawSkeleton scales x and y independently
// (`sx = W/vp.width`, `sy = H/vp.height`). On 16:9 those are equal and nobody ever
// noticed. On anything else the room stretches -- 2736x1824 on a 3:2 Surface is
// 18% of vertical stretch -- while frameCharacter, which derives its width from
// W/H, does not stretch at all. The two halves of the same scene then disagree
// about where a desk is, and the character stands next to furniture that has moved.
//
// Uniform cover fixes both halves at once, and it is also the rule Windows applies
// to LockScreenImage. That is the second reason for it: stage 1's baked still and
// stage 3's live room then crop identically, so the seam between the still and the
// animation stays invisible on every aspect rather than only on 16:9.
//
// On 16:9 this returns exactly the constant viewport above, so nothing already
// measured or baked at 16:9 moves by a pixel.
inline Viewport roomViewport(int W, int H) {
    const Viewport world;
    if (W <= 0 || H <= 0) return world;
    const float cx = world.left   + world.width  * 0.5f;
    const float cy = world.bottom + world.height * 0.5f;
    // px per world unit, the larger of the two so the short axis is the one that
    // fills and the long axis is the one that gets cropped.
    const float s = (std::max)((float)W / world.width, (float)H / world.height);
    Viewport vp;
    vp.width  = (float)W / s;
    vp.height = (float)H / s;
    vp.left   = cx - vp.width  * 0.5f;
    vp.bottom = cy - vp.height * 0.5f;
    return vp;
}

inline sl::Blend toBlend(spine::BlendMode m) {
    switch (m) {
    case spine::BlendMode_Additive: return sl::Blend::Additive;
    case spine::BlendMode_Multiply: return sl::Blend::Multiply;
    case spine::BlendMode_Screen:   return sl::Blend::Screen;
    default:                        return sl::Blend::Normal;
    }
}

// Refuses rather than asserts. spine-cpp's setAnimation aborts the process on an
// unknown name, and this code runs on the logon screen, where aborting is a crash
// in front of the credential UI.
inline bool playIfPresent(spine::AnimationState &state, spine::SkeletonData &data,
                          int track, const char *name, bool loop) {
    if (!data.findAnimation(name)) return false;
    state.setAnimation(track, name, loop);
    return true;
}

// ------------------------------------------------------- slot classification
//
// The room and the character are one skeleton, so anything that treats the
// character as a unit -- the glow, hiding her at the handover -- has to separate
// them first. Ported from the reference implementation together with
// the reasons each rule exists, because every one of them is there because
// something specific went wrong without it.

inline std::string lower(const std::string &s) {
    std::string o = s;
    for (char &c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

// Classify on the attachment, never on the slot. Slot names do not carry the
// BG_/pose prefixes, and classifying on them is what put a 1747x280 backdrop
// strip inside the character's glow on the reference's first attempt.
inline std::string attachmentPath(spine::Attachment *att) {
    if (!att) return "";
    if (auto *r = dynamic_cast<spine::RegionAttachment *>(att))
        return r->getPath().length() ? r->getPath().buffer() : r->getName().buffer();
    if (auto *m = dynamic_cast<spine::MeshAttachment *>(att))
        return m->getPath().length() ? m->getPath().buffer() : m->getName().buffer();
    return att->getName().buffer();
}

// Staging that ships inside a pose group but is not part of her: the desk and
// chair she sits at, her drop shadow, floor glow sprites.
//
// The reference also treats a trailing `_base\d*` as dressing -- a ground base
// plate under the character. **That rule is wrong for this skeleton and is not
// here.** `B_00_base1` and `B_00_base2` measure 46x27 px at y 227..254, which is
// directly above her head, and `E_00_base1` is the same thing above the
// companion's: in these assets "base" is the *halo*. Filing it as dressing leaves
// two haloes hanging in mid-air over an empty classroom after the wake.
//
// Nothing in this skeleton is an actual ground plate, so there is nothing to lose
// by dropping the rule. If a scene ever ships one, it needs a test that looks at
// where the sprite is rather than at what it is called.
inline bool isStageDressing(const std::string &raw) {
    std::string s = lower(raw);
    auto has = [&](const char *n) { return s.find(n) != std::string::npos; };
    return has("desk") || has("chair") || has("shadow") || has("light");
}

// Pose groups are the leading `B_00`, `C_01` ... prefix.
// A..F, and the 'A' is the whole reason this comment exists.
//
// The range used to start at 'B', which was an inference from the only skeleton
// anyone had looked at. The daytime room names its character `A_01_Head_01`,
// `A_01_halo`, `A_01_F_Torso` -- so with 'B' as the floor, dominantGroup found
// nothing at all there: `dominant group '?', character slots -1..-1 of 297`. Every
// consequence of that is silent. The room/character partition becomes empty, so the
// baked still keeps the character in it and the overlay draws her again on top --
// the "two of her on screen" failure render_still.cpp already documents -- and the
// exit flash has no silhouette to cut.
//
// Extending it cannot disturb the night room, and that is checked rather than
// asserted: the only `A_` attachment there is `A_00_Desk`, which isStageDressing
// already removes before partGroup is reached, so the dominant group stays 'B' with
// the same 109..347 span. render_still --probe 1 on both rooms is the test.
//
// 'G' was left out of the first widening, on the grounds that nobody had looked at
// what the night room's `G_` attachments were. Looking at them settled it: in
// Idle_04 the figure standing on the left is `G_01_R_Leg`, `G_01_Torso_Original`,
// `G_01_B_Hair_01`, `G_01_Skirt_` -- an entire person, classified as scenery, baked
// into the still, and therefore still sitting there after the live one walks off.
// The scene-pair sweep is what caught it: night_5 removed 1,280 px where the other
// night idles removed twelve times that.
//
// The range is now A..G because that is what these four skeletons contain. It is
// still a range, and a range is still a guess about the next export -- the check
// that matters is the sweep in tools, not the bound here.
// Two shapes, because these exports use two.
//
//   `A_01_Head_01`, `B_00_Skirt`, `G_01_R_Leg`   -- one letter, underscore, digit
//   `Black01_B_Hair_01`, `Black01_Leg`           -- letters, digits, underscore
//
// The second was missed entirely, and it is the day room's companion, whose
// every part is named `Black01_...`. She was filed as scenery, baked into the
// wallpaper, and left without a silhouette for the exit flash. What hid it is that
// `Black01_chair_07_cover` and `Black01_Shadow` -- real scenery with the same prefix
// -- were being removed correctly all along, by isStageDressing.
//
// This is the third naming rule in this file to be written against one export and
// break on another (`_base` meaning ground plate, the A..F letter range, and now
// this). The pattern is worth naming: **a rule about a name is a rule about one
// artist's habit.** The check that catches these is not a better rule, it is
// Check-ScenePartition.ps1 rendering every scene and comparing what came out.
inline char partGroup(const std::string &s) {
    if (s.size() < 3) return 0;

    // letter '_' digit
    const char c = (char)std::toupper((unsigned char)s[0]);
    if (c >= 'A' && c <= 'G' && s[1] == '_' && std::isdigit((unsigned char)s[2]))
        return c;

    // letters digits '_'
    size_t i = 0;
    while (i < s.size() && std::isalpha((unsigned char)s[i])) i++;
    if (i == 0) return 0;                       // must start with letters
    const size_t digitsAt = i;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
    if (i == digitsAt) return 0;                // and must have digits after them
    if (i >= s.size() || s[i] != '_') return 0; // and an underscore after those
    // Grouped under its first letter. Only the "does it have a group at all" answer
    // is load-bearing -- isSceneAttachment ignores which one -- so a collision with
    // the single-letter groups costs nothing but a less interesting probe line.
    const char g = (char)std::toupper((unsigned char)s[0]);
    return (g >= 'A' && g <= 'G') ? g : 'A';
}

// Which pose group has the most attached parts.
//
// NOT used to decide what is a character -- see isSceneAttachment. Kept because it
// is the reference implementation's rule and because knowing the count per group is
// what showed the rule was wrong here.
inline char dominantGroup(spine::Skeleton &sk) {
    int counts[8] = {0};
    char best = 0; int bestN = 0;
    for (size_t i = 0; i < sk.getSlots().size(); i++) {
        std::string s = attachmentPath(sk.getSlots()[i]->getAttachment());
        if (s.empty() || isStageDressing(s)) continue;
        char g = partGroup(s);
        if (!g) continue;
        // Indexed from 'A', not 'B'. With the range widened, `g - 'B'` is -1 for the
        // day room's every character part -- a write one int before the array.
        int &c = counts[g - 'A'];
        if (++c > bestN) { bestN = c; best = g; }
    }
    return best;
}

// `group` is accepted and ignored. It is kept in the signature because the
// reference implementation's rule ends `return g !== group` -- "anything not in the
// dominant pose group is scene" -- and dropping that test is a deliberate change
// that needs to stay visible.
//
// That rule assumes one character whose other poses are still attached. This room
// has *two* characters: the seated one (pose group B, 48 slots) and the companion
// that `Idle_11` brings in (group E, 41 slots). Both are on stage with visible
// alpha at the same time. Under the reference's rule B wins the count, every one of
// E's 41 slots is filed as scenery, and the consequences are exactly what showed up
// on screen: the companion's hair, torso, neck and ribbons get baked into the
// background still and stay sitting there after the wake has carried her off.
//
// Treating every pose group as character is also what the source material says
// should happen. FINDINGS records keyframe #23 of the reference recording with
// *both characters flashing in the same frame*, each with its own tint -- so the
// companion is meant to leave alongside her, not stay behind.
//
// Parts of a genuinely unshown pose are not a problem: the animation holds them at
// alpha 0, so they draw nothing and contribute nothing to the glow.
inline bool isSceneAttachment(spine::Attachment *att, char /*group*/,
                              const char *slotName = nullptr) {
    if (!att) return true;
    std::string s = attachmentPath(att);
    std::string ls = lower(s), ln = lower(att->getName().buffer());
    if (ls.rfind("bg_", 0) == 0 || ls.rfind("bg/", 0) == 0) return true;
    if (ln.rfind("bg_", 0) == 0 || ln.rfind("bg/", 0) == 0) return true;
    // There used to be a rule here reading "anything named `a_<digit>` is scenery".
    // It was true of the only skeleton it was written against -- the night room's
    // sole `A_` attachment is `A_00_Desk` -- and it is redundant even there, because
    // isStageDressing already removes anything with "desk" in it. In the day room it
    // is a catastrophe: that room names its entire character `A_01_...`, so the rule
    // filed every part of her as background and classify() returned no character
    // slots at all. Removed, and the night room's classification is unchanged by it
    // (`dominant group 'B', character slots 109..347 of 381`, checked both ways).
    //
    // The general lesson, which this file has now learned twice: a rule keyed on a
    // name prefix is a rule about one export. `_base` meaning "ground plate" was the
    // first, and in these assets it is the halo.

    // The pose group comes from the attachment, per the reference's rule -- slot
    // names do not reliably carry the BG_ prefixes, and classifying on them is what
    // once put a full-width backdrop strip inside the character's glow.
    //
    // But exactly one slot in this skeleton breaks the other way. Slot 312 is
    // `B_00_Skirt_Pattern_Original` and its attachment is plain
    // `Skirt_Pattern_Original` -- the group prefix is on the slot and not on the
    // art. With the attachment alone it has no group, falls through to scenery,
    // gets baked into the background, and stays behind when she leaves: a black
    // skirt draped over her chair after the wake has carried her off.
    //
    // So the slot name is a fallback for the group test *only*, and only when the
    // attachment yields nothing. The background and stage-dressing rules above and
    // below still run on the attachment, so this cannot promote scenery.
    char g = partGroup(s);
    if (!g && slotName) g = partGroup(slotName);
    if (!g) return true;
    return isStageDressing(s);
}

// Which slots are her, and where she sits in the draw order.
//
// The second half of that is the part it is easy to miss. Splitting the skeleton
// into "her" and "everything else" is not enough to composite correctly, because
// some of "everything else" belongs *in front of* her -- the desk she is sitting
// at. The reference implementation handles this with a draw-order split around the
// last character slot, and so does this: everything after `maxIdx` is drawn over
// her, whatever it is.
//
// Recomputed per frame, because the visible pose group changes with the animation.
struct Classification {
    std::vector<bool> isChar;
    int maxIdx = -1, minIdx = -1;
    char group = 0;
};

inline Classification classify(spine::Skeleton &sk) {
    Classification c;
    c.group = dominantGroup(sk);
    c.isChar.resize(sk.getSlots().size());
    for (size_t i = 0; i < sk.getSlots().size(); i++) {
        spine::Slot *slot = sk.getSlots()[i];
        const bool ch = !isSceneAttachment(slot->getAttachment(), c.group,
                                           slot->getData().getName().buffer());
        c.isChar[i] = ch;
        if (ch) { c.maxIdx = (int)i; if (c.minIdx < 0) c.minIdx = (int)i; }
    }
    return c;
}

// The contiguous runs of character slots, in draw order.
//
// There is more than one character, and this is what a single under/over split
// gets wrong. Measured on this skeleton: the companion occupies slots 109-147 and
// the seated character 306-347, and `BG_class_room` -- the wall -- is slot **150**.
// The wall is drawn after the companion and before the seated one, so it belongs
// *in front of* the companion and *behind* the other. One global `maxIdx` cannot
// express that: taking the last character slot (347) files the wall as "under" and
// draws both characters over the wall, which is exactly the symptom -- the
// companion's body floating in front of a wall she should be standing behind.
//
// So occlusion is per character: for each run, everything drawn after that run's
// last slot occludes it.
inline std::vector<std::pair<int, int>> characterRuns(const Classification &c) {
    std::vector<std::pair<int, int>> runs;
    int start = -1;
    for (size_t i = 0; i < c.isChar.size(); i++) {
        if (c.isChar[i]) {
            if (start < 0) start = (int)i;
        } else if (start >= 0) {
            runs.emplace_back(start, (int)i - 1);
            start = -1;
        }
    }
    if (start >= 0) runs.emplace_back(start, (int)c.isChar.size() - 1);
    return runs;
}

enum class Pass {
    All,
    CharOnly,     // her
    Under,        // everything up to and including her
    UnderNoChar,  // everything up to her, minus her
    NoChar,       // the whole room, minus her -- the background layer
    Over,         // everything after her: what has to stay in front of her
};

inline bool hiddenInPass(const Classification &c, size_t i, Pass p) {
    switch (p) {
    case Pass::All:         return false;
    case Pass::CharOnly:    return !c.isChar[i];
    case Pass::Under:       return (int)i > c.maxIdx;
    case Pass::UnderNoChar: return (int)i > c.maxIdx || c.isChar[i];
    case Pass::NoChar:      return c.isChar[i];
    case Pass::Over:        return (int)i <= c.maxIdx;
    }
    return false;
}

// ------------------------------------------------------------------ drawing

struct DrawStats {
    size_t batches = 0, triangles = 0;
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
};

inline DrawStats drawSkeleton(sl::Target &target, spine::Skeleton &skeleton,
                              spine::SkeletonRenderer &renderer, const Viewport &vp) {
    const float sx = target.width  / vp.width;
    const float sy = target.height / vp.height;
    const float top = vp.bottom + vp.height;

    DrawStats st;
    st.minX = st.minY = 1e9f; st.maxX = st.maxY = -1e9f;

    std::vector<sl::Vertex> verts;
    for (spine::RenderCommand *cmd = renderer.render(skeleton); cmd; cmd = cmd->next) {
        const sl::Image *tex = static_cast<const sl::Image *>(cmd->texture);
        if (!tex) continue;
        verts.resize(cmd->numVertices);
        for (int i = 0; i < cmd->numVertices; i++) {
            // Spine is y-up, the target is y-down.
            verts[i].x = (cmd->positions[i * 2] - vp.left) * sx;
            verts[i].y = (top - cmd->positions[i * 2 + 1]) * sy;
            verts[i].u = cmd->uvs[i * 2];
            verts[i].v = cmd->uvs[i * 2 + 1];
            const uint32_t c = cmd->colors[i];          // packed ARGB
            verts[i].a = ((c >> 24) & 0xFF) / 255.0f;
            verts[i].r = ((c >> 16) & 0xFF) / 255.0f;
            verts[i].g = ((c >>  8) & 0xFF) / 255.0f;
            verts[i].b = ( c        & 0xFF) / 255.0f;
            if (verts[i].x < st.minX) st.minX = verts[i].x;
            if (verts[i].x > st.maxX) st.maxX = verts[i].x;
            if (verts[i].y < st.minY) st.minY = verts[i].y;
            if (verts[i].y > st.maxY) st.maxY = verts[i].y;
        }
        sl::TextureView view{ tex->rgba.data(), tex->width, tex->height };
        sl::drawTriangles(target, view, verts.data(), cmd->indices,
                          (size_t)cmd->numIndices, toBlend(cmd->blendMode));
        st.batches++;
        st.triangles += (size_t)cmd->numIndices / 3;
    }
    return st;
}

// Renders a subset of the skeleton by muting the rest.
//
// Alpha 0 makes the rasteriser skip the pixel outright, and unlike detaching the
// attachment it does not disturb deform timelines -- which matters because both
// passes run inside a single posed frame.
inline DrawStats drawPass(sl::Target &target, spine::Skeleton &sk,
                          spine::SkeletonRenderer &r, const Viewport &vp, Pass p) {
    const Classification c = classify(sk);
    std::vector<float> saved(sk.getSlots().size());
    for (size_t i = 0; i < sk.getSlots().size(); i++) {
        spine::Slot *s = sk.getSlots()[i];
        saved[i] = s->getColor().a;
        if (hiddenInPass(c, i, p)) s->getColor().a = 0.0f;
    }
    DrawStats st = drawSkeleton(target, sk, r, vp);
    for (size_t i = 0; i < sk.getSlots().size(); i++)
        sk.getSlots()[i]->getColor().a = saved[i];
    return st;
}

// The room half is `NoChar`, not `UnderNoChar`, and the difference is the whole
// desk in front of the character.
//
// `UnderNoChar` also drops everything drawn *after* the last character slot, which
// is how the two halves were originally meant to fit together: the background
// carried what is behind her, the overlay redrew the scenery in front of her on
// top. That second half no longer exists -- the overlay sits above LogonUI, so
// anything it draws lands on the password box, and drawing the room's foreground
// there was reverted for exactly that reason.
//
// Which left that scenery in neither layer. Measured on this skeleton: the desk
// the character sits at, the chairs beside her and the desks around the companion are all
// after slot 347, so the baked background had a desk-shaped hole with only the
// legs left standing, and the composite showed her sitting at a desk that was not
// there.
//
// The background is underneath everything, so it can hold the whole room in the
// skeleton's own draw order. Occlusion is then the overlay's job alone, and it
// already does it the only way it can: by cutting the character's alpha where the
// room draws in front of her, so the background's desk shows through opaque.
inline DrawStats drawFiltered(sl::Target &target, spine::Skeleton &sk,
                              spine::SkeletonRenderer &r, const Viewport &vp,
                              bool wantCharacter) {
    return drawPass(target, sk, r, vp, wantCharacter ? Pass::CharOnly : Pass::NoChar);
}

// Prints how every slot was classified. Exists because "some of her clothing stays
// behind when she leaves" is a classification bug, and the only way to find which
// rule let a part through is to see the rules' verdicts next to the names.
inline void describeClassification(spine::Skeleton &sk,
                                   int (*out)(const char *, ...),
                                   const Viewport *vp = nullptr,
                                   int W = 0, int H = 0) {
    const Classification c = classify(sk);

    // Where each slot lands on screen. Names alone were not enough: a stray halo
    // left floating in the window had to be identified by position, because
    // nothing in its name said "halo".
    std::vector<float> bx0, by0, bx1, by1;
    if (vp && W > 0 && H > 0) {
        const float sx = W / vp->width, sy = H / vp->height, top = vp->bottom + vp->height;
        bx0.assign(sk.getSlots().size(), 1e9f); by0.assign(sk.getSlots().size(), 1e9f);
        bx1.assign(sk.getSlots().size(), -1e9f); by1.assign(sk.getSlots().size(), -1e9f);
        spine::Vector<float> v;
        for (size_t i = 0; i < sk.getSlots().size(); i++) {
            spine::Slot *s = sk.getSlots()[i];
            spine::Attachment *att = s->getAttachment();
            int n = 0;
            if (auto *r = dynamic_cast<spine::RegionAttachment *>(att)) {
                v.setSize(8, 0); r->computeWorldVertices(*s, v, 0, 2); n = 4;
            } else if (auto *m = dynamic_cast<spine::MeshAttachment *>(att)) {
                n = (int)m->getWorldVerticesLength() / 2;
                v.setSize(n * 2, 0); m->computeWorldVertices(*s, 0, n * 2, v, 0, 2);
            } else continue;
            for (int k = 0; k < n; k++) {
                const float px = (v[k * 2] - vp->left) * sx;
                const float py = (top - v[k * 2 + 1]) * sy;
                bx0[i] = std::min(bx0[i], px); bx1[i] = std::max(bx1[i], px);
                by0[i] = std::min(by0[i], py); by1[i] = std::max(by1[i], py);
            }
        }
    }
    out("    dominant group '%c', character slots %d..%d of %d\n",
        c.group ? c.group : '?', c.minIdx, c.maxIdx, (int)sk.getSlots().size());
    out("    %-4s %-30s %-32s %-5s %-6s %-6s %-5s\n",
        "idx", "slot", "attachment", "group", "alpha", "dress", "char");
    for (size_t i = 0; i < sk.getSlots().size(); i++) {
        spine::Slot *s = sk.getSlots()[i];
        const std::string path = attachmentPath(s->getAttachment());
        if (path.empty()) continue;
        const char g = partGroup(path);
        char box[48] = "";
        if (!bx0.empty() && bx1[i] > bx0[i])
            std::snprintf(box, sizeof(box), "%.0f,%.0f..%.0f,%.0f",
                          bx0[i], by0[i], bx1[i], by1[i]);
        out("    %-4d %-30s %-32s %-5c %-6.2f %-6s %-5s %s\n",
            (int)i, s->getData().getName().buffer(), path.c_str(),
            g ? g : '-', s->getColor().a,
            isStageDressing(path) ? "yes" : "", c.isChar[i] ? "CHAR" : "", box);
    }
}

// The world box of every triangle the skeleton emits: a guaranteed superset of
// what is visible, useful only as a camera that provably cannot clip.
inline bool geometryWorldBounds(spine::Skeleton &sk, spine::SkeletonRenderer &renderer,
                                float &x0, float &y0, float &x1, float &y1) {
    x0 = y0 = 1e9f; x1 = y1 = -1e9f;
    bool any = false;
    for (spine::RenderCommand *cmd = renderer.render(sk); cmd; cmd = cmd->next) {
        if (!cmd->texture) continue;
        for (int i = 0; i < cmd->numVertices; i++) {
            if (((cmd->colors[i] >> 24) & 0xFF) == 0) continue;
            const float wx = cmd->positions[i * 2], wy = cmd->positions[i * 2 + 1];
            if (wx < x0) x0 = wx;
            if (wx > x1) x1 = wx;
            if (wy < y0) y0 = wy;
            if (wy > y1) y1 = wy;
            any = true;
        }
    }
    return any && x1 > x0 && y1 > y0;
}

// Frames a foreground character: head near the top, the top `visibleFrac` of her
// filling the screen, and everything below that running off the bottom edge.
//
// Cropping her is the point, not a compromise. A figure drawn whole, feet on the
// floor, reads as standing in the room; one that overflows the frame reads as
// standing in front of the camera, which is where this character is meant to be.
// `xFrac` puts her to one side so the centred credential UI stays clear.
//
// The obvious way to measure -- start from some camera, see where she landed,
// correct -- does not converge when the starting camera clips her: a clipped
// measurement is a lower bound, each correction makes her larger, and the
// clipping gets worse. Measuring through the geometry box instead cannot clip by
// construction, so one solve is enough and there is no iteration to diverge.
inline Viewport frameCharacter(sl::Target &scratch, spine::Skeleton &sk,
                               spine::SkeletonRenderer &renderer,
                               float xFrac, float visibleFrac, float topMargin,
                               float *visibleW = nullptr, float *visibleH = nullptr) {
    const int W = scratch.width, H = scratch.height;
    // A zero width is the failure signal, since no usable camera has one. The
    // default-constructed Viewport is the room's real camera, so returning that on
    // failure would silently render the character through the wallpaper's framing
    // rather than reporting that nothing was found.
    Viewport out;
    out.width = out.height = 0.0f;

    float gx0, gy0, gx1, gy1;
    if (!geometryWorldBounds(sk, renderer, gx0, gy0, gx1, gy1)) return out;

    Viewport measure;
    measure.height = (gy1 - gy0) * 1.02f;
    measure.width  = measure.height * W / H;
    if (measure.width < (gx1 - gx0) * 1.02f) {
        measure.width  = (gx1 - gx0) * 1.02f;
        measure.height = measure.width * H / W;
    }
    measure.left   = (gx0 + gx1) * 0.5f - measure.width  * 0.5f;
    measure.bottom = (gy0 + gy1) * 0.5f - measure.height * 0.5f;

    scratch.clear(0.0f, 0.0f, 0.0f, 0.0f);
    drawSkeleton(scratch, sk, renderer, measure);

    int px0 = W, py0 = H, px1 = -1, py1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (scratch.rgba[(size_t(y) * W + x) * 4 + 3] > 0.03f) {
                if (x < px0) px0 = x;
                if (x > px1) px1 = x;
                if (y < py0) py0 = y;
                if (y > py1) py1 = y;
            }
    if (px1 < 0) return out;

    const float s    = W / measure.width;                  // px per world unit
    const float top  = measure.bottom + measure.height;
    const float vx0  = measure.left + px0 / s;
    const float vx1  = measure.left + (px1 + 1) / s;
    const float vy1  = top - py0 / s;
    const float vy0  = top - (py1 + 1) / s;

    // Solve so that her head sits `topMargin` down from the top, and the point
    // `visibleFrac` of the way down her body lands exactly on the bottom edge.
    // Everything past that is off-screen, which is what sells the foreground.
    const float charH = vy1 - vy0;
    out.height = visibleFrac * charH / (1.0f - topMargin);
    out.width  = out.height * W / H;
    out.bottom = vy1 - (1.0f - topMargin) * out.height;
    out.left   = (vx0 + vx1) * 0.5f - xFrac * out.width;
    if (visibleW) *visibleW = vx1 - vx0;
    if (visibleH) *visibleH = charH;
    return out;
}

// ------------------------------------------------------------- compositing

inline void compositeOver(sl::Target &dst, const sl::Target &src) {
    for (size_t i = 0; i < dst.rgba.size(); i += 4) {
        float a = src.rgba[i + 3];
        if (a <= 0.0f) continue;
        for (int c = 0; c < 3; c++)
            dst.rgba[i + c] = src.rgba[i + c] * a + dst.rgba[i + c] * (1.0f - a);
        dst.rgba[i + 3] = a + dst.rgba[i + 3] * (1.0f - a);
    }
}

// The scrim Windows lays over the wallpaper before drawing the credential UI,
// measured in docs/stage1-findings.md.
//
// The overlay draws *above* that scrim, so anything it puts into the room -- the
// character reacting on the spot she is about to leave -- has to have the same
// curve applied by hand, or she is visibly brighter than the background she is
// standing in. The foreground character is above everything and gets none of it.
inline void applyLogonDim(sl::Target &t, float k = 0.8203f, float b = -3.36f / 255.0f) {
    for (size_t i = 0; i < t.rgba.size(); i += 4)
        for (int c = 0; c < 3; c++) {
            const float v = t.rgba[i + c] * k + b;
            t.rgba[i + c] = v < 0.0f ? 0.0f : v;
        }
}

} // namespace scene
