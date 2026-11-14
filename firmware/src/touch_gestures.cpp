#include "touch_gestures.h"

#include <stdlib.h>

const char* touchGestureName(TouchGesture gesture) {
    switch (gesture) {
        case TouchGesture::TAP:         return "TAP";
        case TouchGesture::DOUBLE_TAP:  return "DOUBLE_TAP";
        case TouchGesture::LONG_PRESS:  return "LONG_PRESS";
        case TouchGesture::SWIPE_LEFT:  return "SWIPE_LEFT";
        case TouchGesture::SWIPE_RIGHT: return "SWIPE_RIGHT";
        case TouchGesture::SWIPE_UP:    return "SWIPE_UP";
        case TouchGesture::SWIPE_DOWN:  return "SWIPE_DOWN";
        default:                        return "NONE";
    }
}

TouchGesture TouchGestures::update(bool touching, uint16_t x, uint16_t y, uint32_t now_ms) {
    // Finger just landed
    if (touching && !down_) {
        down_ = true;
        long_fired_ = false;
        down_ms_ = now_ms;
        start_x_ = last_x_ = x;
        start_y_ = last_y_ = y;
        return TouchGesture::NONE;
    }

    // Finger still down: check for a long press
    if (touching && down_) {
        last_x_ = x;
        last_y_ = y;
        int dx = abs((int)last_x_ - (int)start_x_);
        int dy = abs((int)last_y_ - (int)start_y_);
        if (!long_fired_ && now_ms - down_ms_ >= long_press_ms
                && dx <= move_tolerance_px && dy <= move_tolerance_px) {
            long_fired_ = true;
            tap_pending_ = false;
            return TouchGesture::LONG_PRESS;
        }
        return TouchGesture::NONE;
    }

    // Finger just lifted: decide between swipe and tap
    if (!touching && down_) {
        down_ = false;
        if (long_fired_) {
            return TouchGesture::NONE;
        }

        int dx = (int)last_x_ - (int)start_x_;
        int dy = (int)last_y_ - (int)start_y_;

        if (abs(dx) >= swipe_min_px || abs(dy) >= swipe_min_px) {
            tap_pending_ = false;
            if (abs(dx) >= abs(dy)) {
                return dx > 0 ? TouchGesture::SWIPE_RIGHT : TouchGesture::SWIPE_LEFT;
            }
            return dy > 0 ? TouchGesture::SWIPE_DOWN : TouchGesture::SWIPE_UP;
        }

        bool short_enough = now_ms - down_ms_ <= tap_max_ms;
        bool still_enough = abs(dx) <= move_tolerance_px && abs(dy) <= move_tolerance_px;
        if (short_enough && still_enough) {
            if (tap_pending_) {
                // Second tap arrived before the window ran out
                tap_pending_ = false;
                return TouchGesture::DOUBLE_TAP;
            }
            tap_pending_ = true;
            tap_up_ms_ = now_ms;
        }
        return TouchGesture::NONE;
    }

    // No finger: if a tap has waited out the double-tap window, it was a single tap
    if (tap_pending_ && now_ms - tap_up_ms_ > double_tap_window_ms) {
        tap_pending_ = false;
        return TouchGesture::TAP;
    }
    return TouchGesture::NONE;
}
