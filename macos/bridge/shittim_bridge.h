// macos/bridge/shittim_bridge.h
//
// The C ABI between the Swift shell and the C++ mechanism layer
// (../adapter): one narrow seam, "pose the skeletons, hand me triangles",
// so the camera, the classification rules and the scene table stay in the
// one place that owns them.
//
// All pointers returned by this API are valid until the next sb_* call on
// the same thread. Single-threaded: call everything from one thread.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- types ----------------------------------------------------------

typedef struct SBBatchVertex {
    float x, y;         // target pixels (top-left origin)
    float u, v;         // 0..1
    uint8_t r, g, b, a; // straight alpha; premultiplied in the shader
} SBBatchVertex;

typedef struct SBBatch {
    int vertexOffset;   // into the vertex array returned by sb_render
    int vertexCount;
    int indexOffset;    // into the index array returned by sb_render
    int indexCount;
    int page;           // atlas page index (sb_page)
    int blend;          // 0 normal, 1 additive, 2 multiply, 3 screen
    int pass;           // 0 room stage, 1 room characters, 2 foreground,
                        // 3 flash burst, 4 drift halo, 5 drift body
} SBBatch;

// ---- lifecycle ------------------------------------------------------

// Load a room skeleton (self-contained: room, seated character, props).
// Returns 0 on success. Real names, e.g. "arona_workpage_nighttime_2".
int sb_load_room(const char* assetsDir, const char* roomName);

// Load the foreground character skeleton (e.g. "arona_spr"). Optional.
// Returns 0 on success, non-zero if the skeleton is absent or unreadable.
int sb_load_character(const char* assetsDir, const char* characterName);

// Re-solve the foreground camera on the settled animated pose (~2 s of
// mixes and physics). Call once after the character's animation is chosen;
// the framing sb_load_character did is a fallback on the setup pose, which
// for this skeleton is not a standing figure and magnifies the live pose.
int sb_character_reframe(void);

// Play an animation by name on a skeleton. Returns 0 if it exists (spine-cpp
// aborts on unknown names; this refuses instead -- logon-screen rule).
int sb_room_play(int track, const char* animation, int loop);
int sb_character_play(int track, const char* animation, int loop);

// Number of animations in a skeleton, and name of one (borrowed pointer).
int sb_room_animation_count(void);
int sb_character_animation_count(void);
const char* sb_room_animation_name(int index);
const char* sb_character_animation_name(int index);

// Advance the world. Call once per frame with the real delta.
void sb_tick(float dt);

// Character opacity for the A->B transition (0 = hidden, 1 = on stage).
void sb_set_character_alpha(float alpha);

// Opacity of the characters living INSIDE the room (seated figure, her
// companion). Ramp to 0 as the foreground character fades in on wake; the
// two transitions read as her stepping out of the scene.
void sb_set_room_chars_alpha(float alpha);

// ---- the wake's flash, the 1.3.0 way ---------------------------------
//
// No white silhouette: a flash is an additive glow in the figure's tint
// colour (the room characters flash white->blue, the foreground pink->
// magenta -- PlanaTint/AronaTint in fx_layer.h) accumulated over the
// figure's silhouette, run through a bloom, plus a burst of the
// FX_TEX_Triangle outlines from the silhouette's own points. The figure
// itself is drawn in full colour underneath; the glow decays over 0.5 s
// and she "resolves" out of her own light.
//
// kind: 0 = the room characters' exit flash, 1 = the foreground's
// entrance flash, 2 = the room characters' arrival flash (also the boot
// ignition). sb_fx_begin captures that figure's batch geometry and
// silhouette ONCE into a snapshot (sb_snapshot) that the sink renders
// while the flash runs.
// Two-step arming, for the exit flash: sb_fx_arm captures the figure NOW
// (intact, before the Touch reaction gets under way) without lighting the
// flash; sb_fx_begin later lights the frozen capture from t=0. Without a
// prior arm, sb_fx_begin captures and lights in one step (boot, arrival).
void sb_fx_arm(int kind);
void sb_fx_begin(int kind);
int   sb_fx_active(int kind);
float sb_fx_mag(int kind);                  // magnitude envelope, 0 when inactive
void  sb_fx_color(int kind, float out[3]);  // tint * gain * magnitude
void  sb_fx_reset(void);                    // clear all flashes + drift (next mount)

// The snapshot a flash captured, valid until the next sb_fx_begin of the
// same kind. kind 0 = the WHOLE room (the exit's dissolve image, stage
// included so the desk keeps occluding); kind 1 = the foreground; kind 2 =
// the room's CHARACTERS only -- the exit glow's silhouette, which must not
// take its alpha from the walls. Returns 1 and leaves the outputs null
// when nothing has been captured for that kind yet.
int sb_snapshot(int kind,
                const SBBatchVertex** outVertices, int* outVertexCount,
                const unsigned short** outIndices, int* outIndexCount,
                const SBBatch** outBatches, int* outBatchCount);

// Turn the drifting-particle layer on (the official Drift: pastel solid
// triangles with soft halos, tumbling). Starts when the foreground
// character begins her entrance, like upstream.
void sb_drift_start(void);

// Duration of a room animation by name, in seconds (0 if absent) -- the
// wake's reaction length comes from the Touch animations' own tracks.
float sb_animation_duration(const char* name);

// Pose and emit the frame as pixel-space triangle batches. The room renders
// in its own draw order; the foreground character renders above it, framed
// by its own camera.
// Returned pointers are valid until the next sb_tick / sb_render.
int sb_render(int width, int height,
              const SBBatchVertex** outVertices, int* outVertexCount,
              const unsigned short** outIndices, int* outIndexCount,
              const SBBatch** outBatches, int* outBatchCount);

// Atlas pages exposed as straight-alpha RGBA8 (for Metal textures).
int sb_page_count(void);
int sb_page(int index, const uint8_t** outRgba, int* outW, int* outH);

// Pages belonging to the room skeleton (the rest belong to the foreground
// character; the sink draws the in-scene clock between the two groups).
int sb_room_page_count(void);

// Load the flash-burst particle sprite (e.g. "FX_TEX_Triangle_02_1.png")
// as an extra atlas page. Optional; returns non-zero if absent (the burst
// simply stays off).
int sb_load_fx(const char* pngPath);

// Protect rects (fractions of the screen, top-left origin): room elements
// whose screen bounds intersect a protect rect are muted (alpha 0) for the
// frame, so the system clock and password field read through unoccluded.
void sb_set_protect_rects(float clockX, float clockY, float clockW, float clockH,
                          float pwdX, float pwdY, float pwdW, float pwdH);

// The room's single muting knob: comma-separated substrings matched against
// each attachment path (lowercased). An empty list mutes nothing.
void sb_set_mute_patterns(const char* csv);

void sb_shutdown(void);

#ifdef __cplusplus
}
#endif
