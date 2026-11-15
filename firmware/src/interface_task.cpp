#if SK_LEDS
#include <FastLED.h>
#endif

#if SK_STRAIN
#include <HX711.h>
#endif

#if SK_ALS
#include <Adafruit_VEML7700.h>
#endif

#include "interface_task.h"
#include "semaphore_guard.h"
#include "util.h"
#include "version.h"
#include "profiles.h"

// Icons used in pop-ups (LVGL symbols; blank if there's no screen)
#ifndef LV_SYMBOL_BLUETOOTH
#define LV_SYMBOL_BLUETOOTH ""
#endif
#ifndef LV_SYMBOL_BATTERY_1
#define LV_SYMBOL_BATTERY_1 ""
#endif
#ifndef LV_SYMBOL_BATTERY_FULL
#define LV_SYMBOL_BATTERY_FULL ""
#endif
#ifndef LV_SYMBOL_CHARGE
#define LV_SYMBOL_CHARGE ""
#endif

#if SK_LEDS
CRGB leds[NUM_LEDS];
#endif

#if SK_STRAIN
HX711 scale;
#endif

#if SK_ALS
Adafruit_VEML7700 veml = Adafruit_VEML7700();
#endif

#if SK_BLE
static const bool BLE_FINE_VOLUME = true;  // Shift+Option quarter steps on macOS

// Spring scroll tuning (deflection is in "full deflections", i.e. 1.0 = 60 degrees)
static const float SCROLL_DEAD_ZONE = 0.08;      // ignore tiny wobbles around center
static const float SCROLL_MIN_RATE = 6;          // wheel ticks per second just past the dead zone
static const float SCROLL_EXTRA_RATE = 120;      // added at full deflection (grows with the square)
static const float RIM_SCROLL_DEGREES = 5;       // rim travel per wheel tick in Scroll mode
static const float RIM_KEY_DEGREES = 20;         // rim travel per key press on pages with rim keys
static const float BUTTON_TAP_MIN_RADIUS = 62;   // taps further out than this pick an edge button (pixels)

// Volume: pushing past an end stop by this much (in steps) re-syncs with the Mac
static const float VOLUME_RESYNC_PUSH = 0.3;
#endif

// Apps are profiles now: see profiles.cpp

// ---- Settings list ----
enum SettingItem {
    SET_BRIGHTNESS,
    SET_HAPTICS,
    SET_AUTO_OFF,
    SET_BLUETOOTH,
    SET_CALIBRATE,
    SET_BATTERY,
    SET_VERSION,
    SET_DEMO,
    SET_COUNT,
};
static const char* setting_names[SET_COUNT] = {
    "Brightness", "Click strength", "Auto off", "Bluetooth", "Calibrate", "Battery", "Version", "Demo",
};
static const char* auto_off_names[] = {"5 min", "15 min", "30 min", "Never"};
static const uint32_t auto_off_minutes[] = {5, 15, 30, 0};

// Sleep: dim after a short pause, then screen off with the motor relaxed
static const uint32_t SLEEP_DIM_MS = 30000;
static const uint32_t SLEEP_SCREEN_OFF_MS = 120000;
static const uint8_t SLEEP_DIM_PCT = 25;          // dimmed brightness, as a % of the normal setting
static const float WAKE_TURN_DEGREES = 1.5f;      // turning this far counts as activity

// A screen inside Bluetooth (not in the Settings list itself)
static const int SET_BT_DEVICES = SET_COUNT;

// How long "Add device" lets new devices pair
static const uint32_t PAIRING_WINDOW_MS = 120000;

// Main menu: the apps, then these
enum MenuExtra { MENU_CONTROLS = 0, MENU_SETTINGS, MENU_SLEEP, MENU_POWER_OFF, MENU_EXTRA_COUNT };
static const char* const menu_extra_names[MENU_EXTRA_COUNT] = { "Controls", "Settings", "Sleep", "Power off" };

// A screen inside the menu: what the gestures do on the current page
static const int SET_CONTROLS = 1000;

// ---- Knob feel for menus ----
// A list: one click per item. Looping lists have no end stops (past the last item
// comes back to the first); the others stop at the first and last item.
static PB_SmartKnobConfig makeListConfig(const char* id, int count, int selected, uint8_t nonce, bool loop = false) {
    PB_SmartKnobConfig config = {};
    config.position = selected;
    config.position_nonce = nonce;
    config.min_position = 0;
    config.max_position = loop ? -1 : count - 1;  // max below min = no end stops
    config.position_width_radians = 24 * PI / 180;
    config.detent_strength_unit = 0.6;
    config.endstop_strength_unit = 1;
    config.snap_point = 1.1;
    strncpy(config.text, id, sizeof(config.text) - 1);
    return config;
}

// A slider: light clicks between min and max
static PB_SmartKnobConfig makeSliderConfig(const char* id, int min, int max, int value, uint8_t nonce) {
    PB_SmartKnobConfig config = makeListConfig(id, max - min + 1, value, nonce);
    config.min_position = min;
    config.max_position = max;
    config.position = CLAMP(value, min, max);
    config.position_width_radians = 10 * PI / 180;
    config.detent_strength_unit = 0.4;
    return config;
}

// Nothing to turn: the knob is gently held in place
static PB_SmartKnobConfig makeStillConfig(const char* id, uint8_t nonce) {
    PB_SmartKnobConfig config = makeListConfig(id, 1, 0, nonce);
    config.endstop_strength_unit = 0.5;
    return config;
}

static void copyText(char* dest, const char* src) {
    strncpy(dest, src, UI_TEXT_LEN - 1);
    dest[UI_TEXT_LEN - 1] = 0;
}

InterfaceTask::InterfaceTask(const uint8_t task_core, MotorTask& motor_task, DisplayTask* display_task) : 
        // Bluetooth sends happen on this task, so give it more stack room when enabled
        Task("Interface", SK_BLE ? 6000 : 3400, 1, task_core),
        stream_(),
        motor_task_(motor_task),
        display_task_(display_task),
        plaintext_protocol_(stream_, [this] () {
            motor_task_.runCalibration();
        }),
        proto_protocol_(stream_, [this] (PB_SmartKnobConfig& config) {
            applyConfig(config, true);
        }) {
    #if SK_DISPLAY
        assert(display_task != nullptr);
    #endif


    log_queue_ = xQueueCreate(10, sizeof(std::string *));
    assert(log_queue_ != NULL);

    knob_state_queue_ = xQueueCreate(1, sizeof(PB_SmartKnobState));
    assert(knob_state_queue_ != NULL);

    mutex_ = xSemaphoreCreateMutex();
    assert(mutex_ != NULL);
}

InterfaceTask::~InterfaceTask() {
    vSemaphoreDelete(mutex_);
}

void InterfaceTask::run() {
    stream_.begin();
    
    #if SK_LEDS
        FastLED.addLeds<SK6812, PIN_LED_DATA, GRB>(leds, NUM_LEDS);
    #endif

    #if SK_ALS && PIN_SDA >= 0 && PIN_SCL >= 0
        Wire.begin(PIN_SDA, PIN_SCL);
        Wire.setClock(400000);
    #endif

    #if SK_TOUCH
        touch_.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    #endif

    #ifdef PIN_POWER_BUTTON
        pinMode(PIN_POWER_BUTTON, INPUT_PULLUP);
    #endif

    #if SK_BLE
    if (!charging_mode_) {
        ble_.begin("SmartKnob");
        log("Bluetooth: pair with \"SmartKnob\" in your Mac's Bluetooth settings");
    }
    #endif

    #if SK_STRAIN
        scale.begin(PIN_STRAIN_DO, PIN_STRAIN_SCK);
    #endif

    #if SK_ALS
        if (veml.begin()) {
            veml.setGain(VEML7700_GAIN_2);
            veml.setIntegrationTime(VEML7700_IT_400MS);
        } else {
            log("ALS sensor not found!");
        }
    #endif

    // Built-in profiles (the apps in the menu)
    loadBuiltinProfiles(profiles_, [this] (const char* msg) { log(msg); });
    snprintf(buf_, sizeof(buf_), "Profiles: %d loaded", (int)profiles_.size());
    log(buf_);
    for (int i = 0; i < (int)profiles_.size(); i++) {
        if (profiles_[i].in_menu) {
            menu_apps_.push_back(i);
        }
    }
    if (profiles_.empty()) {
        // Never leave the knob with nothing to open
        log("No profiles loaded: using a plain knob");
        Profile plain;
        plain.name = "Knob";
        ProfilePage page;
        page.label = "Knob";
        page.feel.max_position = -1;
        page.feel.position_width_radians = 10 * PI / 180;
        page.feel.endstop_strength_unit = 1;
        page.feel.snap_point = 1.1;
        strncpy(page.feel.text, "Knob", sizeof(page.feel.text) - 1);
        ProfileModule module;
        module.name = "Knob";
        module.pages.push_back(page);
        ProfileMode mode;
        mode.name = "Knob";
        mode.modules.push_back(module);
        plain.modes.push_back(mode);
        profiles_.push_back(plain);
        menu_apps_.push_back(0);
    }

    // Restore saved settings and open the last-used app
    settings_ = settings_store_.load();
    applySettings();
    #if SK_BLE
        ble_volume_level_ = CLAMP(settings_.volume, (int32_t)0, (int32_t)64);
    #endif
    if (charging_mode_) {
        log("Started by a charger: charging screen (press the Function button to start)");
        motor_task_.setRelaxed(true);
        showChargingScreen();
    } else {
        openApp(menuIndexOf(settings_.app) >= 0 ? settings_.app : (menu_apps_.empty() ? 0 : menu_apps_[0]));
    }
    sendStatus();
    motor_task_.addListener(knob_state_queue_);

    plaintext_protocol_.setJsonLineCallback([this] (const char* line) {
        handleLinkMessage(line);
    });
    plaintext_protocol_.init([this] () {
        changeConfig(true);
    }, [this] () {
        if (!configuration_loaded_) {
            return;
        }
        if (strain_calibration_step_ == 0) {
            log("Strain calibration step 1: Don't touch the knob, then press 'S' again");
            strain_calibration_step_ = 1;
        } else if (strain_calibration_step_ == 1) {
            configuration_value_.strain.idle_value = strain_reading_;
            snprintf(buf_, sizeof(buf_), "  idle_value=%d", configuration_value_.strain.idle_value);
            log(buf_);
            log("Strain calibration step 2: Push and hold down the knob with medium pressure, and press 'S' again");
            strain_calibration_step_ = 2;
        } else if (strain_calibration_step_ == 2) {
            configuration_value_.strain.press_delta = strain_reading_ - configuration_value_.strain.idle_value;
            configuration_value_.has_strain = true;
            snprintf(buf_, sizeof(buf_), "  press_delta=%d", configuration_value_.strain.press_delta);
            log(buf_);
            log("Strain calibration complete! Saving...");
            strain_calibration_step_ = 0;
            if (configuration_->setStrainCalibrationAndSave(configuration_value_.strain)) {
                log("  Saved!");
            } else {
                log("  FAILED to save config!!!");
            }
        }
    });

    // Start in legacy protocol mode
    current_protocol_ = &plaintext_protocol_;

    ProtocolChangeCallback protocol_change_callback = [this] (uint8_t protocol) {
        switch (protocol) {
            case SERIAL_PROTOCOL_LEGACY:
                current_protocol_ = &plaintext_protocol_;
                break;
            case SERIAL_PROTOCOL_PROTO:
                current_protocol_ = &proto_protocol_;
                break;
            default:
                log("Unknown protocol requested");
                break;
        }
    };

    plaintext_protocol_.setProtocolChangeCallback(protocol_change_callback);
    proto_protocol_.setProtocolChangeCallback(protocol_change_callback);

    // Interface loop:
    while (1) {
        if (xQueueReceive(knob_state_queue_, &latest_state_, 0) == pdTRUE) {
            publishState();
            checkKnobActivity();
            updateLiveSetting();
            #if SK_BLE
                updateBleOutput();
            #endif
        }

        current_protocol_->loop();

        std::string* log_string;
        while (xQueueReceive(log_queue_, &log_string, 0) == pdTRUE) {
            current_protocol_->log(log_string->c_str());
            delete log_string;
        }

        updateHardware();

        #if SK_TOUCH
            updateTouch();
        #endif

        #ifdef PIN_POWER_BUTTON
            updatePowerButton();
        #endif

        #if SK_BLE
        {
            PairingEvent pairing = ble_.update(millis());
            if (pairing == PairingEvent::PAIRED) {
                log("Bluetooth: new device paired");
                toast(LV_SYMBOL_BLUETOOTH " Paired");
                refreshBluetoothScreen();
            } else if (pairing == PairingEvent::CLOSED) {
                log("Bluetooth: pairing closed");
                toast("Pairing closed");
                refreshBluetoothScreen();
            }

            // Keep the pairing countdown ticking on the Bluetooth screen
            static uint32_t last_countdown;
            if (ble_.isPairing() && millis() - last_countdown >= 1000) {
                last_countdown = millis();
                refreshBluetoothScreen();
            }
        }
        #endif

        updateUsbPower();
        updateSleep();
        checkPageTimeout();

        #ifdef PIN_BATTERY_ADC
            updateBattery();
        #endif

        if (!configuration_loaded_) {
            SemaphoreGuard lock(mutex_);
            if (configuration_ != nullptr) {
                configuration_value_ = configuration_->get();
                configuration_loaded_ = true;
            }
        }

        delay(1);
    }
}

void InterfaceTask::log(const char* msg) {
    // Allocate a string for the duration it's in the queue; it is free'd by the queue consumer
    std::string* msg_str = new std::string(msg);

    // Put string in queue (or drop if full to avoid blocking)
    xQueueSendToBack(log_queue_, &msg_str, 0);
}

static int wrapIndex(int i, int count) {
    return count > 0 ? ((i % count) + count) % count : 0;
}

const ProfilePage& InterfaceTask::currentPage() {
    const Profile& profile = profiles_[current_app_];
    const ProfileMode& mode = profile.modes[current_mode_];
    const ProfileModule& module = mode.modules[current_module_];
    return module.pages[current_page_];
}

void InterfaceTask::changeConfig(bool next) {
    // Next / previous mode in the current profile (swipe left / right; wraps around)
    if (nav_screen_ != NavScreen::APP) {
        return;
    }
    const Profile& profile = profiles_[current_app_];
    int count = profile.modes.size();
    int mode = current_mode_;
    for (int tries = 0; tries < count; tries++) {
        mode = wrapIndex(mode + (next ? 1 : -1), count);
        if (profile.modes[mode].tools.empty()) {
            break;  // tool modes (e.g. Brush) only open by themselves
        }
    }
    current_mode_ = mode;
    current_module_ = 0;
    current_page_ = 0;
    module_pages_.clear();
    view_page_ = 0;
    applyAppPage();
    if (profiles_[current_app_].mode_toasts) {
        toast(profiles_[current_app_].modes[current_mode_].name.c_str());
    }
    // e.g. Lightroom: Library opens the grid, Develop opens Develop
    sendKey(currentPage().enter);
}

void InterfaceTask::sendKey(const KeyCombo& combo, int32_t times) {
    #if SK_BLE
        if (combo.media != 0) {
            ble_.sendMediaKey(combo.media);
        } else if (combo.isSet()) {
            ble_.sendKey(combo.modifiers, combo.key, times);
        }
    #endif
}

// Edge buttons for the current page: the page's own, else its module's
const std::vector<PickOption>& InterfaceTask::edgeButtons() {
    const ProfilePage& page = currentPage();
    if (!page.buttons.empty()) return page.buttons;
    return profiles_[current_app_].modes[current_mode_].modules[current_module_].buttons;
}

// Pages with a timeout (zoom, color, rating) go back to browsing when left alone
void InterfaceTask::checkPageTimeout() {
    if (nav_screen_ != NavScreen::APP) {
        return;
    }
    const ProfilePage& page = currentPage();
    if (page.timeout_s == 0) {
        return;
    }
    uint32_t since = millis() - max(page_opened_ms_, last_activity_ms_);
    if (since > page.timeout_s * 1000UL) {
        openPage(viewPage(false));
    }
}

void InterfaceTask::openPage(int page) {
    const ProfileModule& module = profiles_[current_app_].modes[current_mode_].modules[current_module_];
    current_page_ = CLAMP(page, 0, (int)module.pages.size() - 1);
    if (module.pages[current_page_].enter.isSet()) {
        view_page_ = current_page_;   // Grid / Loupe: remember which view we're in
    }
    applyAppPage();
    sendKey(module.pages[current_page_].enter);
}

// "View" pages are the ones with an enter key (Grid, Loupe). other = the one we're not in.
int InterfaceTask::viewPage(bool other) {
    const ProfileModule& module = profiles_[current_app_].modes[current_mode_].modules[current_module_];
    int count = module.pages.size();
    if (view_page_ >= count || !module.pages[view_page_].enter.isSet()) {
        view_page_ = 0;
    }
    if (!other) return view_page_;
    for (int i = 1; i < count; i++) {
        int p = (view_page_ + i) % count;
        if (module.pages[p].enter.isSet()) return p;
    }
    return view_page_;
}

void InterfaceTask::openModuleNamed(const char* name) {
    const ProfileMode& mode = profiles_[current_app_].modes[current_mode_];
    for (size_t i = 0; i < mode.modules.size(); i++) {
        if (mode.modules[i].name.equalsIgnoreCase(name)) {
            openModule(i);
            return;
        }
    }
}

// Do a gesture's action (or its default when the page doesn't set one)
bool InterfaceTask::runAction(const Action& action, ActionKind fallback, bool strong) {
    ActionKind kind = action.isSet() ? action.kind : fallback;
    if (kind == ActionKind::NONE) {
        return false;
    }
    if (kind == ActionKind::NOTHING) {
        return true;
    }
    motor_task_.playHaptic(strong);
    switch (kind) {
        case ActionKind::KEY:         sendKey(action.key); break;
        case ActionKind::NEXT_PAGE:   nextPage(); break;
        case ActionKind::NEXT_MODULE: changeModule(true); break;
        case ActionKind::PREV_MODULE: changeModule(false); break;
        case ActionKind::NEXT_MODE:   changeConfig(true); break;
        case ActionKind::PREV_MODE:   changeConfig(false); break;
        case ActionKind::MODULE:      openModuleNamed(action.name.c_str()); break;
        case ActionKind::VIEW:        openPage(viewPage(true)); break;
        case ActionKind::BACK:        openPage(viewPage(false)); break;
        case ActionKind::PAGE: {
            // "A|B": pick by which view we're in (first view page = A)
            String name = action.name;
            int bar = name.indexOf('|');
            if (bar >= 0) {
                const ProfileModule& module = profiles_[current_app_].modes[current_mode_].modules[current_module_];
                int view_index = 0;
                for (int i = 0; i < viewPage(false); i++) {
                    if (module.pages[i].enter.isSet()) view_index++;
                }
                name = view_index == 0 ? name.substring(0, bar) : name.substring(bar + 1);
            }
            const ProfileModule& module = profiles_[current_app_].modes[current_mode_].modules[current_module_];
            for (size_t i = 0; i < module.pages.size(); i++) {
                if (module.pages[i].label.equalsIgnoreCase(name)) openPage(i);
            }
            break;
        }
        case ActionKind::LINK: {
            JsonDocument doc;
            doc["t"] = "act";
            doc["a"] = action.name;
            doc["param"] = currentPage().param;
            sendLink(doc);
            break;
        }
        default: break;
    }
    return true;
}

void InterfaceTask::sendLinkTurn(int32_t steps) {
    const ProfilePage& page = currentPage();
    JsonDocument doc;
    if (steps == 0) {
        // Opening a slider: the full message names it and asks for its value and range
        doc["t"] = "turn";
        doc["param"] = page.param;
        doc["d"] = 0;
        if (page.step > 0) {
            doc["step"] = page.step;
        }
    } else {
        // Every click after that: as short as possible (one radio report), numbered
        doc["t"] = "n";
        doc["d"] = steps;
        doc["s"] = ++link_seq_;
        last_turn_ms_ = millis();
        showLinkEstimate();
    }
    sendLink(doc);
}

// Show where the slider should be after our turn, without waiting for Lightroom
void InterfaceTask::showLinkEstimate() {
    #if SK_DISPLAY
        const PB_SmartKnobConfig& feel = currentPage().feel;
        if (link_high_ <= link_low_ || feel.max_position <= feel.min_position) {
            return;
        }
        float value = link_low_ + (latest_state_.current_position - feel.min_position)
            * (link_high_ - link_low_) / (feel.max_position - feel.min_position);
        display_task_->showValueText(formatLinkValue(value).c_str());
    #endif
}

String InterfaceTask::formatLinkValue(float value) {
    char text[16];
    if (link_format_ == "%+.2f") snprintf(text, sizeof(text), "%+.2f", value);
    else if (link_format_ == "%+d") snprintf(text, sizeof(text), "%+d", (int)lroundf(value));
    else snprintf(text, sizeof(text), "%d", (int)lroundf(value));
    return String(text);
}

void InterfaceTask::openMode(int mode, int module) {
    const Profile& profile = profiles_[current_app_];
    current_mode_ = CLAMP(mode, 0, (int)profile.modes.size() - 1);
    current_module_ = CLAMP(module, 0, (int)profile.modes[current_mode_].modules.size() - 1);
    current_page_ = 0;
    module_pages_.clear();
    view_page_ = 0;
    applyAppPage();
    if (profile.mode_toasts) {
        toast(profile.modes[current_mode_].name.c_str());
    }
}

// The Mac says which module the program is in (Lightroom: "library", "develop"):
// open the mode for it. While a tool mode is open, it's where you return to after.
void InterfaceTask::handleModule(const char* module) {
    if (nav_screen_ != NavScreen::APP) {
        return;
    }
    const Profile& profile = profiles_[current_app_];
    for (int i = 0; i < (int)profile.modes.size(); i++) {
        if (profile.modes[i].module.length() > 0 && profile.modes[i].module.equalsIgnoreCase(module)) {
            if (!profile.modes[current_mode_].tools.empty()) {
                mode_before_tool_ = i;
                module_before_tool_ = 0;
            } else if (i != current_mode_) {
                openMode(i, 0);
            }
            return;
        }
    }
}

// The Mac says which tool is in use (e.g. Lightroom "masking"): open the mode that
// lists it, and go back to where you were when it closes
void InterfaceTask::handleTool(const char* tool) {
    if (nav_screen_ != NavScreen::APP) {
        return;
    }
    const Profile& profile = profiles_[current_app_];
    int tool_mode = -1;
    for (int i = 0; i < (int)profile.modes.size() && tool_mode < 0; i++) {
        for (const String& t : profile.modes[i].tools) {
            if (t.equalsIgnoreCase(tool)) {
                tool_mode = i;
                break;
            }
        }
    }
    bool in_tool_mode = !profile.modes[current_mode_].tools.empty();
    if (tool_mode >= 0 && tool_mode != current_mode_) {
        if (!in_tool_mode) {
            mode_before_tool_ = current_mode_;
            module_before_tool_ = current_module_;
        }
        openMode(tool_mode, 0);
    } else if (tool_mode < 0 && in_tool_mode) {
        openMode(mode_before_tool_, module_before_tool_);
    }
}

// A tap near the edge of a page with buttons picks the nearest button's page.
// Returns false for taps in the middle (those do the page's own tap).
bool InterfaceTask::tapButton(uint16_t x, uint16_t y) {
    const ProfileModule& module = profiles_[current_app_].modes[current_mode_].modules[current_module_];
    const std::vector<PickOption>& actions = edgeButtons();
    const ProfilePage& page = currentPage();
    bool action_buttons = !actions.empty();
    int count = action_buttons ? actions.size() : module.pages.size();
    if (!action_buttons && (count < 2 || count > UI_MAX_ITEMS || module.pages[0].short_name.length() == 0)) {
        return false;
    }
    float dx = (float)x - TFT_WIDTH / 2;
    float dy = (float)y - TFT_HEIGHT / 2;
    if (sqrtf(dx * dx + dy * dy) < BUTTON_TAP_MIN_RADIUS) {
        return false;
    }
    float angle = atan2f(dy, dx) * 180 / PI;   // same degrees as the screen (0 = right, 90 = down)
    int nearest = 0;
    float best = 1000;
    for (int i = 0; i < count; i++) {
        float d = angle - uiButtonAngle(i, count);
        d -= 360 * floorf((d + 180) / 360);   // into -180..180
        float diff = fabsf(d);
        if (diff < best) {
            best = diff;
            nearest = i;
        }
    }
    if (action_buttons) {
        const PickOption& button = actions[nearest];
        if (!page.pick.empty()) {
            bool same = button.action.kind == ActionKind::PAGE && button.action.name.indexOf(page.label) >= 0;
            if (same) {
                applyPick(true);   // the button for this picker: accept and return
                return true;
            }
            applyPick(false);      // another button: accept, then do that one
        }
        snprintf(buf_, sizeof(buf_), "Button: %s", button.label.c_str());
        log(buf_);
        runAction(button.action, ActionKind::NONE, true);
        return true;
    }
    motor_task_.playHaptic(true);
    if (nearest != current_page_) {
        current_page_ = nearest;
        applyAppPage();
    }
    return true;
}

// Lightroom reported a slider's value and range: put the knob there, so the ring
// shows where the slider sits and the end stops match its limits
void InterfaceTask::syncLinkValue(float value, float low, float high) {
    const PB_SmartKnobConfig& feel = currentPage().feel;
    if (high <= low || feel.max_position < feel.min_position) {
        return;
    }
    int32_t mapped = feel.min_position + (int32_t)lroundf((value - low) / (high - low) * (feel.max_position - feel.min_position));
    mapped = CLAMP(mapped, feel.min_position, feel.max_position);
    if (abs(mapped - latest_state_.current_position) < 2) {
        return;   // already there (one step of rounding is fine)
    }
    PB_SmartKnobConfig config = latest_config_;
    config.position = mapped;
    config.sub_position_unit = 0;
    config.position_nonce = ++nav_nonce_;
    #if SK_BLE
        ble_last_position_ = mapped;   // the jump isn't a turn
    #endif
    applyConfig(config, false);
}

// Menu > Controls: what each gesture does on the page you were on
static String describeAction(const Action& action, const char* fallback) {
    if (!action.isSet()) return fallback;
    switch (action.kind) {
        case ActionKind::NEXT_PAGE:   return "next page";
        case ActionKind::NEXT_MODULE: return "next module";
        case ActionKind::PREV_MODULE: return "prev module";
        case ActionKind::NEXT_MODE:   return "next mode";
        case ActionKind::PREV_MODE:   return "prev mode";
        case ActionKind::LINK:        return action.name.substring(5);   // after "link:"
        default:                      return "key " + action.name;
    }
}

void InterfaceTask::openControls(int menu_index) {
    controls_menu_index_ = menu_index;
    setNavScreen(NavScreen::SETTING);
    open_setting_ = SET_CONTROLS;
    const ProfilePage& page = currentPage();
    bool picker = !page.pick.empty();

    UiScreen ui;
    ui.layout = UiLayout::LIST;
    copyText(ui.title, page.label.c_str());
    ui.ring_at_bottom = true;
    struct Row { const char* gesture; String what; } rows[] = {
        {"Tap", picker ? String("choose") : describeAction(page.tap, "-")},
        {"2x tap", describeAction(page.double_tap, "next mode")},
        {"3x tap", describeAction(page.triple_tap, "-")},
        {"Hold", describeAction(page.long_press, "next page")},
        {"Swipe L", picker ? String("back") : describeAction(page.swipe_left, "-")},
        {"Swipe R", describeAction(page.swipe_right, "-")},
        {"Swipe up", describeAction(page.swipe_up, "next module")},
        {"Swipe dn", describeAction(page.swipe_down, "prev module")},
    };
    ui.item_count = 8;
    for (int i = 0; i < 8; i++) {
        snprintf(ui.items[i], UI_TEXT_LEN, "%s: %s", rows[i].gesture, rows[i].what.c_str());
    }
    showUi(ui);
    PB_SmartKnobConfig config = makeListConfig("Controls", 8, 0, ++nav_nonce_);
    applyConfig(config, false);
}

// Picker page: send the chosen option's key, then go back to the first module
// Under "Volume": a pause symbol while music plays, play while it's paused, nothing otherwise
void InterfaceTask::showMediaState() {
    #if SK_DISPLAY
        if (nav_screen_ != NavScreen::APP || currentPage().turn != TurnOutput::VOLUME) {
            return;
        }
        const char* symbol = media_state_ == "playing" ? LV_SYMBOL_PAUSE
                           : media_state_ == "paused" ? LV_SYMBOL_PLAY : "";
        display_task_->showValueText(symbol, true);
    #endif
}

void InterfaceTask::applyPick(bool go_back) {
    const ProfilePage& page = currentPage();
    if (page.pick.empty()) {
        return;
    }
    int32_t index = CLAMP(selectedIndex(), (int32_t)0, (int32_t)page.pick.size() - 1);
    const PickOption& option = page.pick[index];
    if (page.pick_value == "rating") link_rating_ = index;
    if (page.pick_value == "label") link_label_ = option.label;
    runAction(option.action, ActionKind::NONE, true);
    if (go_back) {
        openPage(viewPage(false));
    }
}

void InterfaceTask::changeModule(bool next) {
    // Next / previous module in the current mode (swipe up / down; wraps around)
    const ProfileMode& mode = profiles_[current_app_].modes[current_mode_];
    if (nav_screen_ != NavScreen::APP || mode.modules.size() < 2) {
        return;
    }
    openModule(wrapIndex(current_module_ + (next ? 1 : -1), mode.modules.size()));
    if (currentPage().pick.empty()) {
        toast(mode.modules[current_module_].name.c_str());   // e.g. "Presence" (a slider group counts as a mode)
    }
}

void InterfaceTask::openModule(int module) {
    const ProfileMode& mode = profiles_[current_app_].modes[current_mode_];
    module = CLAMP(module, 0, (int)mode.modules.size() - 1);
    module_pages_.resize(mode.modules.size(), 0);
    module_pages_[current_module_] = current_page_;
    current_module_ = module;
    current_page_ = CLAMP(module_pages_[module], 0, (int)mode.modules[module].pages.size() - 1);
    applyAppPage();
}

void InterfaceTask::nextPage() {
    // Next page in the current module (long press; wraps around)
    const ProfileModule& module = profiles_[current_app_].modes[current_mode_].modules[current_module_];
    if (nav_screen_ != NavScreen::APP || module.pages.size() < 2) {
        return;
    }
    openPage(wrapIndex(current_page_ + 1, module.pages.size()));
}

int InterfaceTask::menuIndexOf(int app) {
    for (int i = 0; i < menuAppCount(); i++) {
        if (menu_apps_[i] == app) {
            return i;
        }
    }
    return -1;
}

int InterfaceTask::findProfile(const char* name) {
    for (int i = 0; i < appCount(); i++) {
        if (profiles_[i].name.equalsIgnoreCase(name)) {
            return i;
        }
    }
    return -1;
}

// ======================= Link (Mac app messages) =======================
//
// Mac -> knob:  {"t":"hello"}
//               {"t":"switch","p":"Demo"}       open a profile (the program in front changed)
// Knob -> Mac:  {"t":"hello","fw":"0.2.0","profiles":["Mac","Demo"],"p":"Mac"}
//               {"t":"where","p":"Mac","mode":"Volume","module":"Volume","page":"Volume"}
//               {"t":"ok"}  /  {"t":"err","msg":"..."}

void InterfaceTask::handleLinkMessage(const char* line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) {
        sendLinkResult("bad JSON");
        return;
    }
    const char* type = doc["t"] | "";

    if (strcmp(type, "hello") == 0) {
        sendHello();
    } else if (strcmp(type, "switch") == 0) {
        const char* name = doc["p"] | "";
        int index = findProfile(name);
        if (index < 0) {
            sendLinkResult("no such profile");
            return;
        }
        link_app_ = profiles_[index].in_menu ? -1 : index;
        if (index != current_app_) {
            if (nav_screen_ == NavScreen::APP) {
                openApp(index);
                toast(profiles_[index].name.c_str());
            } else {
                // In a menu: don't pull you out; the Function button returns to the new profile
                current_app_ = index;
                settings_.app = index;
                current_mode_ = current_module_ = current_page_ = 0;
                module_pages_.clear();
                view_page_ = 0;
            }
        }
        sendLinkResult(nullptr);
    } else if (strcmp(type, "val") == 0) {
        // A value to show, e.g. Lightroom's Exposure "+0.35"
        // Full form (on opening a slider): param, v, lo, hi, fmt. Short form while
        // turning: v and s (the click it answers), for the slider the knob is on.
        const char* param = doc["param"] | "";
        bool mine = nav_screen_ == NavScreen::APP && currentPage().turn == TurnOutput::LINK
            && (param[0] == 0 || currentPage().param.equals(param));
        if (mine) {
            if (doc["lo"].is<float>() && doc["hi"].is<float>()) {
                link_low_ = doc["lo"] | 0.0f;
                link_high_ = doc["hi"] | 0.0f;
                link_format_ = doc["fmt"] | "%d";
                link_seq_ = 0;
            }
            // A reply to an older click (we've turned since) is stale: the knob already
            // knows better. Only the answer to the latest click, with the knob at rest,
            // may move the knob.
            uint32_t seq = doc["s"] | 0;
            bool current = seq == link_seq_ && millis() - last_turn_ms_ > 150;
            if (doc["v"].is<float>() && current) {
                float value = doc["v"] | 0.0f;
                #if SK_DISPLAY
                    display_task_->showValueText(formatLinkValue(value).c_str());
                #endif
                syncLinkValue(value, link_low_, link_high_);
            }
        }
    } else if (strcmp(type, "ping") == 0) {
        JsonDocument reply;
        reply["t"] = "pong";
        reply["s"] = doc["s"] | 0;
        #if SK_BLE
            uint16_t interval, latency;
            if (ble_.connParams(interval, latency)) {
                reply["itvl"] = interval;   // x 1.25 ms
                reply["lat"] = latency;
                reply["held"] = millis() - ble_.linkLastWriteMs();   // ms the knob sat on the ping
                uint32_t send_ms; uint8_t reports; bool ok;
                ble_.linkSendStats(send_ms, reports, ok);
                reply["prev"] = send_ms;      // how long the previous send took
                reply["rep"] = reports;
                reply["ok"] = ok;
            }
        #endif
        sendLink(reply);
    } else if (strcmp(type, "media") == 0) {
        media_state_ = doc["state"] | "";
        showMediaState();
    } else if (strcmp(type, "meta") == 0) {
        // The selected photo's rating and color label
        link_rating_ = doc["rating"] | 0;
        link_label_ = doc["label"] | "";
    } else if (strcmp(type, "tool") == 0) {
        link_tool_ = doc["tool"] | "";
        handleTool(link_tool_.c_str());
    } else if (strcmp(type, "module") == 0) {
        link_module_ = doc["module"] | "";
        handleModule(link_module_.c_str());
    } else {
        sendLinkResult("unknown message");
    }
}

void InterfaceTask::sendLink(JsonDocument& doc) {
    // Over USB (in plaintext mode; the binary protocol owns the line otherwise) and Bluetooth
    String line;
    serializeJson(doc, line);
    if (current_protocol_ == &plaintext_protocol_) {
        stream_.println(line);
    }
    #if SK_BLE
        if (ble_.linkSubscribed()) {
            ble_.linkSend(line.c_str());
        }
    #endif
}

void InterfaceTask::sendLinkResult(const char* error) {
    JsonDocument doc;
    if (error == nullptr) {
        doc["t"] = "ok";
    } else {
        doc["t"] = "err";
        doc["msg"] = error;
    }
    sendLink(doc);
}

void InterfaceTask::sendHello() {
    JsonDocument doc;
    doc["t"] = "hello";
    doc["fw"] = FIRMWARE_VERSION;
    JsonArray names = doc["profiles"].to<JsonArray>();
    for (const Profile& profile : profiles_) {
        names.add(profile.name);
    }
    doc["p"] = profiles_[current_app_].name;
    sendLink(doc);
}

void InterfaceTask::sendWhere() {
    const Profile& profile = profiles_[current_app_];
    const ProfileMode& mode = profile.modes[current_mode_];
    JsonDocument doc;
    doc["t"] = "where";
    doc["p"] = profile.name;
    doc["mode"] = mode.name;
    doc["module"] = mode.modules[current_module_].name;
    doc["page"] = currentPage().label;
    sendLink(doc);
}

void InterfaceTask::updateHardware() {
    // How far button is pressed, in range [0, 1]
    float press_value_unit = 0;

    #if SK_ALS
        const float LUX_ALPHA = 0.005;
        static float lux_avg;
        float lux = veml.readLux();
        lux_avg = lux * LUX_ALPHA + lux_avg * (1 - LUX_ALPHA);
        static uint32_t last_als;
        if (millis() - last_als > 1000 && strain_calibration_step_ == 0) {
            snprintf(buf_, sizeof(buf_), "millilux: %.2f", lux*1000);
            log(buf_);
            last_als = millis();
        }
    #endif

    static bool pressed;
    #if SK_STRAIN
        if (scale.wait_ready_timeout(100)) {
            strain_reading_ = scale.read();

            static uint32_t last_reading_display;
            if (millis() - last_reading_display > 1000 && strain_calibration_step_ == 0) {
                snprintf(buf_, sizeof(buf_), "HX711 reading: %d", strain_reading_);
                log(buf_);
                last_reading_display = millis();
            }
            if (configuration_loaded_ && configuration_value_.has_strain && strain_calibration_step_ == 0) {
                // TODO: calibrate and track (long term moving average) idle point (lower)
                press_value_unit = lerp(strain_reading_, configuration_value_.strain.idle_value, configuration_value_.strain.idle_value + configuration_value_.strain.press_delta, 0, 1);

                // Ignore readings that are way out of expected bounds
                if (-1 < press_value_unit && press_value_unit < 2) {
                    static uint8_t press_readings;
                    if (!pressed && press_value_unit > 1) {
                        press_readings++;
                        if (press_readings > 2) {
                            motor_task_.playHaptic(true);
                            pressed = true;
                            press_count_++;
                            publishState();
                            if (!remote_controlled_) {
                                changeConfig(true);
                            }
                        }
                    } else if (pressed && press_value_unit < 0.5) {
                        press_readings++;
                        if (press_readings > 2) {
                            motor_task_.playHaptic(false);
                            pressed = false;
                        }
                    } else {
                        press_readings = 0;
                    }
                }
            }
        } else {
            log("HX711 not found.");

            #if SK_LEDS
                for (uint8_t i = 0; i < NUM_LEDS; i++) {
                    leds[i] = CRGB::Red;
                }
                FastLED.show();
            #endif
        }
    #endif

    uint16_t brightness = (uint32_t)UINT16_MAX * settings_.brightness_pct / 100;
    if (sleep_stage_ == 1) {
        brightness = (uint32_t)brightness * SLEEP_DIM_PCT / 100;
    } else if (sleep_stage_ == 2) {
        brightness = 0;
    }
    // TODO: brightness scale factor should be configurable (depends on reflectivity of surface)
    #if SK_ALS
        brightness = (uint16_t)CLAMP(lux_avg * 13000, (float)1280, (float)UINT16_MAX);
    #endif

    #if SK_DISPLAY
        display_task_->setBrightness(brightness); // TODO: apply gamma correction
    #endif

    #if SK_LEDS
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
            leds[i].setHSV(latest_config_.led_hue, 255 - 180*CLAMP(press_value_unit, (float)0, (float)1) - 75*pressed, brightness >> 8);

            // Gamma adjustment
            leds[i].r = dim8_video(leds[i].r);
            leds[i].g = dim8_video(leds[i].g);
            leds[i].b = dim8_video(leds[i].b);
        }
        FastLED.show();
    #endif
}

void InterfaceTask::setConfiguration(Configuration* configuration) {
    SemaphoreGuard lock(mutex_);
    configuration_ = configuration;
}

void InterfaceTask::publishState() {
    // Apply local state before publishing to serial
    latest_state_.press_nonce = press_count_;
    current_protocol_->handleState(latest_state_);
}

void InterfaceTask::applyConfig(PB_SmartKnobConfig& config, bool from_remote) {
    remote_controlled_ = from_remote;
    latest_config_ = config;

    // Settings > Haptics scales every click and end stop
    PB_SmartKnobConfig scaled = config;
    float strength = settings_.haptics_pct / 100.0f;
    scaled.detent_strength_unit *= strength;
    scaled.endstop_strength_unit *= strength;
    motor_task_.setConfig(scaled);
}

#if SK_TOUCH
// Rim travel needed before a touch counts as a slide instead of a tap
static const float RING_SLIDE_START_DEGREES = 4;

void InterfaceTask::updateTouch() {
    // Poll the touch chip every 10 ms
    static uint32_t last_poll;
    uint32_t now = millis();
    if (now - last_poll < 10) {
        return;
    }
    last_poll = now;

    uint16_t x = 0, y = 0;
    bool touching = touch_.read(x, y);

    // Charging screen: a tap turns the screen off / back on
    if (charging_mode_) {
        if (touch_gestures_.update(touching, x, y, now) == TouchGesture::TAP) {
            if (sleep_stage_ == 2) {
                noteActivity();  // wake
            } else {
                last_activity_ms_ = millis() - SLEEP_SCREEN_OFF_MS;  // straight to screen off
                updateSleep();
            }
        }
        return;
    }

    // A touch while the screen is off only wakes the knob
    if (touching) {
        if (sleep_stage_ == 2) {
            swallow_touch_ = true;
        }
        noteActivity();
    }
    if (swallow_touch_) {
        touch_gestures_.update(touching, x, y, now);
        touch_gestures_.cancelCurrentTouch();
        if (!touching) {
            swallow_touch_ = false;
        }
        return;
    }

    // Sliding around the rim acts like turning the knob
    float ring_delta = touch_ring_.update(touching, x, y);
    if (touch_ring_.active() && ring_mode_ != RingMode::OFF) {
        if (!ring_was_active_) {
            // Finger just arrived on the rim
            ring_was_active_ = true;
            ring_is_slide_ = false;
            ring_stepper_.start(latest_state_.current_position);
        }

        // Small wiggles still count as taps; past a few degrees it's a slide
        float feed_degrees = 0;
        if (!ring_is_slide_ && fabsf(touch_ring_.totalDegrees()) > RING_SLIDE_START_DEGREES) {
            ring_is_slide_ = true;
            touch_gestures_.cancelCurrentTouch();
            feed_degrees = touch_ring_.totalDegrees();
        } else if (ring_is_slide_) {
            feed_degrees = ring_delta;
        }

        if (ring_is_slide_) {
            #if SK_BLE
            if (nav_screen_ == NavScreen::APP && currentPage().rim_cw.isSet()) {
                // This page gives the rim its own keys (e.g. zoom in Lightroom)
                ring_scroll_accum_degrees_ += feed_degrees;
                int32_t presses = (int32_t)(ring_scroll_accum_degrees_ / RIM_KEY_DEGREES);
                if (presses != 0) {
                    sendKey(presses > 0 ? currentPage().rim_cw : currentPage().rim_ccw, presses);
                    ring_scroll_accum_degrees_ -= presses * RIM_KEY_DEGREES;
                    motor_task_.playHaptic(false);
                }
            } else if (inBleMode((int)TurnOutput::SCROLL)) {
                // Scroll mode has no value to step: the rim scrolls directly
                ring_scroll_accum_degrees_ += feed_degrees;
                int32_t ticks = (int32_t)(ring_scroll_accum_degrees_ / RIM_SCROLL_DEGREES);
                if (ticks != 0) {
                    ble_.scrollSteps(ticks);
                    ring_scroll_accum_degrees_ -= ticks * RIM_SCROLL_DEGREES;
                }
            } else
            #endif
            if (ring_mode_ == RingMode::TRACK) {
                // Change the value exactly as turning the knob the same angle would
                RingStepResult result = ring_stepper_.step(feed_degrees * DEG_TO_RAD,
                    latest_config_.position_width_radians, latest_config_.min_position, latest_config_.max_position);
                if (result.moved) {
                    PB_SmartKnobConfig config = latest_config_;
                    config.position = result.position;
                    config.sub_position_unit = 0;
                    config.position_nonce = latest_config_.position_nonce + 1;
                    applyConfig(config, remote_controlled_);
                    if (latest_config_.detent_strength_unit > 0) {
                        motor_task_.playHaptic(false);  // light click, only in modes with detents
                    }
                }
                if (result.hit_end) {
                    motor_task_.playHaptic(true);  // firm bump at the limit
                }
            } else {
                // Motor physically turns the knob to follow the finger
                motor_task_.setFollow(true, touch_ring_.totalDegrees() * DEG_TO_RAD);
            }
        }
    } else if (ring_was_active_) {
        // Finger left the rim
        ring_was_active_ = false;
        if (ring_mode_ == RingMode::FOLLOW) {
            motor_task_.setFollow(false, 0);
        }
    }

    TouchGesture gesture = touch_gestures_.update(touching, x, y, now);
    if (gesture == TouchGesture::NONE) {
        return;
    }

    // Test mode: every gesture is printed in the serial monitor
    snprintf(buf_, sizeof(buf_), "Touch: %s at (%d, %d)",
        touchGestureName(gesture), touch_gestures_.gestureX(), touch_gestures_.gestureY());
    log(buf_);

    // System controls live on the Function button; the touch screen belongs to the app,
    // except inside menus (no app running there)
    if (nav_screen_ != NavScreen::APP) {
        if (gesture == TouchGesture::TAP) {
            motor_task_.playHaptic(true);
            navSelect();
        } else if (gesture == TouchGesture::SWIPE_RIGHT || gesture == TouchGesture::SWIPE_LEFT) {
            // Either swipe: back
            motor_task_.playHaptic(false);
            navBack();
        }
        return;
    }

    // In an app: each page sets what gestures do. Defaults: double tap = next mode,
    // swipe up / down = module, long press = next page.
    const ProfilePage& page = currentPage();
    switch (gesture) {
        case TouchGesture::TAP:
            if (!page.pick.empty() && !edgeButtons().empty()
                    && tapButton(touch_gestures_.gestureX(), touch_gestures_.gestureY())) {
                // chose a button round the edge of a picker
            } else if (!page.pick.empty()) {
                applyPick();
            } else if (tapButton(touch_gestures_.gestureX(), touch_gestures_.gestureY())) {
                // chose a button round the edge
            } else {
                runAction(page.tap, ActionKind::NONE, true);
            }
            break;
        case TouchGesture::DOUBLE_TAP:  runAction(page.double_tap, ActionKind::NEXT_MODE, true); break;
        case TouchGesture::TRIPLE_TAP:
            runAction(page.triple_tap.isSet() ? page.triple_tap : profiles_[current_app_].triple_tap, ActionKind::NONE, true);
            break;
        case TouchGesture::LONG_PRESS:  runAction(page.long_press, ActionKind::NEXT_PAGE, true); break;
        case TouchGesture::SWIPE_LEFT:
            if (!page.pick.empty() && !page.swipe_left.isSet()) {
                // Picker (Color, Rating): swipe left backs out without choosing
                motor_task_.playHaptic(false);
                openPage(viewPage(false));
            } else {
                runAction(page.swipe_left, ActionKind::NONE, false);
            }
            break;
        case TouchGesture::SWIPE_RIGHT: runAction(page.swipe_right, ActionKind::NONE, false); break;
        case TouchGesture::SWIPE_UP:    runAction(page.swipe_up, ActionKind::NEXT_MODULE, false); break;
        case TouchGesture::SWIPE_DOWN:  runAction(page.swipe_down, ActionKind::PREV_MODULE, false); break;
        default: break;
    }
}
#endif

#if SK_BLE
bool InterfaceTask::inBleMode(int output) {
    return !remote_controlled_
        && nav_screen_ == NavScreen::APP
        && (int)currentPage().turn == output;
}

void InterfaceTask::updateBleOutput() {
    // Messages from the Mac app over Bluetooth
    String line;
    while (ble_.linkReadLine(line)) {
        if (line.length() > 0) handleLinkMessage(line.c_str());
    }

    bool connected = ble_.isConnected();
    if (connected != ble_was_connected_) {
        ble_was_connected_ = connected;
        log(connected ? "Bluetooth: connected" : "Bluetooth: disconnected");
        toast(connected ? LV_SYMBOL_BLUETOOTH " Connected" : LV_SYMBOL_BLUETOOTH " Disconnected");
        sendStatus();
        refreshBluetoothScreen();
    }

    // Wait until the motor has switched to the current mode, so a mode change
    // (which resets the value) isn't mistaken for turning the knob
    if (strcmp(latest_state_.config.text, latest_config_.text) != 0
        || latest_state_.config.position_nonce != latest_config_.position_nonce) {
        return;
    }

    int32_t position = latest_state_.current_position;
    if (page_serial_ != ble_last_config_) {
        ble_last_config_ = page_serial_;
        ble_last_position_ = position;
        ble_scroll_accum_ = 0;
        ble_scroll_last_ms_ = millis();
        ble_volume_resynced_ = false;
        return;
    }

    int32_t steps = position - ble_last_position_;
    ble_last_position_ = position;

    if (inBleMode((int)TurnOutput::VOLUME)) {
        if (steps != 0) {
            ble_.volumeSteps(steps, BLE_FINE_VOLUME);
        }
        ble_volume_level_ = position;

        // Pushing into an end stop sends a full sweep in that direction. The Mac
        // stops at 0% / 100% too, so afterwards the knob and Mac agree again.
        float push = latest_state_.sub_position_unit;
        bool push_low = position == latest_config_.min_position && push < -VOLUME_RESYNC_PUSH;
        bool push_high = position == latest_config_.max_position && push > VOLUME_RESYNC_PUSH;
        if ((push_low || push_high) && !ble_volume_resynced_) {
            ble_volume_resynced_ = true;
            log(push_high ? "Volume: lining up with the Mac at 100%" : "Volume: lining up with the Mac at 0%");
            ble_.volumeSteps(push_high ? 16 : -16, false);  // 16 normal steps = the whole range
        } else if (fabsf(push) < 0.1f) {
            ble_volume_resynced_ = false;
        }
    } else if (inBleMode((int)TurnOutput::SCROLL)) {
        // Spring scroll: speed comes from how far the knob is held off center
        uint32_t now = millis();
        float dt = (now - ble_scroll_last_ms_) / 1000.0f;
        ble_scroll_last_ms_ = now;
        if (dt > 0.1f) {
            dt = 0.1f;
        }
        float deflection = latest_state_.sub_position_unit;
        float beyond = fabsf(deflection) - SCROLL_DEAD_ZONE;
        if (beyond <= 0) {
            ble_scroll_accum_ = 0;
            return;
        }
        float rate = SCROLL_MIN_RATE + SCROLL_EXTRA_RATE * beyond * beyond;
        ble_scroll_accum_ += (deflection > 0 ? 1 : -1) * rate * dt;
        int32_t ticks = (int32_t)ble_scroll_accum_;
        if (ticks != 0) {
            ble_.scrollSteps(ticks);
            ble_scroll_accum_ -= ticks;
        }
    } else if (inBleMode((int)TurnOutput::ARROWS)) {
        if (steps != 0) {
            ble_.arrowSteps(steps);
        }
    } else if (inBleMode((int)TurnOutput::LINK)) {
        if (steps != 0) {
            sendLinkTurn(steps);
        }
    } else if (inBleMode((int)TurnOutput::KEYS)) {
        if (steps != 0) {
            const ProfilePage& page = currentPage();
            sendKey(steps > 0 ? page.turn_cw : page.turn_ccw, steps);
        }
    }
}
#endif

#ifdef PIN_POWER_BUTTON
void InterfaceTask::updatePowerButton() {
    // The button pulls its pin low when pressed
    ButtonEvent event = power_button_.update(digitalRead(PIN_POWER_BUTTON) == LOW, millis());
    switch (event) {
        case ButtonEvent::SHORT_PRESS:
            if (charging_mode_) {
                leaveChargingMode();
                break;
            }
            if (sleep_stage_ == 2) {
                noteActivity();  // screen was off: just wake
                break;
            }
            noteActivity();
            motor_task_.playHaptic(true);
            if (nav_screen_ == NavScreen::APP) {
                openAppsList(0);  // main menu
            } else {
                openApp(current_app_);       // back to the app
            }
            break;
        case ButtonEvent::LONG_PRESS:
            if (charging_mode_) {
                leaveChargingMode();
                break;
            }
            powerOff();
            break;
        default:
            break;
    }
}

void InterfaceTask::powerOff() {
    saveSettings();
    log("Powering off... release the button");
    motor_task_.playHaptic(true);
    delay(150);
    #if SK_DISPLAY
        display_task_->setBrightness(0);
    #endif

    // Let go of the power latch. On battery the power cuts as soon as the
    // button is released (the button itself also holds the power on).
    #ifdef PIN_POWER_HOLD
        digitalWrite(PIN_POWER_HOLD, LOW);
    #endif
    delay(4000);

    // Still running, so USB must be powering the board: carry on as normal
    #ifdef PIN_POWER_HOLD
        digitalWrite(PIN_POWER_HOLD, HIGH);
    #endif
    log("Still powered: on USB the knob can't switch itself off");
}
#endif

#ifdef PIN_BATTERY_ADC
void InterfaceTask::updateBattery() {
    // Read every 5 seconds (after startup settles); log every 30
    static uint32_t last_read;
    static uint32_t last_log;
    uint32_t now = millis();
    if (now < 2000 || (last_read != 0 && now - last_read < 5000)) {
        return;
    }
    last_read = now;

    // Average a burst of readings; the board halves the battery voltage (two 330K resistors)
    uint32_t sum_mv = 0;
    for (uint8_t i = 0; i < 16; i++) {
        sum_mv += analogReadMilliVolts(PIN_BATTERY_ADC);
    }
    uint32_t reading_mv = (sum_mv / 16) * 2;

    // Smooth over time so motor bursts and noise don't make it jump
    battery_mv_ = battery_mv_ == 0 ? reading_mv : (battery_mv_ * 7 + reading_mv * 3) / 10;
    int8_t pct = batteryPercentFromMillivolts(battery_mv_);

    // On battery the level can only go down; it may rise only while plugged in.
    // (Charging pushes the voltage up, so readings while plugged in run high.)
    if (battery_pct_shown_ < 0 || usb_power_.connected() || charging_mode_ || pct < battery_pct_shown_) {
        battery_pct_shown_ = pct;
    }
    if (charging_mode_) {
        showChargingScreen();
    }

    if (last_log == 0 || now - last_log >= 30000) {
        last_log = now;
        snprintf(buf_, sizeof(buf_), "Battery: %.2f V (about %d%%)%s",
            battery_mv_ / 1000.0f, battery_pct_shown_, usb_power_.connected() ? ", charging" : "");
        log(buf_);
    }
    sendStatus();

    // Warn once when it gets low; warn again only after it has been charged back up
    if (!usb_power_.connected() && !charging_mode_ && battery_pct_shown_ <= 15 && !battery_low_warned_) {
        battery_low_warned_ = true;
        toast(LV_SYMBOL_BATTERY_1 " Battery low");
    } else if (battery_pct_shown_ >= 25) {
        battery_low_warned_ = false;
    }
}
#endif

// ======================= Navigation =======================

void InterfaceTask::showUi(const UiScreen& screen) {
    #if SK_DISPLAY
        if (display_task_ != nullptr) {
            display_task_->showScreen(screen);
        }
    #endif
}

void InterfaceTask::setNavScreen(NavScreen screen) {
    if (nav_screen_ == NavScreen::APP && screen != NavScreen::APP) {
        saveSettings();  // remembers the volume level and app
    }
    nav_screen_ = screen;
    nav_list_count_ = 0;
    #if SK_BLE
        ble_last_config_ = -1;  // start fresh when an app is shown again, so nothing is sent by mistake
    #endif
}

int32_t InterfaceTask::selectedIndex() {
    // The knob's position is the highlighted item, once the motor has switched to this menu
    int32_t position = latest_state_.current_position;
    if (strcmp(latest_state_.config.text, latest_config_.text) != 0) {
        position = latest_config_.position;
    }
    if (nav_list_count_ > 0) {
        // Looping list: the position keeps counting up or down, so wrap it into the list
        position = ((position % nav_list_count_) + nav_list_count_) % nav_list_count_;
    }
    return position;
}

void InterfaceTask::sendStatus() {
    UiStatus status;
    #if SK_BLE
        status.bluetooth = ble_.isConnected();
    #endif
    status.usb = usb_power_.connected();
    #ifdef PIN_BATTERY_ADC
        status.battery_pct = battery_pct_shown_;
    #endif
    #if SK_DISPLAY
        if (display_task_ != nullptr) {
            display_task_->showStatus(status);
        }
    #endif
}

void InterfaceTask::toast(const char* text) {
    UiToast message;
    copyText(message.text, text);
    #if SK_DISPLAY
        if (display_task_ != nullptr) {
            display_task_->showToast(message);
        }
    #endif
}

void InterfaceTask::goHome() {
    openApp(0);
}

void InterfaceTask::openApp(int app) {
    app = CLAMP(app, 0, appCount() - 1);
    if (app != current_app_) {
        // A different profile starts at its first mode; returning to the same one keeps your place
        current_mode_ = current_module_ = current_page_ = 0;
        module_pages_.clear();
        view_page_ = 0;
        mode_before_tool_ = module_before_tool_ = 0;
    }
    current_app_ = app;
    settings_.app = current_app_;
    setNavScreen(NavScreen::APP);
    applyAppPage();
    // Catch up with what the Mac last reported (e.g. Lightroom is already in Develop)
    if (link_module_.length() > 0) {
        handleModule(link_module_.c_str());
    }
    if (link_tool_.length() > 0) {
        handleTool(link_tool_.c_str());
    }
}

void InterfaceTask::applyAppPage() {
    const ProfilePage& page = currentPage();
    PB_SmartKnobConfig config = page.feel;

    UiScreen ui;
    const ProfileModule& module = profiles_[current_app_].modes[current_mode_].modules[current_module_];
    bool page_buttons = module.pages.size() > 1 && module.pages.size() <= UI_MAX_ITEMS;
    for (const ProfilePage& p : module.pages) {
        page_buttons = page_buttons && p.short_name.length() > 0;
    }
    const std::vector<PickOption>& actions = edgeButtons();
    bool action_buttons = !actions.empty();
    bool buttons = page_buttons || action_buttons;
    if (action_buttons) {
        // Action buttons round the edge; the one that opens this page is highlighted
        ui.button_count = actions.size();
        ui.button_selected = 255;
        for (size_t i = 0; i < actions.size(); i++) {
            String label = actions[i].label;
            if (label == "%view%") {
                label = module.pages[viewPage(true)].label;
            }
            strncpy(ui.buttons[i], label.c_str(), sizeof(ui.buttons[i]) - 1);
            if (actions[i].action.kind == ActionKind::PAGE && actions[i].action.name.indexOf(page.label) >= 0) {
                ui.button_selected = i;
            }
        }
    }
    if (page.pick.empty()) {
        ui.layout = UiLayout::CONTROL;
        if (action_buttons) {
            // done above
        } else if (page_buttons) {
            // Sliders as buttons round the edge; the middle shows the chosen one
            ui.button_count = module.pages.size();
            ui.button_selected = current_page_;
            for (size_t i = 0; i < module.pages.size(); i++) {
                strncpy(ui.buttons[i], module.pages[i].short_name.c_str(), sizeof(ui.buttons[i]) - 1);
            }
        }
        if (profiles_[current_app_].title_in_center && (!buttons || !page.show_value)) {
            // Profile name in the middle with the page under it; nothing at the top
            snprintf(config.text, sizeof(config.text), "%s\n%s",
                profiles_[current_app_].name.c_str(), page.label.c_str());
        } else {
            copyText(ui.title, profiles_[current_app_].name.c_str());
        }
        ui.hide_value = !page.show_value;
        ui.control_style = (UiControlStyle)page.display;
        if (!page.show_label) {
            config.text[0] = ' ';   // keeps the page-change check working, draws nothing
            config.text[1] = 0;
        }
        if (page_buttons) {
            ui.hide_ring = false;   // the small ring in the middle shows the slider's range
        }
        ui.hide_ring = !page.show_ring;
        strncpy(ui.hint_left, page.hint_left.c_str(), sizeof(ui.hint_left) - 1);
        strncpy(ui.hint_right, page.hint_right.c_str(), sizeof(ui.hint_right) - 1);
        strncpy(ui.hint_up, page.hint_up.c_str(), sizeof(ui.hint_up) - 1);
        strncpy(ui.hint_down, page.hint_down.c_str(), sizeof(ui.hint_down) - 1);
    } else {
        // Picker: the choices in the menu-style list, one click each
        ui.layout = UiLayout::LIST;
        copyText(ui.title, page.label.c_str());
        ui.ring_at_bottom = true;
        ui.item_count = page.pick.size();
        for (size_t i = 0; i < page.pick.size(); i++) {
            copyText(ui.items[i], page.pick[i].label.c_str());
            ui.item_icons[i] = (UiIcon)page.pick[i].icon;
        }
        ui.list_style = (UiListStyle)page.pick_style;
        if (ui.list_style != UiListStyle::DRUM) {
            ui.hide_ring = true;   // the emoji show where you are
        }
        nav_list_count_ = 0;
        int start = 0;
        if (page.pick_value == "rating") {
            start = CLAMP(link_rating_, 0, (int)page.pick.size() - 1);
        } else if (page.pick_value == "label") {
            for (size_t i = 0; i < page.pick.size(); i++) {
                if (page.pick[i].label.equalsIgnoreCase(link_label_.c_str())) start = i;
            }
        }
        config = makeListConfig(page.label.c_str(), page.pick.size(), start, 0);
    }
    showUi(ui);

    #if SK_BLE
        // Volume picks up where it was last left
        if (page.turn == TurnOutput::VOLUME) {
            config.position = ble_volume_level_;
        }
    #endif
    config.position_nonce = ++nav_nonce_;
    page_serial_++;

    snprintf(buf_, sizeof(buf_), "%s: %s", profiles_[current_app_].name.c_str(), config.text);
    log(buf_);
    page_opened_ms_ = millis();
    applyConfig(config, false);
    sendWhere();
    showMediaState();
    if (page.turn == TurnOutput::LINK) {
        link_low_ = link_high_ = 0;
        sendLinkTurn(0);  // "turned 0" = just tell me the current value and range
    }
}

// Main menu: the apps, then Settings, Sleep and Power off
void InterfaceTask::openAppsList(int selected) {
    setNavScreen(NavScreen::APPS_LIST);
    UiScreen ui;
    ui.layout = UiLayout::LIST;
    copyText(ui.title, "Menu");
    ui.ring_at_bottom = true;
    ui.show_status = true;
    // One app to flip to: from a program (Lightroom, Demo) that's Mac; from Mac it's the
    // program the Mac last asked for, if any. Never both.
    bool in_menu_app = menuIndexOf(current_app_) >= 0;
    if (!in_menu_app) {
        menu_flip_app_ = menu_apps_.empty() ? -1 : menu_apps_[0];
    } else {
        menu_flip_app_ = (link_app_ >= 0 && link_app_ != current_app_) ? link_app_ : -1;
    }
    int n = 0;
    if (menu_flip_app_ >= 0) {
        copyText(ui.items[n++], profiles_[menu_flip_app_].name.c_str());
    }
    for (int i = 0; i < MENU_EXTRA_COUNT; i++) {
        copyText(ui.items[n++], menu_extra_names[i]);
    }
    ui.item_count = n;
    showUi(ui);
    PB_SmartKnobConfig config = makeListConfig("Menu", n, selected, ++nav_nonce_);
    applyConfig(config, false);
}

void InterfaceTask::openSettingsList(int selected) {
    setNavScreen(NavScreen::SETTINGS_LIST);
    UiScreen ui;
    ui.layout = UiLayout::LIST;
    copyText(ui.title, "Settings");
    ui.ring_at_bottom = true;
    ui.item_count = SET_COUNT;
    for (int i = 0; i < SET_COUNT; i++) {
        copyText(ui.items[i], setting_names[i]);
    }
    showUi(ui);
    PB_SmartKnobConfig config = makeListConfig("Settings", SET_COUNT, selected, ++nav_nonce_);
    applyConfig(config, false);
}

void InterfaceTask::openSetting(int item) {
    setNavScreen(NavScreen::SETTING);
    open_setting_ = item;

    UiScreen ui;
    copyText(ui.title, item < SET_COUNT ? setting_names[item] : "Devices");
    ui.ring_at_bottom = true;
    PB_SmartKnobConfig config;

    switch (item) {
        case SET_BRIGHTNESS:
            // 10-100% in steps of 5
            ui.layout = UiLayout::VALUE;
            ui.show_number = true;
            ui.number_multiplier = 5;
            copyText(ui.suffix, "%");
            config = makeSliderConfig("Brightness", 2, 20, settings_.brightness_pct / 5, ++nav_nonce_);
            break;
        case SET_HAPTICS:
            // 20-200% in steps of 10; you feel the new strength as you turn
            ui.layout = UiLayout::VALUE;
            ui.show_number = true;
            ui.number_multiplier = 10;
            copyText(ui.suffix, "%");
            config = makeSliderConfig("Click strength", 2, 20, settings_.haptics_pct / 10, ++nav_nonce_);
            break;
        case SET_AUTO_OFF:
            ui.layout = UiLayout::LIST;
            ui.item_count = COUNT_OF(auto_off_names);
            for (int i = 0; i < ui.item_count; i++) {
                copyText(ui.items[i], auto_off_names[i]);
            }
            config = makeListConfig("Auto off", ui.item_count, settings_.auto_off, ++nav_nonce_);
            break;
        case SET_BLUETOOTH:
            fillBluetoothList(ui);
            config = makeListConfig("Bluetooth", ui.item_count, 0, ++nav_nonce_);
            break;
        case SET_BT_DEVICES: {
            ui.layout = UiLayout::VALUE;
            int paired = 0;
            bool connected = false;
            #if SK_BLE
                paired = ble_.pairedCount();
                connected = ble_.isConnected();
            #endif
            snprintf(ui.line1, UI_TEXT_LEN, paired == 1 ? "1 device paired" : "%d devices paired", paired);
            copyText(ui.line2, connected ? "1 connected" : "none connected");
            config = makeStillConfig("Devices", ++nav_nonce_);
            break;
        }
        case SET_CALIBRATE:
            ui.layout = UiLayout::VALUE;
            copyText(ui.line1, "Hands off");
            copyText(ui.line2, "Tap to start");
            config = makeStillConfig("Calibrate", ++nav_nonce_);
            break;
        case SET_BATTERY:
            ui.layout = UiLayout::VALUE;
            #ifdef PIN_BATTERY_ADC
                if (battery_mv_ > 0) {
                    snprintf(ui.line1, UI_TEXT_LEN, "%.2f V", battery_mv_ / 1000.0f);
                    snprintf(ui.line2, UI_TEXT_LEN, "about %d%%%s", battery_pct_shown_, usb_power_.connected() ? ", charging" : "");
                } else {
                    copyText(ui.line1, "Reading...");
                }
            #else
                copyText(ui.line1, "No battery sensor");
            #endif
            config = makeStillConfig("Battery", ++nav_nonce_);
            break;
        default:  // SET_VERSION
            ui.layout = UiLayout::VALUE;
            copyText(ui.line1, FIRMWARE_VERSION);
            copyText(ui.line2, "MaTouch Knob");
            config = makeStillConfig("Version", ++nav_nonce_);
            break;
    }

    showUi(ui);
    applyConfig(config, false);
}

void InterfaceTask::navSelect() {
    int32_t sel = selectedIndex();
    switch (nav_screen_) {
        case NavScreen::APPS_LIST:
            {
            int apps = menu_flip_app_ >= 0 ? 1 : 0;
            if (apps && sel == 0) {
                openApp(menu_flip_app_);
            } else if (sel == apps + MENU_CONTROLS) {
                openControls(apps + MENU_CONTROLS);
            } else if (sel == apps + MENU_SETTINGS) {
                openSettingsList(0);
            } else if (sel == apps + MENU_SLEEP) {
                // Screen off; any touch, turn or button press wakes it
                log("Sleep: screen off (from the menu)");
                last_activity_ms_ = millis() - SLEEP_SCREEN_OFF_MS;
                updateSleep();
            } else {
                #ifdef PIN_POWER_BUTTON
                    powerOff();
                    toast("Plugged in: can't power off");  // only reached when USB keeps it on
                #endif
            }
            break;
            }
        case NavScreen::SETTINGS_LIST:
            if (sel == SET_DEMO) {
                int demo = findProfile("Demo");
                if (demo >= 0) openApp(demo);
                break;
            }
            openSetting(CLAMP(sel, (int32_t)0, (int32_t)SET_COUNT - 1));
            break;
        case NavScreen::SETTING:
            switch (open_setting_) {
                case SET_AUTO_OFF:
                    settings_.auto_off = CLAMP(sel, (int32_t)0, (int32_t)COUNT_OF(auto_off_names) - 1);
                    saveSettings();
                    openSettingsList(SET_AUTO_OFF);
                    break;
                case SET_BLUETOOTH:
                    #if SK_BLE
                        if (sel == 0) {
                            openSetting(SET_BT_DEVICES);
                        } else if (sel == 1) {
                            if (!ble_.isPairing()) {
                                ble_.openPairing(PAIRING_WINDOW_MS);
                                log("Bluetooth: pairing open for 2 minutes");
                            }
                            toast("Open Bluetooth on your Mac");
                            refreshBluetoothScreen();
                        } else if (sel == 2) {
                            log("Bluetooth: forgetting all devices and restarting");
                            ble_.forgetAllDevices();
                            saveSettings();
                            delay(200);
                            ESP.restart();
                        }
                    #endif
                    break;
                case SET_CALIBRATE:
                    log("Calibrating: hands off the knob");
                    motor_task_.runCalibration();
                    openApp(current_app_);  // the app's feel is restored once calibration finishes
                    break;
                default:
                    // Sliders and info screens: tap = done
                    navBack();
                    break;
            }
            break;
        default:
            break;
    }
}

void InterfaceTask::navBack() {
    switch (nav_screen_) {
        case NavScreen::SETTING:
            saveSettings();
            if (open_setting_ == SET_CONTROLS) {
                openAppsList(controls_menu_index_);
            } else if (open_setting_ == SET_BT_DEVICES) {
                openSetting(SET_BLUETOOTH);
            } else {
                openSettingsList(open_setting_);
            }
            break;
        case NavScreen::SETTINGS_LIST:
            openAppsList((menu_flip_app_ >= 0 ? 1 : 0) + MENU_SETTINGS);  // back to the main menu, on Settings
            break;
        case NavScreen::APPS_LIST:
            openApp(current_app_);
            break;
        default:
            break;
    }
}

void InterfaceTask::updateLiveSetting() {
    // Sliders store their value as you turn (brightness changes immediately)
    if (nav_screen_ != NavScreen::SETTING || strcmp(latest_state_.config.text, latest_config_.text) != 0) {
        return;
    }
    int32_t position = latest_state_.current_position;
    if (open_setting_ == SET_BRIGHTNESS) {
        settings_.brightness_pct = CLAMP(position, (int32_t)2, (int32_t)20) * 5;
    } else if (open_setting_ == SET_HAPTICS) {
        uint8_t pct = CLAMP(position, (int32_t)2, (int32_t)20) * 10;
        if (pct != settings_.haptics_pct) {
            // Re-apply this slider with the new strength so you feel it while turning
            settings_.haptics_pct = pct;
            PB_SmartKnobConfig config = latest_config_;
            config.position = position;
            config.sub_position_unit = 0;
            config.position_nonce = ++nav_nonce_;
            applyConfig(config, false);
        }
    }
}

void InterfaceTask::applySettings() {
    #if SK_TOUCH
        ring_mode_ = RingMode::OFF;   // sliding round the rim is off (it got in the way of the buttons)
    #endif
    // Brightness is applied every loop in updateHardware(); haptics in applyConfig()
}

void InterfaceTask::saveSettings() {
    #if SK_BLE
        settings_.volume = ble_volume_level_;
    #endif
    settings_store_.save(settings_);
}

void InterfaceTask::updateUsbPower() {
    // Check a few times a second whether a computer is plugged in
    static uint32_t last_check;
    uint32_t now = millis();
    if (now - last_check < 250) {
        return;
    }
    last_check = now;

    bool was_connected = usb_power_.connected();
    bool connected = usb_power_.update(now);
    if (connected != was_connected) {
        log(connected ? "USB: plugged in (charging)" : "USB: unplugged");
        toast(connected ? LV_SYMBOL_CHARGE " Charging" : LV_SYMBOL_BATTERY_FULL " On battery");
        sendStatus();
    }
}

void InterfaceTask::fillBluetoothList(UiScreen& ui) {
    ui.layout = UiLayout::LIST;
    ui.item_count = 3;
    bool connected = false;
    bool pairing = false;
    uint32_t seconds_left = 0;
    #if SK_BLE
        connected = ble_.isConnected();
        pairing = ble_.isPairing();
        seconds_left = ble_.pairingSecondsLeft(millis());
    #endif
    copyText(ui.items[0], connected ? "Connected" : "Not connected");
    if (pairing && seconds_left > 0) {
        snprintf(ui.items[1], UI_TEXT_LEN, "Pairing %u:%02u", (unsigned)(seconds_left / 60), (unsigned)(seconds_left % 60));
    } else if (pairing) {
        copyText(ui.items[1], "Pairing...");
    } else {
        copyText(ui.items[1], "Add device");
    }
    copyText(ui.items[2], "Forget all");
}

void InterfaceTask::refreshBluetoothScreen() {
    // Redraw the Bluetooth list's text without moving the knob
    if (nav_screen_ != NavScreen::SETTING || open_setting_ != SET_BLUETOOTH) {
        return;
    }
    UiScreen ui;
    copyText(ui.title, setting_names[SET_BLUETOOTH]);
    ui.ring_at_bottom = true;
    fillBluetoothList(ui);
    showUi(ui);
}

// ======================= Sleep =======================

void InterfaceTask::noteActivity() {
    last_activity_ms_ = millis();
    auto_off_tried_ = false;
    if (sleep_stage_ != 0) {
        updateSleep();  // wake right away
    }
}

void InterfaceTask::checkKnobActivity() {
    // Turning the knob (more than a hair) counts as activity
    float width_degrees = latest_state_.config.position_width_radians * 180 / PI;
    float moved_degrees = fabsf((latest_state_.current_position - activity_position_)
        + (latest_state_.sub_position_unit - activity_sub_position_)) * width_degrees;
    if (moved_degrees >= WAKE_TURN_DEGREES) {
        activity_position_ = latest_state_.current_position;
        activity_sub_position_ = latest_state_.sub_position_unit;
        if (sleep_stage_ == 2) {
            #if SK_BLE
                ble_last_config_ = -1;  // the turn that wakes it doesn't send anything
            #endif
        }
        noteActivity();
    }
}

void InterfaceTask::updateSleep() {
    uint32_t now = millis();
    uint32_t idle_ms = now - last_activity_ms_;
    uint8_t stage = idle_ms >= SLEEP_SCREEN_OFF_MS ? 2 : (idle_ms >= SLEEP_DIM_MS ? 1 : 0);

    if (stage != sleep_stage_) {
        if (stage == 2) {
            log("Sleep: screen off, knob relaxed");
            motor_task_.setRelaxed(true);
        } else if (sleep_stage_ == 2) {
            log("Sleep: awake");
            motor_task_.setRelaxed(charging_mode_);  // stays relaxed on the charging screen
        }
        sleep_stage_ = stage;
    }

    // Auto off: only on battery (plugged into a computer it stays on)
    uint32_t minutes = auto_off_minutes[CLAMP(settings_.auto_off, (uint8_t)0, (uint8_t)(COUNT_OF(auto_off_minutes) - 1))];
    if (minutes > 0 && idle_ms >= minutes * 60000 && !usb_power_.connected() && !charging_mode_ && !auto_off_tried_) {
        auto_off_tried_ = true;  // don't keep retrying if it turns out USB is powering it
        log("Sleep: idle too long, powering off");
        #ifdef PIN_POWER_BUTTON
            powerOff();
        #endif
    }
}

// ======================= Charging screen =======================

void InterfaceTask::showChargingScreen() {
    UiScreen ui;
    ui.layout = UiLayout::VALUE;
    copyText(ui.title, "Charging");
    int8_t pct = -1;
    #ifdef PIN_BATTERY_ADC
        pct = battery_pct_shown_;
    #endif
    if (pct >= 99) {
        copyText(ui.line1, LV_SYMBOL_CHARGE " Charged");
    } else if (pct >= 0) {
        snprintf(ui.line1, UI_TEXT_LEN, LV_SYMBOL_CHARGE " %d%%", pct);
    } else {
        copyText(ui.line1, LV_SYMBOL_CHARGE);
    }
    copyText(ui.line2, "Button: start");
    showUi(ui);

    // Nothing to turn: hold nothing (the motor is relaxed anyway)
    if (strcmp(latest_config_.text, "Charging") != 0) {
        PB_SmartKnobConfig config = makeStillConfig("Charging", ++nav_nonce_);
        applyConfig(config, false);
    }
}

void InterfaceTask::leaveChargingMode() {
    // Switch to normal use right here (no restart): hold the power on so it
    // keeps running when unplugged, start Bluetooth and open the last app
    log("Starting up from the charging screen");
    charging_mode_ = false;
    #ifdef PIN_POWER_HOLD
        digitalWrite(PIN_POWER_HOLD, HIGH);
    #endif
    motor_task_.setRelaxed(false);
    #if SK_BLE
        ble_.begin("SmartKnob");
    #endif
    noteActivity();
    openApp(menuIndexOf(settings_.app) >= 0 ? settings_.app : (menu_apps_.empty() ? 0 : menu_apps_[0]));
    sendStatus();
}
