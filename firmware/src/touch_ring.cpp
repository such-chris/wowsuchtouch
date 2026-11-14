#include "touch_ring.h"

#include <math.h>

int TouchRing::update(bool touching, uint16_t x, uint16_t y) {
    float dx = (float)x - center_x;
    float dy = (float)y - center_y;
    float radius = sqrtf(dx * dx + dy * dy);

    if (!touching || radius < inner_radius_px) {
        active_ = false;
        return 0;
    }

    // Angle measured clockwise from the top (screen y grows downward)
    float angle = atan2f(dx, -dy) * 180.0f / (float)M_PI;
    if (angle < 0) {
        angle += 360;
    }

    if (!active_) {
        // Finger just arrived on the rim: start measuring from here
        active_ = true;
        last_angle_ = angle;
        leftover_degrees_ = 0;
        return 0;
    }

    // Change since last reading, taking the short way around (e.g. 350 -> 10 is +20)
    float delta = angle - last_angle_;
    if (delta > 180) {
        delta -= 360;
    } else if (delta < -180) {
        delta += 360;
    }
    last_angle_ = angle;

    leftover_degrees_ += delta;
    int steps = (int)(leftover_degrees_ / step_degrees);  // rounds toward zero
    leftover_degrees_ -= steps * step_degrees;
    return steps;
}
