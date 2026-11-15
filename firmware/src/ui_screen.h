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

// Little pictures (emoji) that lists and hints can show
enum class UiIcon : uint8_t { NONE, RED, YELLOW, GREEN, BLUE, PURPLE, STAR, PALETTE };

// How a LIST draws its items
enum class UiListStyle : uint8_t {
    DRUM,        // the turning drum (menus)
    RING,        // icons around the top edge; the chosen one is bright
    RING_COUNT,  // same, but everything up to the chosen one is lit (star rating)
};

// Where things sit on the ring (LVGL degrees: 0 = right, 90 = bottom, 270 = top)
static inline float uiPickerAngle(int i, int count) {    // spread across the top
    return 270 + (i - (count - 1) / 2.0f) * 32;
}
static inline float uiButtonAngle(int i, int count) {    // evenly round, first at the top
    return 270 + i * 360.0f / count;
}
static const int UI_PICKER_RADIUS = 86;
static const int UI_PICKER_RADIUS_INNER = 58;   // when buttons take the edge
static const int UI_BUTTON_RADIUS = 90;    // the words follow the edge here, inside the ring
static const int UI_BUTTON_LINE_FROM = 66; // dividing lines between buttons run from here...
static const int UI_BUTTON_LINE_TO = 102;  // ...to just inside the ring
static const int UI_SMALL_RING_RADIUS = 60; // the value ring when buttons take the edge

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
    UiListStyle list_style = UiListStyle::DRUM;
    UiIcon item_icons[UI_MAX_ITEMS] = {};

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
    char hint_left[16] = {};
    char hint_right[16] = {};
    char hint_up[16] = {};
    char hint_down[16] = {};
    // A hint is a word, or ":x:" (grey cross), ":check:" (white tick), ":palette:", ":star:",
    // optionally followed by a word: ":x: Reject"

    // CONTROL: buttons around the edge (e.g. Lightroom sliders); tap one to choose it
    uint8_t button_count = 0;
    uint8_t button_selected = 0;
    char buttons[UI_MAX_ITEMS][12] = {};
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
    bool sub = false;   // true: a small line under the page name instead of the big value
};

// A short message that pops up and fades away
struct UiToast {
    char text[UI_TEXT_LEN] = {};
    uint16_t duration_ms = 1500;
};
