#pragma once

// Profiles: what the knob does in each program, described as data (JSON) instead of code.
//
//   profile  - one program or device (Mac, Lightroom, Demo...)
//     mode   - swipe left / right   (e.g. Review, Exposure, Color)
//       module - swipe up / down    (e.g. Exposure, Contrast, Highlights)
//         page - long press         (one control on the knob: its feel + what turning sends)
//
// Built-in profiles are JSON text inside the firmware (profiles.cpp). Later the Mac
// app can send more, saved to the knob's flash storage.

#include <Arduino.h>
#include <functional>
#include <vector>
#include "proto_gen/smartknob.pb.h"

// What turning the knob sends on a page
// LINK: turning is sent to the Mac (e.g. the Lightroom plugin sets a slider)
enum class TurnOutput { NONE, VOLUME, SCROLL, ARROWS, KEYS, LINK };

// A keyboard shortcut, e.g. "shift+]" or "cmd+z" (key = 0: none)
struct KeyCombo {
    uint8_t modifiers = 0;   // HID modifier bits: ctrl 0x01, shift 0x02, option 0x04, cmd 0x08
    uint8_t key = 0;         // HID key code
    bool isSet() const { return key != 0; }
};

// "p", "=", "shift+]", "cmd+z", "right"... Returns false if it doesn't recognise it.
bool parseKeyCombo(const char* text, KeyCombo& out);

// What a gesture does: a key, or moving around the profile
enum class ActionKind { NONE, KEY, NEXT_PAGE, NEXT_MODULE, PREV_MODULE, NEXT_MODE, PREV_MODE, LINK };
struct Action {
    ActionKind kind = ActionKind::NONE;   // NONE = use the gesture's default
    KeyCombo key;
    String name;    // LINK: action sent to the Mac, e.g. "reset"
    bool isSet() const { return kind != ActionKind::NONE; }
};

// One choice on a picker page (turn to choose, tap to send its key)
struct PickOption {
    String label;
    Action action;   // a key, or e.g. "link:label:Purple" for the Mac to carry out
};

struct ProfilePage {
    String label;               // shown on screen, e.g. "Volume"
    String sub;                 // optional second line, e.g. "Left / Right"
    PB_SmartKnobConfig feel = {};  // detents, end stops, spring...
    TurnOutput turn = TurnOutput::NONE;
    KeyCombo turn_cw, turn_ccw;     // turn = keys: one press per detent
    String param;                   // turn = link: what the Mac adjusts, e.g. "Exposure2012"
    float step = 0;                 // turn = link: change per detent (0 = the Mac picks)
    KeyCombo rim_cw, rim_ccw;       // sliding around the screen edge (unset = acts like turning)
    KeyCombo enter;                 // sent when you change to this page or mode
    // Gestures. Unset ones do the default: double tap = next mode, swipe up / down =
    // next / previous module, long press = next page; the rest do nothing.
    Action tap, double_tap, triple_tap, long_press, swipe_left, swipe_right, swipe_up, swipe_down;
    // A picker page: the screen lists these, turn to choose, tap sends the choice
    // and returns to the first module
    std::vector<PickOption> pick;
    // Screen: hide the number / the ring, and what each swipe does (shown at the edges)
    bool show_value = true;
    uint8_t display = 0;            // UiControlStyle: 0 normal, 1 "spring", 2 "direction"
    bool show_ring = true;
    String hint_left, hint_right, hint_up, hint_down;
};

struct ProfileModule {
    String name;
    std::vector<ProfilePage> pages;
};

struct ProfileMode {
    String name;
    // Opened automatically while the Mac reports one of these tools (e.g. Lightroom's
    // "masking"), and skipped when double tapping through modes
    std::vector<String> tools;
    String module;      // opened automatically when the Mac reports this module (e.g. "develop")
    std::vector<ProfileModule> modules;
};

struct Profile {
    String name;
    bool in_menu = true;   // false: only opened automatically (by the Mac app), not from the menu
    bool title_in_center = false;  // profile name in the middle of the screen
    std::vector<ProfileMode> modes;
};

// Read one profile from JSON text. Returns false (and fills `error`) if it can't.
bool parseProfile(const char* json, Profile& out, String& error);

// Add every built-in profile to `out`; problems are reported through `log`.
void loadBuiltinProfiles(std::vector<Profile>& out, std::function<void(const char*)> log);
