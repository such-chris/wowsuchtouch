#pragma once

#include <stdint.h>

enum class TouchGesture {
    NONE,
    TAP,
    DOUBLE_TAP,
    LONG_PRESS,
    SWIPE_LEFT,
    SWIPE_RIGHT,
    SWIPE_UP,
    SWIPE_DOWN,
};

const char* touchGestureName(TouchGesture gesture);

// Turns raw touch readings ("finger down? where?") into gestures.
// Pure logic with no hardware, so it can be tested on a computer.
// Call update() every few milliseconds with the latest reading.
class TouchGestures {
    public:
        // Tuning (milliseconds and screen pixels)
        uint32_t tap_max_ms = 300;          // touches longer than this aren't taps
        uint32_t double_tap_window_ms = 250; // wait this long after a tap for a second one
        uint32_t long_press_ms = 600;       // hold still this long = long press
        int16_t move_tolerance_px = 15;     // wiggle allowed for taps and long presses
        int16_t swipe_min_px = 50;          // travel needed to count as a swipe

        // Returns the gesture recognized on this call, or NONE.
        TouchGesture update(bool touching, uint16_t x, uint16_t y, uint32_t now_ms);

        // Where the last recognized gesture started.
        uint16_t gestureX() const { return start_x_; }
        uint16_t gestureY() const { return start_y_; }

    private:
        bool down_ = false;         // finger currently on the screen
        bool long_fired_ = false;   // long press already reported for this touch
        bool tap_pending_ = false;  // a tap happened; waiting to see if a second follows
        uint32_t down_ms_ = 0;      // when the finger landed
        uint32_t tap_up_ms_ = 0;    // when the pending tap's finger lifted
        uint16_t start_x_ = 0;
        uint16_t start_y_ = 0;
        uint16_t last_x_ = 0;
        uint16_t last_y_ = 0;
};
