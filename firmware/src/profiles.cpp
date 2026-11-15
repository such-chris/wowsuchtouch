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
// "turn": "volume" | "scroll" | "arrows" | "none", or keys: { "cw": "=", "ccw": "-" }
// "rim":  keys for sliding around the screen edge: { "cw": "=", "ccw": "-" }
// "enter": a key sent when you change to this page or mode
// Gestures: "tap", "double_tap", "triple_tap", "long_press", "swipe_left", "swipe_right",
//   "swipe_up", "swipe_down": a key ("p", "shift+]"...) or "next_page", "next_module",
//   "prev_module", "next_mode", "prev_mode"
// "pick": [ { "label": "Red", "key": "6" }, ... ] makes a picker page
// "pick_style": "ring" (emoji round the edge) or "count" (lit up to the choice, e.g. stars);
//   options can have "icon": red, yellow, green, blue, purple, star
// "short": a short name on every page of a module puts them as buttons round the edge
// "buttons": [ { "label": "Pick", "action": "p" }, ... ] = action buttons round the edge (on a
//   page, or on a module for all its pages); label "%view%" = the other view page's name
// "pick_value": "rating" / "label" = a picker opens on the photo's current value
// Actions "module:Color" open that module; keys may be media: play, next, prev, mute
// "back" returns to the view page (Grid / Loupe); "timeout_s" does the same after idling
// "none" switches a gesture's default off; a profile-level "triple_tap" applies to all its pages
// Hints can be words or ":x:", ":check:", ":palette:", ":star:"
// "display": "spring" (speed meter) or "direction" (flashing arrow) for the control screen
// "show_value": false hides the number, "ring": false hides the ring,
// "hints": { "left": "Reject", "right": "Pick", "up": "Label", "down": "Stars" } at the edges
// "turn": { "param": "Exposure2012", "step": 0.05 } = the Mac adjusts it (Lightroom plugin)
// Actions "link:reset" etc. are carried out by the Mac
// Mode "tools": [ "masking" ] = opens by itself while that tool is in use
// Mode "module": "develop" = opens by itself when the Mac reports that module (Lightroom)
// Profile "title_in_center": true = the profile name sits in the middle, not at the top
// Profile "menu": false = not listed in the menu (program profiles the Mac app opens)

#if SK_BLE
static const char MAC_PROFILE[] = R"JSON(
{
  "name": "Mac",
  "mode_toasts": false,
  "modes": [
    { "name": "Volume", "modules": [ { "name": "Volume", "pages": [
      { "label": "Volume", "turn": "volume", "swipe_left": "next_mode", "swipe_right": "prev_mode",
        "tap": "play", "swipe_up": "next", "swipe_down": "prev", "long_press": "mute",
        "feel": { "min": 0, "max": 64, "start": 32, "step_deg": 5, "detent": 0.8 } } ] } ] },
    { "name": "Scroll", "modules": [ { "name": "Scroll", "pages": [
      { "label": "Scroll", "turn": "scroll", "swipe_left": "next_mode", "swipe_right": "prev_mode",
        "display": "spring",
        "feel": { "min": 0, "max": 0, "step_deg": 60 } } ] } ] },
    { "name": "Arrows", "modules": [ { "name": "Arrows", "pages": [
      { "label": "Arrows", "sub": "Left / Right", "turn": "arrows", "swipe_left": "next_mode", "swipe_right": "prev_mode",
        "display": "direction", "ring": false,
        "feel": { "step_deg": 15, "detent": 1 } } ] } ] }
  ]
}
)JSON";
#endif

#if SK_BLE
// Lightroom Classic, using its own keyboard shortcuts (no plugin yet)
static const char LIGHTROOM_PROFILE[] = R"JSON(
{
  "name": "Lightroom",
  "menu": false,
  "triple_tap": "cmd+z",
  "modes": [
    { "name": "Library", "module": "library", "modules": [
      { "name": "Library",
        "buttons": [ { "label": "Zoom", "action": "page:Thumbnails|Zoom" }, { "label": "Target", "action": "b" },
                     { "label": "Rating", "action": "page:Rating" }, { "label": "Unflag", "action": "u" },
                     { "label": "Color", "action": "page:Color" }, { "label": "%view%", "action": "view" } ],
        "pages": [
        { "label": "Grid", "enter": "g",
          "turn": { "cw": "right", "ccw": "left" },
          "swipe_left": "x", "swipe_right": "p", "show_value": false, "show_label": false, "ring": false,
          "feel": { "step_deg": 15, "detent": 1 } },
        { "label": "Loupe", "enter": "e",
          "turn": { "cw": "right", "ccw": "left" },
          "swipe_left": "x", "swipe_right": "p", "long_press": "none", "show_value": false, "show_label": false, "ring": false,
          "feel": { "step_deg": 15, "detent": 1 } },
        { "label": "Thumbnails", "turn": { "cw": "=", "ccw": "-" }, "tap": "back", "timeout_s": 15, "long_press": "none",
          "show_value": false, "feel": { "min": 0, "max": 16, "start": 8, "step_deg": 12, "detent": 0.6 } },
        { "label": "Zoom", "turn": { "cw": "cmd+=", "ccw": "cmd+-" }, "tap": "back", "timeout_s": 15, "long_press": "none",
          "show_value": false, "feel": { "min": 0, "max": 16, "start": 8, "step_deg": 12, "detent": 0.6 } },
        { "label": "Rating", "pick_style": "count", "pick_value": "rating", "timeout_s": 30, "pick": [
          { "label": "No stars", "key": "0" }, { "label": "1 star", "key": "1", "icon": "star" },
          { "label": "2 stars", "key": "2", "icon": "star" }, { "label": "3 stars", "key": "3", "icon": "star" },
          { "label": "4 stars", "key": "4", "icon": "star" }, { "label": "5 stars", "key": "5", "icon": "star" } ] },
        { "label": "Color", "pick_style": "ring", "pick_value": "label", "timeout_s": 30, "pick": [
          { "label": "Red", "key": "6", "icon": "red" }, { "label": "Yellow", "key": "7", "icon": "yellow" },
          { "label": "Green", "key": "8", "icon": "green" }, { "label": "Blue", "key": "9", "icon": "blue" },
          { "label": "Purple", "key": "link:label:Purple", "icon": "purple" } ] } ] } ] },
    { "name": "Develop", "module": "develop", "modules": [
      { "name": "Exposure", "pages": [
          { "label": "Exposure", "short": "Exposure", "enter": "d", "turn": { "param": "Exposure2012" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Whites", "short": "Whites", "enter": "d", "turn": { "param": "Whites2012" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Blacks", "short": "Blacks", "enter": "d", "turn": { "param": "Blacks2012" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Contrast", "short": "Contrast", "enter": "d", "turn": { "param": "Contrast2012" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Shadows", "short": "Shadows", "enter": "d", "turn": { "param": "Shadows2012" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Highlights", "short": "Highlights", "enter": "d", "turn": { "param": "Highlights2012" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } } ] },
      { "name": "Presence", "pages": [
          { "label": "Texture", "short": "Texture", "enter": "d", "turn": { "param": "Texture" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Clarity", "short": "Clarity", "enter": "d", "turn": { "param": "Clarity2012" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Dehaze", "short": "Dehaze", "enter": "d", "turn": { "param": "Dehaze" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Vibrance", "short": "Vibrance", "enter": "d", "turn": { "param": "Vibrance" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Saturation", "short": "Saturation", "enter": "d", "turn": { "param": "Saturation" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } } ] },
      { "name": "Color", "pages": [
          { "label": "Temp", "short": "Temp", "enter": "d", "turn": { "param": "Temperature" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } },
          { "label": "Tint", "short": "Tint", "enter": "d", "turn": { "param": "Tint" }, "long_press": "link:reset", "swipe_left": "next_mode", "swipe_right": "prev_mode",
            "feel": { "min": -100, "max": 100, "start": 0, "step_deg": 3, "detent": 0.4 } } ] } ] },
    { "name": "Brush", "tools": [ "masking", "dust", "localized" ], "modules": [ { "name": "Brush", "pages": [
      { "label": "Brush size", "swipe_left": "next_mode", "swipe_right": "prev_mode",
        "turn": { "cw": "]", "ccw": "[" },
        "feel": { "step_deg": 8, "detent": 0.6 } },
      { "label": "Feather", "swipe_left": "next_mode", "swipe_right": "prev_mode",
        "turn": { "cw": "shift+]", "ccw": "shift+[" },
        "feel": { "step_deg": 8, "detent": 0.6 } } ] } ] }
  ]
}
)JSON";

#endif

// Scott Bezek's original haptic demos, one per mode
static const char DEMO_PROFILE[] = R"JSON(
{
  "name": "Demo",
  "menu": false,
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

// ---------------- Keys ----------------

bool parseKeyCombo(const char* text, KeyCombo& out) {
    out = KeyCombo();
    String s(text);
    s.toLowerCase();
    s.trim();
    if (s.length() == 0) {
        return false;
    }
    // Modifiers come first, each followed by "+"
    int start = 0;
    while (true) {
        int plus = s.indexOf('+', start);
        if (plus < 0 || plus == (int)s.length() - 1) {
            break;
        }
        String mod = s.substring(start, plus);
        if (mod == "shift") out.modifiers |= 0x02;
        else if (mod == "ctrl" || mod == "control") out.modifiers |= 0x01;
        else if (mod == "alt" || mod == "opt" || mod == "option") out.modifiers |= 0x04;
        else if (mod == "cmd" || mod == "command") out.modifiers |= 0x08;
        else return false;
        start = plus + 1;
    }
    String k = s.substring(start);

    if (k.length() == 1) {
        char c = k[0];
        if (c >= 'a' && c <= 'z') out.key = 0x04 + (c - 'a');
        else if (c >= '1' && c <= '9') out.key = 0x1e + (c - '1');
        else if (c == '0') out.key = 0x27;
        else {
            static const char punct[] = "-=[]\\;'`,./";
            static const uint8_t codes[] = {0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
            const char* found = strchr(punct, c);
            if (found == nullptr) return false;
            out.key = codes[found - punct];
        }
        return true;
    }
    struct Media { const char* name; uint32_t code; };
    static const Media media[] = { {"play", 0x800}, {"next", 0x20}, {"prev", 0x40}, {"mute", 0x1000} };
    for (const Media& m : media) {
        if (k == m.name) {
            out.media = m.code;
            return true;
        }
    }
    struct Named { const char* name; uint8_t code; };
    static const Named named[] = {
        {"right", 0x4f}, {"left", 0x50}, {"down", 0x51}, {"up", 0x52},
        {"space", 0x2c}, {"enter", 0x28}, {"return", 0x28}, {"esc", 0x29}, {"tab", 0x2b},
        {"delete", 0x2a}, {"backspace", 0x2a},
    };
    for (const Named& n : named) {
        if (k == n.name) {
            out.key = n.code;
            return true;
        }
    }
    if (k[0] == 'f') {
        int n = k.substring(1).toInt();
        if (n >= 1 && n <= 12) {
            out.key = 0x3a + (n - 1);
            return true;
        }
    }
    return false;
}

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

// Read an optional key field; a field that's there but not a key we know is an error
static bool readKey(JsonVariantConst value, KeyCombo& out, String& error) {
    if (value.isNull()) {
        return true;
    }
    const char* text = value.as<const char*>();
    if (text == nullptr || !parseKeyCombo(text, out)) {
        error = String("unknown key '") + (text ? text : "?") + "'";
        return false;
    }
    return true;
}

// A gesture: a named move, or a key
static bool readAction(JsonVariantConst value, Action& out, String& error) {
    if (value.isNull()) {
        return true;
    }
    const char* text = value.as<const char*>();
    struct Named { const char* name; ActionKind kind; };
    static const Named named[] = {
        {"next_page", ActionKind::NEXT_PAGE}, {"next_module", ActionKind::NEXT_MODULE},
        {"prev_module", ActionKind::PREV_MODULE}, {"next_mode", ActionKind::NEXT_MODE},
        {"prev_mode", ActionKind::PREV_MODE},
    };
    for (const Named& n : named) {
        if (text != nullptr && strcmp(text, n.name) == 0) {
            out.kind = n.kind;
            return true;
        }
    }
    out.name = text ? text : "";
    if (text != nullptr && strcmp(text, "none") == 0) {
        out.kind = ActionKind::NOTHING;
        return true;
    }
    if (text != nullptr && strncmp(text, "page:", 5) == 0) {
        out.kind = ActionKind::PAGE;
        out.name = text + 5;
        return true;
    }
    if (text != nullptr && strcmp(text, "back") == 0) {
        out.kind = ActionKind::BACK;
        return true;
    }
    if (text != nullptr && strcmp(text, "view") == 0) {
        out.kind = ActionKind::VIEW;
        return true;
    }
    if (text != nullptr && strncmp(text, "module:", 7) == 0) {
        out.kind = ActionKind::MODULE;
        out.name = text + 7;
        return true;
    }
    if (text != nullptr && strncmp(text, "link:", 5) == 0) {
        out.kind = ActionKind::LINK;   // e.g. "link:reset", carried out by the Mac
        out.name = text + 5;
        return true;
    }
    out.kind = ActionKind::KEY;
    out.name = text ? text : "";   // kept for the Controls screen
    return readKey(value, out.key, error);
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
    out.in_menu = doc["menu"] | true;
    out.title_in_center = doc["title_in_center"] | false;
    out.mode_toasts = doc["mode_toasts"] | true;
    if (!readAction(doc["triple_tap"], out.triple_tap, error)) return false;
    for (JsonObjectConst m : doc["modes"].as<JsonArrayConst>()) {
        ProfileMode mode;
        mode.name = m["name"] | "";
        mode.module = m["module"] | "";
        for (JsonVariantConst tool : m["tools"].as<JsonArrayConst>()) {
            mode.tools.push_back(tool.as<const char*>());
        }
        for (JsonObjectConst mod : m["modules"].as<JsonArrayConst>()) {
            ProfileModule module;
            module.name = mod["name"] | "";
            for (JsonObjectConst o : mod["buttons"].as<JsonArrayConst>()) {
                PickOption button;
                button.label = o["label"] | "";
                if (!readAction(o["action"], button.action, error)) return false;
                module.buttons.push_back(button);
            }
            for (JsonObjectConst p : mod["pages"].as<JsonArrayConst>()) {
                ProfilePage page;
                page.label = p["label"] | "";
                page.sub = p["sub"] | "";
                if (p["turn"]["param"].is<const char*>()) {
                    page.turn = TurnOutput::LINK;
                    page.param = p["turn"]["param"].as<const char*>();
                    page.step = p["turn"]["step"] | 0.0f;
                } else if (p["turn"].is<JsonObjectConst>()) {
                    page.turn = TurnOutput::KEYS;
                    if (!readKey(p["turn"]["cw"], page.turn_cw, error)) return false;
                    if (!readKey(p["turn"]["ccw"], page.turn_ccw, error)) return false;
                } else {
                    page.turn = parseTurn(p["turn"].as<const char*>());
                }
                if (!readKey(p["rim"]["cw"], page.rim_cw, error)) return false;
                if (!readKey(p["rim"]["ccw"], page.rim_ccw, error)) return false;
                if (!readKey(p["enter"], page.enter, error)) return false;
                if (!readAction(p["tap"], page.tap, error)) return false;
                if (!readAction(p["double_tap"], page.double_tap, error)) return false;
                if (!readAction(p["triple_tap"], page.triple_tap, error)) return false;
                if (!readAction(p["long_press"], page.long_press, error)) return false;
                if (!readAction(p["swipe_left"], page.swipe_left, error)) return false;
                if (!readAction(p["swipe_right"], page.swipe_right, error)) return false;
                if (!readAction(p["swipe_up"], page.swipe_up, error)) return false;
                if (!readAction(p["swipe_down"], page.swipe_down, error)) return false;
                for (JsonObjectConst o : p["pick"].as<JsonArrayConst>()) {
                    PickOption option;
                    option.label = o["label"] | "";
                    const char* icon = o["icon"] | "";
                    static const char* const icons[] = {"", "red", "yellow", "green", "blue", "purple", "star", "palette"};
                    for (uint8_t n = 0; n < sizeof(icons) / sizeof(icons[0]); n++) {
                        if (strcmp(icon, icons[n]) == 0) option.icon = n;
                    }
                    if (!readAction(o["key"], option.action, error)) return false;
                    page.pick.push_back(option);
                }
                page.show_value = p["show_value"] | true;
                page.show_label = p["show_label"] | true;
                page.timeout_s = p["timeout_s"] | 0;
                const char* pick_style = p["pick_style"] | "";
                page.pick_style = strcmp(pick_style, "ring") == 0 ? 1 : (strcmp(pick_style, "count") == 0 ? 2 : 0);
                page.short_name = p["short"] | "";
                page.pick_value = p["pick_value"] | "";
                for (JsonObjectConst o : p["buttons"].as<JsonArrayConst>()) {
                    PickOption button;
                    button.label = o["label"] | "";
                    if (!readAction(o["action"], button.action, error)) return false;
                    page.buttons.push_back(button);
                }
                if (page.buttons.size() > 8) {
                    error = "at most 8 buttons";
                    return false;
                }
                const char* display = p["display"] | "";
                page.display = strcmp(display, "spring") == 0 ? 1 : (strcmp(display, "direction") == 0 ? 2 : 0);
                page.show_ring = p["ring"] | true;
                page.hint_left = p["hints"]["left"] | "";
                page.hint_right = p["hints"]["right"] | "";
                page.hint_up = p["hints"]["up"] | "";
                page.hint_down = p["hints"]["down"] | "";
                if (page.pick.size() > 8) {
                    error = "a picker can have at most 8 choices";
                    return false;
                }
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
        LIGHTROOM_PROFILE,
    #endif
        DEMO_PROFILE,
    };
    for (const char* json : builtins) {
        Profile profile;
        String error;
        if (parseProfile(json, profile, error)) {
            out.push_back(profile);
        } else if (log != nullptr) {
            log((String("Profile error: ") + error).c_str());  // e.g. "unknown key 'shft+]'"
        }
    }
}
