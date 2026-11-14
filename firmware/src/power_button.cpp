#include "power_button.h"

ButtonEvent PowerButton::update(bool pressed, uint32_t now_ms) {
    // Debounce: only accept a change once it has held steady for debounce_ms
    if (pressed != raw_pressed_) {
        raw_pressed_ = pressed;
        raw_change_ms_ = now_ms;
    }
    bool settled = now_ms - raw_change_ms_ >= debounce_ms;

    if (!armed_) {
        // Arm once the button has been steadily released (the power-on press is over)
        if (settled && !raw_pressed_) {
            armed_ = true;
            stable_pressed_ = false;
        }
        return ButtonEvent::NONE;
    }

    if (!settled || raw_pressed_ == stable_pressed_) {
        // No new stable change; check for a long press while held
        if (stable_pressed_ && !long_fired_ && now_ms - down_ms_ >= long_press_ms) {
            long_fired_ = true;
            return ButtonEvent::LONG_PRESS;
        }
        return ButtonEvent::NONE;
    }

    stable_pressed_ = raw_pressed_;

    if (stable_pressed_) {
        down_ms_ = now_ms;
        long_fired_ = false;
        return ButtonEvent::NONE;
    }

    // Released
    return long_fired_ ? ButtonEvent::NONE : ButtonEvent::SHORT_PRESS;
}
