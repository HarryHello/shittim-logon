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
} SBBatch;

// ---- lifecycle ------------------------------------------------------

// Load a room skeleton (self-contained: room, seated character, props).
// Returns 0 on success. Real names, e.g. "arona_workpage_nighttime_2".
int sb_load_room(const char* assetsDir, const char* roomName);

// Load the foreground character skeleton (e.g. "arona_spr"). Optional.
// Returns 0 on success, non-zero if the skeleton is absent or unreadable.
int sb_load_character(const char* assetsDir, const char* characterName);

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

// Protect rects (fractions of the screen): scene elements whose screen
// bounds intersect a protect rect are muted (alpha 0) for the frame, so the
// system clock and password field read through unoccluded. The classroom
// backdrop spans the whole screen and is muted by the same rule.
void sb_set_protect_rects(float clockX, float clockY, float clockW, float clockH,
                          float pwdX, float pwdY, float pwdW, float pwdH);

void sb_set_room_char_only(int enabled);

void sb_set_mute_patterns(const char* csv);

void sb_shutdown(void);

#ifdef __cplusplus
}
#endif
