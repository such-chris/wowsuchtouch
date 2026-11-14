#include "settings_store.h"

#include <Preferences.h>

static const char* NAMESPACE = "knob";

KnobSettings SettingsStore::load() {
    KnobSettings defaults;
    KnobSettings settings;
    Preferences prefs;
    prefs.begin(NAMESPACE, true);  // true = read-only
    settings.brightness_pct = prefs.getUChar("bright", defaults.brightness_pct);
    settings.haptics_pct = prefs.getUChar("haptics", defaults.haptics_pct);
    settings.touch_mode = prefs.getUChar("touch", defaults.touch_mode);
    settings.volume = prefs.getInt("volume", defaults.volume);
    settings.app = prefs.getUChar("app", defaults.app);
    settings.auto_off = prefs.getUChar("autooff", defaults.auto_off);
    prefs.end();
    return settings;
}

void SettingsStore::save(const KnobSettings& settings) {
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    // Saved only on menu exits and power-off, so flash writes stay rare
    prefs.putUChar("bright", settings.brightness_pct);
    prefs.putUChar("haptics", settings.haptics_pct);
    prefs.putUChar("touch", settings.touch_mode);
    prefs.putInt("volume", settings.volume);
    prefs.putUChar("app", settings.app);
    prefs.putUChar("autooff", settings.auto_off);
    prefs.end();
}
