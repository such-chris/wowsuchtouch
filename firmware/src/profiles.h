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
enum class TurnOutput { NONE, VOLUME, SCROLL, ARROWS };

struct ProfilePage {
    String label;               // shown on screen, e.g. "Volume"
    String sub;                 // optional second line, e.g. "Left / Right"
    PB_SmartKnobConfig feel = {};  // detents, end stops, spring...
    TurnOutput turn = TurnOutput::NONE;
};

struct ProfileModule {
    String name;
    std::vector<ProfilePage> pages;
};

struct ProfileMode {
    String name;
    std::vector<ProfileModule> modules;
};

struct Profile {
    String name;
    std::vector<ProfileMode> modes;
};

// Read one profile from JSON text. Returns false (and fills `error`) if it can't.
bool parseProfile(const char* json, Profile& out, String& error);

// Add every built-in profile to `out`; problems are reported through `log`.
void loadBuiltinProfiles(std::vector<Profile>& out, std::function<void(const char*)> log);
