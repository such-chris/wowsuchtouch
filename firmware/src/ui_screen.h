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

// How a CONTROL screen draws the knob
enum class UiControlStyle : uint8_t {
    NORMAL,     // ring with the range, dot at the value
    SPRING,     // meter growing from the middle, end marks, arrow brightening with speed
    DIRECTION,  // a big arrow flashes the way you turned
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

    // CONTROL extras: hide the big number / the ring, and short hints at the edges
    // saying what each swipe does (empty = no hint)
    UiControlStyle control_style = UiControlStyle::NORMAL;
    bool hide_value = false;
    bool hide_ring = false;
    char hint_left[12] = {};
    char hint_right[12] = {};
    char hint_up[12] = {};
    char hint_down[12] = {};
};

// Status line at the top of every screen
struct UiStatus {
    bool bluetooth = false;   // connected to a Mac / iPad
    bool usb = false;         // plugged into a computer (charging)
    int8_t battery_pct = -1;  // -1 = no reading
};

// Text shown in place of the knob's number (e.g. "+0.35" from Lightroom).
// Cleared whenever a new screen is shown.
struct UiValueText {
    char text[16] = {};
};

// A short message that pops up and fades away
struct UiToast {
    char text[UI_TEXT_LEN] = {};
    uint16_t duration_ms = 1500;
};
