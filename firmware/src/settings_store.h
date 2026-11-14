#pragma once

#include <stdint.h>

// Settings saved on the knob so they survive restarts.
struct KnobSettings {
    uint8_t brightness_pct = 100;  // screen brightness, 10-100
    uint8_t haptics_pct = 100;     // click / end-stop strength, 20-200
    uint8_t touch_mode = 0;        // rim slide: 0 = track, 1 = follow, 2 = off
    int32_t volume = 32;           // last Volume level (0-64)
    uint8_t app = 0;               // last app opened
};

// Reads and writes KnobSettings in the ESP32's built-in key-value storage
// (the "Preferences" library, which lives in a small area of flash).
class SettingsStore {
    public:
        KnobSettings load();
        void save(const KnobSettings& settings);
};
