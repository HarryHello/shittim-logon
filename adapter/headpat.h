#pragma once

#include "scene.h"

#include <spine/spine.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace headpat {

constexpr float kHeadCentreLift   = 0.161f;
constexpr float kHeadHalfWidth    = 0.356f;
constexpr float kHeadHalfHeight   = 0.403f;
constexpr float kMinimumReference = 1.0f;

constexpr float kBodyHalfWidth  = 0.40f;
constexpr float kBodyHalfHeight = 0.55f;

struct BoxFractions {
    const char *skeleton;
    float left, right, top, bottom;
};
constexpr BoxFractions kBoxes[] = {
    { "NP0035_spr", 0.3180f, 0.7310f, 0.0436f, 0.2877f },
    { "arona_spr",  0.3427f, 0.6773f, 0.0890f, 0.2811f },
};

inline const BoxFractions *BoxFor(const char *skeleton) {
    if (!skeleton) return nullptr;
    for (const BoxFractions &b : kBoxes)
        if (std::strcmp(b.skeleton, skeleton) == 0) return &b;
    return nullptr;
}

constexpr float kClamp = 80.0f;

constexpr float kTravelGain = 0.35f;

constexpr float kPatTau     = 0.038682f;
constexpr float kReleaseTau = 0.130374f;

constexpr int kTrackM = 1;
constexpr int kTrackA = 2;

constexpr float kReleaseMix = 0.5f;

struct Box {
    float left = 0, bottom = 0, right = 0, top = 0;
    bool contains(float x, float y) const {
        return x >= left && x <= right && y >= bottom && y <= top;
    }
    bool valid() const { return right > left && top > bottom; }
};

struct Rig {

    void Attach(spine::Skeleton *skeleton, spine::SkeletonData *skeletonData,
                spine::AnimationState *animationState,
                void (*logfn)(const wchar_t *, ...) = nullptr) {
        log = logfn;
        sk = skeleton;
        data = skeletonData;
        state = animationState;
        point = head = hip = nullptr;
        if (!sk) return;
        point = sk->findBone("Touch_Point");
        head  = sk->findBone("Head_Rot");
        hip   = sk->findBone("Hip");
        if (point) { pointBaseX = point->getX(); pointBaseY = point->getY(); }
        if (log) {
            if (ready())
                log(L"headpat: bound Touch_Point, Head_Rot and Hip");
            else
                log(L"headpat: unavailable -- Touch_Point %s, Head_Rot %s, Hip %s",
                    point ? L"ok" : L"MISSING", head ? L"ok" : L"MISSING",
                    hip ? L"ok" : L"MISSING");
        }
    }

    bool ready() const { return point && head && hip; }

    float reference() const {
        if (!head || !hip) return 0.0f;
        const float dx = head->getWorldX() - hip->getWorldX();
        const float dy = head->getWorldY() - hip->getWorldY();
        const float d = std::sqrt(dx * dx + dy * dy);
        return d >= kMinimumReference ? d : 0.0f;
    }

    Box HeadBox() const {
        Box b;
        const float ref = reference();
        if (!head || ref <= 0.0f) return b;

        const float cx = head->getWorldX();
        const float cy = head->getWorldY() + kHeadCentreLift * ref;
        const float halfW = kHeadHalfWidth * ref;
        const float halfH = kHeadHalfHeight * ref;
        b.left   = cx - halfW; b.right = cx + halfW;
        b.bottom = cy - halfH; b.top   = cy + halfH;
        return b;
    }

    bool IsOverHead(float worldX, float worldY) const {
        const Box b = HeadBox();
        return b.valid() && b.contains(worldX, worldY);
    }

    Box BodyBox() const {
        Box b;
        const float ref = reference();
        if (!hip || ref <= 0.0f) return b;
        const float cx = hip->getWorldX();
        const float cy = hip->getWorldY();
        b.left   = cx - kBodyHalfWidth * ref;
        b.right  = cx + kBodyHalfWidth * ref;
        b.bottom = cy - kBodyHalfHeight * ref;
        b.top    = cy + kBodyHalfHeight * ref;
        return b;
    }

    bool IsOverBody(float worldX, float worldY) const {
        const Box b = BodyBox();
        return b.valid() && b.contains(worldX, worldY);
    }

    bool Waist(float *x, float *y) const {
        if (!ready() || !x || !y) return false;
        *x = hip->getWorldX();
        *y = hip->getWorldY();
        return true;
    }

    bool patting() const { return isPatting; }

    void Begin(float worldX) {
        if (!ready() || isPatting) return;
        isPatting = true;
        lastX = worldX;
        haveLast = true;
        Play("Pat_01_M", "Pat_01_A");
        if (log) log(L"headpat: began at world x %.0f", worldX);
    }

    void Continue(float worldX) {
        if (!isPatting || !haveLast) return;

        const float delta = (lastX - worldX) * kTravelGain;
        lastX = worldX;
        targetOffset = std::min(std::max(targetOffset + delta, -kClamp), kClamp);
    }

    void End() {
        if (!isPatting) return;
        isPatting = false;
        haveLast = false;
        targetOffset = 0.0f;
        Release("PatEnd_01_M", "PatEnd_01_A");
        if (log) log(L"headpat: ended");
    }

    void SetExpression(const char *name) {
        if (!name || !*name) return;
        if (std::strncmp(expr, name, sizeof(expr) - 1) == 0) return;
        std::strncpy(expr, name, sizeof(expr) - 1);
        expr[sizeof(expr) - 1] = '\0';
        haveExpr = true;
        if (isPatting || !state || !data) return;
        if (data->findAnimation(expr)) state->setAnimation(kTrackM, expr, true);
        else if (log) log(L"headpat: expression '%hs' is not in this skeleton", expr);
    }

    const char *expression() const { return haveExpr ? expr : nullptr; }

    void Cancel() {
        isPatting = false;
        haveLast = false;
        targetOffset = 0.0f;
        offset = 0.0f;
        if (state) {
            state->setEmptyAnimation(kTrackM, 0.0f);
            state->setEmptyAnimation(kTrackA, 0.0f);
        }
        if (point) { point->setX(pointBaseX); point->setY(pointBaseY); }
    }

    void Apply(float dt) {
        if (!ready()) return;
        const float tau = isPatting ? kPatTau : kReleaseTau;

        const float k = 1.0f - std::exp(-std::min(dt, 0.1f) / tau);
        offset += (targetOffset - offset) * k;
        point->setX(pointBaseX);
        point->setY(pointBaseY + offset);
    }

    float currentOffset() const { return offset; }

private:

    void Play(const char *m, const char *a) {
        if (!state || !data) return;
        if (spine::TrackEntry *e = Set(kTrackM, m)) e->setMixDuration(0.0f);
        if (spine::TrackEntry *e = Set(kTrackA, a)) e->setMixDuration(0.0f);
    }

    void Release(const char *m, const char *a) {
        if (!state || !data) return;
        if (spine::TrackEntry *e = Set(kTrackM, m)) e->setMixDuration(0.0f);
        if (spine::TrackEntry *e = Set(kTrackA, a)) e->setMixDuration(0.0f);
        state->addEmptyAnimation(kTrackM, kReleaseMix, 0.0f);
        state->addEmptyAnimation(kTrackA, kReleaseMix, 0.0f);

        if (haveExpr && data->findAnimation(expr))
            state->addAnimation(kTrackM, expr, true, 0.0f);
    }

    spine::TrackEntry *Set(int track, const char *name) {
        if (!data->findAnimation(name)) {
            if (log) log(L"headpat: '%hs' is not in this skeleton -- skipped", name);
            return nullptr;
        }
        return state->setAnimation(track, name, false);
    }

    spine::Skeleton      *sk    = nullptr;
    spine::SkeletonData  *data  = nullptr;
    spine::AnimationState *state = nullptr;
    spine::Bone *point = nullptr, *head = nullptr, *hip = nullptr;
    float pointBaseX = 0.0f, pointBaseY = 0.0f;

    char  expr[8] = { 0 };
    bool  haveExpr = false;

    bool  isPatting = false;
    bool  haveLast  = false;
    float lastX = 0.0f;
    float targetOffset = 0.0f;
    float offset = 0.0f;
    void (*log)(const wchar_t *, ...) = nullptr;
};

inline void ScreenToWorld(const scene::Viewport &vp, int W, int H,
                          float px, float py, float *wx, float *wy) {
    if (W <= 0 || H <= 0 || vp.width <= 0.0f || vp.height <= 0.0f) {
        *wx = *wy = 0.0f;
        return;
    }
    *wx = vp.left + px * vp.width / (float)W;
    *wy = (vp.bottom + vp.height) - py * vp.height / (float)H;
}

inline void WorldToScreen(const scene::Viewport &vp, int W, int H,
                          float wx, float wy, float *px, float *py) {
    if (W <= 0 || H <= 0 || vp.width <= 0.0f || vp.height <= 0.0f) {
        *px = *py = 0.0f;
        return;
    }
    *px = (wx - vp.left) * (float)W / vp.width;
    *py = ((vp.bottom + vp.height) - wy) * (float)H / vp.height;
}

}
