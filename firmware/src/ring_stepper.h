#pragma once

#include <stdint.h>

struct RingStepResult {
    int32_t position;  // value after this step
    bool moved;        // value changed
    bool hit_end;      // just ran into the mode's min or max
};

// Turns rim-slide travel into knob value changes using the current mode's
// detent spacing and limits, so the rim behaves like turning the knob.
// Pure logic with no hardware, so it can be tested on a computer.
class RingStepper {
    public:
        // Call when a slide starts, with the knob's current value.
        void start(int32_t position);

        // Feed travel since the last call (radians, + = clockwise = value up).
        // width = the mode's detent spacing; min > max means "no limits".
        RingStepResult step(float delta_radians, float width_radians, int32_t min_position, int32_t max_position);

    private:
        int32_t position_ = 0;
        float leftover_radians_ = 0;  // travel not yet big enough for a whole step
        bool at_end_ = false;         // already bumped into a limit
};
