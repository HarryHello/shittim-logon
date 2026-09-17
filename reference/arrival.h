#pragma once

namespace arrival {

struct Hold {

    float held = 0.0f;
    float delay = 0.35f;

    bool  arrived = false;

    bool onStage() const { return arrived || held >= delay; }

    bool flashDue() const { return !arrived && held >= delay; }

    void step(float dt) { held += dt; }

    void entered() { arrived = true; }

    void settle() { arrived = true; held = delay; }

    void woken() { if (!onStage()) settle(); }

    void rewind() { arrived = false; held = 0.0f; }
};

}
