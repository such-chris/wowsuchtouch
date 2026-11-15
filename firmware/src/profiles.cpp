#include "profiles.h"

#include <ArduinoJson.h>

// ---------------- Built-in profiles ----------------
//
// "feel" fields (all optional):
//   min, max     - value range; leave out "max" for no end stops
//   start        - value when the page opens
//   step_deg     - degrees of turn per step (detent spacing)
//   detent       - click strength (0 = smooth)
//   stop         - end stop strength (1 = normal). With min = max = 0 it's a spring
//   snap         - how far past a step it snaps to the next (1.1 = normal, 0.55 = toggle)
//   detents      - list of values with clicks (only these click; "magnetic")
//   bias         - snap point bias
//   hue          - LED color (unused on the MaTouch)
// "turn": "volume" | "scroll" | "arrows" | "none"

#if SK_BLE
static const char MAC_PROFILE[] = R"JSON(
{
  "name": "Mac",
  "modes": [
    { "name": "Volume", "modules": [ { "name": "Volume", "pages": [
      { "label": "Volume", "turn": "volume",
        "feel": { "min": 0, "max": 64, "start": 32, "step_deg": 5, "detent": 0.8 } } ] } ] },
    { "name": "Scroll", "modules": [ { "name": "Scroll", "pages": [
      { "label": "Scroll", "turn": "scroll",
        "feel": { "min": 0, "max": 0, "step_deg": 60 } } ] } ] },
    { "name": "Arrows", "modules": [ { "name": "Arrows", "pages": [
      { "label": "Arrows", "sub": "Left / Right", "turn": "arrows",
        "feel": { "step_deg": 15, "detent": 1 } } ] } ] }
  ]
}
)JSON";
#endif

// Scott Bezek's original haptic demos, one per mode
static const char DEMO_PROFILE[] = R"JSON(
{
  "name": "Demo",
  "modes": [
    { "name": "Unbounded", "modules": [ { "name": "", "pages": [
      { "label": "Unbounded", "sub": "No detents", "feel": { "step_deg": 10, "hue": 200 } } ] } ] },
    { "name": "Bounded", "modules": [ { "name": "", "pages": [
      { "label": "Bounded 0-10", "sub": "No detents", "feel": { "max": 10, "step_deg": 10 } } ] } ] },
    { "name": "Multi-rev", "modules": [ { "name": "", "pages": [
      { "label": "Multi-rev", "sub": "No detents", "feel": { "max": 72, "step_deg": 10, "hue": 73 } } ] } ] },
    { "name": "On/off", "modules": [ { "name": "", "pages": [
      { "label": "On/off", "sub": "Strong detent",
        "feel": { "max": 1, "step_deg": 60, "detent": 1, "snap": 0.55, "hue": 157 } } ] } ] },
    { "name": "Return", "modules": [ { "name": "", "pages": [
      { "label": "Return-to-center",
        "feel": { "max": 0, "step_deg": 60, "detent": 0.01, "stop": 0.6, "hue": 45 } } ] } ] },
    { "name": "Fine", "modules": [ { "name": "", "pages": [
      { "label": "Fine values", "sub": "No detents",
        "feel": { "max": 255, "start": 127, "step_deg": 1, "hue": 219 } } ] } ] },
    { "name": "Fine detents", "modules": [ { "name": "", "pages": [
      { "label": "Fine values", "sub": "With detents",
        "feel": { "max": 255, "start": 127, "step_deg": 1, "detent": 1, "hue": 25 } } ] } ] },
    { "name": "Coarse strong", "modules": [ { "name": "", "pages": [
      { "label": "Coarse values", "sub": "Strong detents",
        "feel": { "max": 31, "step_deg": 8.225806452, "detent": 2, "hue": 200 } } ] } ] },
    { "name": "Coarse weak", "modules": [ { "name": "", "pages": [
      { "label": "Coarse values", "sub": "Weak detents",
        "feel": { "max": 31, "step_deg": 8.225806452, "detent": 0.2 } } ] } ] },
    { "name": "Magnetic", "modules": [ { "name": "", "pages": [
      { "label": "Magnetic detents",
        "feel": { "max": 31, "step_deg": 7, "detent": 2.5, "snap": 0.7, "detents": [2, 10, 21, 22], "hue": 73 } } ] } ] },
    { "name": "Return detents", "modules": [ { "name": "", "pages": [
      { "label": "Return-to-center", "sub": "with detents",
        "feel": { "min": -6, "max": 6, "step_deg": 60, "detent": 1, "snap": 0.55, "bias": 0.4, "hue": 157 } } ] } ] }
  ]
}
)JSON";

// ---------------- Reading JSON ----------------

static TurnOutput parseTurn(const char* s) {
    if (s == nullptr) return TurnOutput::NONE;
    if (strcmp(s, "volume") == 0) return TurnOutput::VOLUME;
    if (strcmp(s, "scroll") == 0) return TurnOutput::SCROLL;
    if (strcmp(s, "arrows") == 0) return TurnOutput::ARROWS;
    return TurnOutput::NONE;
}

static PB_SmartKnobConfig parseFeel(JsonObjectConst feel, const String& text) {
    PB_SmartKnobConfig c = {};
    c.min_position = feel["min"] | 0;
    c.max_position = feel["max"].isNull() ? c.min_position - 1 : (int32_t)(feel["max"] | 0);  // max < min = no end stops
    c.position = feel["start"] | c.min_position;
    c.position_width_radians = (feel["step_deg"] | 10.0f) * PI / 180;
    c.detent_strength_unit = feel["detent"] | 0.0f;
    c.endstop_strength_unit = feel["stop"] | 1.0f;
    c.snap_point = feel["snap"] | 1.1f;
    c.snap_point_bias = feel["bias"] | 0.0f;
    c.led_hue = feel["hue"] | 0;
    JsonArrayConst detents = feel["detents"].as<JsonArrayConst>();
    for (JsonVariantConst d : detents) {
        if (c.detent_positions_count >= sizeof(c.detent_positions) / sizeof(c.detent_positions[0])) break;
        c.detent_positions[c.detent_positions_count++] = d.as<int32_t>();
    }
    // The motor reports this text back; it also tells us when a new page has taken effect
    strncpy(c.text, text.c_str(), sizeof(c.text) - 1);
    return c;
}

bool parseProfile(const char* json, Profile& out, String& error) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        error = String("JSON: ") + err.c_str();
        return false;
    }
    out = Profile();
    out.name = doc["name"] | "";
    if (out.name.length() == 0) {
        error = "no name";
        return false;
    }
    for (JsonObjectConst m : doc["modes"].as<JsonArrayConst>()) {
        ProfileMode mode;
        mode.name = m["name"] | "";
        for (JsonObjectConst mod : m["modules"].as<JsonArrayConst>()) {
            ProfileModule module;
            module.name = mod["name"] | "";
            for (JsonObjectConst p : mod["pages"].as<JsonArrayConst>()) {
                ProfilePage page;
                page.label = p["label"] | "";
                page.sub = p["sub"] | "";
                page.turn = parseTurn(p["turn"].as<const char*>());
                String text = page.sub.length() > 0 ? page.label + "\n" + page.sub : page.label;
                page.feel = parseFeel(p["feel"].as<JsonObjectConst>(), text);
                module.pages.push_back(page);
            }
            if (!module.pages.empty()) mode.modules.push_back(module);
        }
        if (!mode.modules.empty()) out.modes.push_back(mode);
    }
    if (out.modes.empty()) {
        error = out.name + ": no pages";
        return false;
    }
    return true;
}

void loadBuiltinProfiles(std::vector<Profile>& out, std::function<void(const char*)> log) {
    const char* builtins[] = {
    #if SK_BLE
        MAC_PROFILE,
    #endif
        DEMO_PROFILE,
    };
    for (const char* json : builtins) {
        Profile profile;
        String error;
        if (parseProfile(json, profile, error)) {
            out.push_back(profile);
        } else if (log != nullptr) {
            log((String("Profile error: ") + error).c_str());
        }
    }
}
