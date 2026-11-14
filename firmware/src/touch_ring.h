#pragma once

#include <stdint.h>

// Treats the outer edge of the round screen like a click wheel: sliding a
// finger around the rim produces clockwise / counter-clockwise steps.
// Pure logic with no hardware, so it can be tested on a computer.
class TouchRing {
    public:
        // Tuning (screen pixels and degrees)
        uint16_t center_x = 120;
        uint16_t center_y = 120;
        uint16_t inner_radius_px = 75;  // finger must be at least this far from center
        float step_degrees = 15;        // one step per this much travel around the rim

        // Call with every touch reading. Returns steps moved since the last
        // call: positive = clockwise, negative = counter-clockwise.
        int update(bool touching, uint16_t x, uint16_t y);

        // True while a finger is on the rim.
        bool active() const { return active_; }

        // Finger's position around the rim in degrees (0 = top, 90 = right).
        float angle() const { return last_angle_; }

    private:
        bool active_ = false;
        float last_angle_ = 0;
        float leftover_degrees_ = 0;  // travel not yet big enough for a whole step
};
