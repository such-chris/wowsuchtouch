#pragma once

#include <stdint.h>

enum class ButtonEvent { NONE, SHORT_PRESS, LONG_PRESS };

// Short press / long press detection for the MaTouch's Function button
// (which is also its power button).
// Pure logic with no hardware, so it can be tested on a computer.
class PowerButton {
    public:
        uint32_t long_press_ms = 2000;  // hold this long to power off
        uint32_t debounce_ms = 30;      // ignore contact bounce shorter than this

        // pressed = button currently held down. Returns an event, or NONE.
        ButtonEvent update(bool pressed, uint32_t now_ms);

    private:
        // The press that switched the knob on is usually still held while it
        // boots; ignore the button until it has been released once.
        bool armed_ = false;
        bool stable_pressed_ = false;   // debounced state
        bool raw_pressed_ = false;
        uint32_t raw_change_ms_ = 0;
        uint32_t down_ms_ = 0;
        bool long_fired_ = false;
};
