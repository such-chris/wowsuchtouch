#pragma once

#include <stdint.h>

// What the screen should show. The interface task (logic) fills this in and
// sends it to the display task (drawing), which lays it out with LVGL.
// The knob's current position picks the highlighted item / value.

enum class UiLayout : uint8_t {
    CONTROL,  // big value + mode name (apps like Volume, Scroll)
    LIST,     // menu: one item per knob click
    VALUE,    // a setting: big number, or two lines of info
};

static const uint8_t UI_MAX_ITEMS = 8;
static const uint8_t UI_TEXT_LEN = 24;

struct UiScreen {
    UiLayout layout = UiLayout::CONTROL;
    char title[UI_TEXT_LEN] = {};

    // LIST: one label per knob position (0 = first)
    uint8_t item_count = 0;
    bool wrap = false;  // turning past the last item comes back to the first

    // Where the position ring sits (true = along the bottom, false = along the top)
    bool ring_at_bottom = false;
    // Show the Bluetooth / battery status line at the top
    bool show_status = false;
    char items[UI_MAX_ITEMS][UI_TEXT_LEN] = {};

    // VALUE: shows (knob position x multiplier) + suffix, or line1/line2 if show_number is false
    bool show_number = false;
    int16_t number_multiplier = 1;
    char suffix[6] = {};
    char line1[UI_TEXT_LEN] = {};
    char line2[UI_TEXT_LEN] = {};
};

// Status line at the top of every screen
struct UiStatus {
    bool bluetooth = false;   // connected to a Mac / iPad
    bool usb = false;         // plugged into a computer (charging)
    int8_t battery_pct = -1;  // -1 = no reading
};

// A short message that pops up and fades away
struct UiToast {
    char text[UI_TEXT_LEN] = {};
    uint16_t duration_ms = 1500;
};
