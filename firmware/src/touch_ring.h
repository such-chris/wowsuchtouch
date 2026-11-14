#pragma once

#include <stdint.h>

// Treats the outer edge of the round screen like a click wheel: tracks how far
// a finger slides around the rim.
// Pure logic with no hardware, so it can be tested on a computer.
class TouchRing {
    public:
        // Tuning (screen pixels)
        uint16_t center_x = 120;
        uint16_t center_y = 120;
        uint16_t inner_radius_px = 75;  // finger must be at least this far from center

        // Call with every touch reading. Returns degrees moved around the rim
        // since the last call: positive = clockwise, negative = counter-clockwise.
        float update(bool touching, uint16_t x, uint16_t y);

        // True while a finger is on the rim.
        bool active() const { return active_; }

        // Finger's position around the rim in degrees (0 = top, 90 = right).
        float angle() const { return last_angle_; }

        // Total degrees moved since the finger arrived on the rim (signed).
        float totalDegrees() const { return total_degrees_; }

    private:
        bool active_ = false;
        float last_angle_ = 0;
        float total_degrees_ = 0;
};
