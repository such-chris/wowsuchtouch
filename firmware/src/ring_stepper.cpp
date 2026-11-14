#include "ring_stepper.h"

void RingStepper::start(int32_t position) {
    position_ = position;
    leftover_radians_ = 0;
    at_end_ = false;
}

RingStepResult RingStepper::step(float delta_radians, float width_radians, int32_t min_position, int32_t max_position) {
    RingStepResult result = {position_, false, false};
    if (width_radians <= 0) {
        return result;
    }

    leftover_radians_ += delta_radians;
    int32_t steps = (int32_t)(leftover_radians_ / width_radians);  // rounds toward zero
    leftover_radians_ -= steps * width_radians;

    int32_t target = position_ + steps;
    bool bounded = min_position <= max_position;
    bool clamped = false;
    if (bounded && target > max_position) {
        target = max_position;
        clamped = true;
    } else if (bounded && target < min_position) {
        target = min_position;
        clamped = true;
    }

    if (clamped) {
        // Pushing past a limit: bump once, and don't store up travel past the end
        leftover_radians_ = 0;
        result.hit_end = !at_end_;
        at_end_ = true;
    } else if (steps != 0) {
        at_end_ = false;
    }

    result.moved = target != position_;
    result.position = target;
    position_ = target;
    return result;
}
