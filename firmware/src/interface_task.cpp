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
// What each Bluetooth mode sends when the knob's value changes.
// These modes are the first entries in configs[] below, in this order.
enum class KnobOutput { VOLUME, SCROLL, ARROWS };
static const KnobOutput ble_mode_outputs[] = { KnobOutput::VOLUME, KnobOutput::SCROLL, KnobOutput::ARROWS };
static const bool BLE_FINE_VOLUME = true;  // Shift+Option quarter steps on macOS

// Spring scroll tuning (deflection is in "full deflections", i.e. 1.0 = 60 degrees)
static const float SCROLL_DEAD_ZONE = 0.08;      // ignore tiny wobbles around center
static const float SCROLL_MIN_RATE = 6;          // wheel ticks per second just past the dead zone
static const float SCROLL_EXTRA_RATE = 120;      // added at full deflection (grows with the square)
static const float RIM_SCROLL_DEGREES = 5;       // rim travel per wheel tick in Scroll mode

// Volume: pushing past an end stop by this much (in steps) re-syncs with the Mac
static const float VOLUME_RESYNC_PUSH = 0.3;
#endif

static PB_SmartKnobConfig configs[] = {
    // int32_t position;
    // float sub_position_unit;
    // uint8_t position_nonce;
    // int32_t min_position;
    // int32_t max_position;
    // float position_width_radians;
    // float detent_strength_unit;
    // float endstop_strength_unit;
    // float snap_point;
    // char text[51];
    // pb_size_t detent_positions_count;
    // int32_t detent_positions[5];
    // float snap_point_bias;
    // int8_t led_hue;

#if SK_BLE
    {
        32, // starts mid-way; push into either end stop to line up with the Mac
        0,
        0,
        0,
        64, // the Mac's 64 quarter steps of volume
        5 * PI / 180,
        0.8,
        1,
        1.1,
        "Volume",
        0,
        {},
        0,
        0,
    },
    {
        // Spring-return: a single position, so any turn pushes against the end-stop
        // force, which grows the further you turn. How far = how fast it scrolls.
        0,
        0,
        0,
        0,
        0,
        60 * PI / 180,  // 60 degrees of turn = "full" deflection
        0,
        1,              // spring strength
        1.1,
        "Scroll",
        0,
        {},
        0,
        0,
    },
    {
        0,
        0,
        0,
        0,
        -1,
        15 * PI / 180,
        1,
        1,
        1.1,
        "Arrows\nLeft / Right",
        0,
        {},
        0,
        0,
    },
#endif
    {
        0,
        0,
        0,
        0,
        -1, // max position < min position indicates no bounds
        10 * PI / 180,
        0,
        1,
        1.1,
        "Unbounded\nNo detents",
        0,
        {},
        0,
        200,
    },
    {
        0,
        0,
        1,
        0,
        10,
        10 * PI / 180,
        0,
        1,
        1.1,
        "Bounded 0-10\nNo detents",
        0,
        {},
        0,
        0,
    },
    {
        0,
        0,
        2,
        0,
        72,
        10 * PI / 180,
        0,
        1,
        1.1,
        "Multi-rev\nNo detents",
        0,
        {},
        0,
        73,
    },
    {
        0,
        0,
        3,
        0,
        1,
        60 * PI / 180,
        1,
        1,
        0.55, // Note the snap point is slightly past the midpoint (0.5); compare to normal detents which use a snap point *past* the next value (i.e. > 1)
        "On/off\nStrong detent",
        0,
        {},
        0,
        157,
    },
    {
        0,
        0,
        4,
        0,
        0,
        60 * PI / 180,
        0.01,
        0.6,
        1.1,
        "Return-to-center",
        0,
        {},
        0,
        45,
    },
    {
        127,
        0,
        5,
        0,
        255,
        1 * PI / 180,
        0,
        1,
        1.1,
        "Fine values\nNo detents",
        0,
        {},
        0,
        219,
    },
    {
        127,
        0,
        5,
        0,
        255,
        1 * PI / 180,
        1,
        1,
        1.1,
        "Fine values\nWith detents",
        0,
        {},
        0,
        25,
    },
    {
        0,
        0,
        6,
        0,
        31,
        8.225806452 * PI / 180,
        2,
        1,
        1.1,
        "Coarse values\nStrong detents",
        0,
        {},
        0,
        200,
    },
    {
        0,
        0,
        6,
        0,
        31,
        8.225806452 * PI / 180,
        0.2,
        1,
        1.1,
        "Coarse values\nWeak detents",
        0,
        {},
        0,
        0,
    },
    {
        0,
        0,
        7,
        0,
        31,
        7 * PI / 180,
        2.5,
        1,
        0.7,
        "Magnetic detents",
        4,
        {2, 10, 21, 22},
        0,
        73,
    },
    {
        0,
        0,
        8,
        -6,
        6,
        60 * PI / 180,
        1,
        1,
        0.55,
        "Return-to-center\nwith detents",
        0,
        {},
        0.4,
        157,
    },
};

// ---- Apps: each app is a group of modes (pages) from configs[] above ----
struct AppDef {
    const char* name;
    int first_config;   // index of its first page in configs[]
    int config_count;   // how many pages
};
#if SK_BLE
static const int MAC_PAGE_COUNT = COUNT_OF(ble_mode_outputs);
static const AppDef apps[] = {
    {"Mac", 0, MAC_PAGE_COUNT},                                      // Volume, Scroll, Arrows
    {"Demo", MAC_PAGE_COUNT, (int)COUNT_OF(configs) - MAC_PAGE_COUNT},  // Scott's haptic demos
};
#else
static const AppDef apps[] = {
    {"Demo", 0, (int)COUNT_OF(configs)},
};
#endif
static const int APP_COUNT = COUNT_OF(apps);

// ---- Settings list ----
enum SettingItem {
    SET_BRIGHTNESS,
    SET_HAPTICS,
    SET_TOUCH,
    SET_BLUETOOTH,
    SET_CALIBRATE,
    SET_BATTERY,
    SET_VERSION,
    SET_COUNT,
};
static const char* setting_names[SET_COUNT] = {
    "Brightness", "Haptics", "Touch", "Bluetooth", "Calibrate", "Battery", "Version",
};
static const char* touch_mode_names[] = {"Track", "Follow", "Off"};

// How far from the top / bottom edge a swipe must start to open Settings / Apps (pixels)
static const uint16_t EDGE_SWIPE_PX = 60;

// ---- Knob feel for menus ----
// A list: one firm click per item, end stops at the first and last item
static PB_SmartKnobConfig makeListConfig(const char* id, int count, int selected, uint8_t nonce) {
    PB_SmartKnobConfig config = {};
    config.position = selected;
    config.position_nonce = nonce;
    config.min_position = 0;
    config.max_position = count - 1;
    config.position_width_radians = 24 * PI / 180;
    config.detent_strength_unit = 1.5;
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
    config.detent_strength_unit = 0.7;
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
        ble_.begin("SmartKnob");
        log("Bluetooth: pair with \"SmartKnob\" in your Mac's Bluetooth settings");
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

    // Restore saved settings and open the last-used app
    settings_ = settings_store_.load();
    applySettings();
    #if SK_BLE
        ble_volume_level_ = CLAMP(settings_.volume, (int32_t)0, (int32_t)64);
    #endif
    openApp(settings_.app < APP_COUNT ? settings_.app : 0);
    motor_task_.addListener(knob_state_queue_);

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

void InterfaceTask::changeConfig(bool next) {
    // Next / previous page within the current app (wraps around)
    if (nav_screen_ != NavScreen::APP) {
        return;
    }
    const AppDef& app = apps[current_app_];
    int page = current_config_ - app.first_config;
    page = (page + (next ? 1 : -1) + app.config_count) % app.config_count;
    current_config_ = app.first_config + page;
    applyAppPage();
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
            if (inBleMode((int)KnobOutput::SCROLL)) {
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

    // These work everywhere: edge swipes open Settings / Apps, long press goes Home
    uint16_t start_y = touch_gestures_.gestureY();
    if (gesture == TouchGesture::SWIPE_DOWN && start_y < EDGE_SWIPE_PX) {
        motor_task_.playHaptic(false);
        openSettingsList(0);
        return;
    }
    if (gesture == TouchGesture::SWIPE_UP && start_y > TFT_HEIGHT - EDGE_SWIPE_PX) {
        motor_task_.playHaptic(false);
        openAppsList();
        return;
    }
    if (gesture == TouchGesture::LONG_PRESS) {
        motor_task_.playHaptic(true);  // felt while the finger is still down
        goHome();
        return;
    }

    if (nav_screen_ != NavScreen::APP) {
        // In a menu: tap opens / chooses; double tap or swipe right goes back
        if (gesture == TouchGesture::TAP) {
            motor_task_.playHaptic(true);
            navSelect();
        } else if (gesture == TouchGesture::DOUBLE_TAP) {
            motor_task_.playHaptic(true);
            delay(80);
            motor_task_.playHaptic(true);
            navBack();
        } else if (gesture == TouchGesture::SWIPE_RIGHT) {
            motor_task_.playHaptic(false);
            navBack();
        }
        return;
    }

    // In an app: tap / swipe left = next page, double tap / swipe right = previous page
    switch (gesture) {
        case TouchGesture::TAP:
            motor_task_.playHaptic(true);
            changeConfig(true);
            break;
        case TouchGesture::DOUBLE_TAP:
            motor_task_.playHaptic(true);
            delay(80);
            motor_task_.playHaptic(true);
            changeConfig(false);
            break;
        case TouchGesture::SWIPE_LEFT:
            motor_task_.playHaptic(false);
            changeConfig(true);
            break;
        case TouchGesture::SWIPE_RIGHT:
            motor_task_.playHaptic(false);
            changeConfig(false);
            break;
        default:
            // Triple tap, and swipes up/down away from the edges: free for app-specific actions
            break;
    }
}
#endif

#if SK_BLE
bool InterfaceTask::inBleMode(int output) {
    return !remote_controlled_
        && nav_screen_ == NavScreen::APP
        && current_config_ < (int)COUNT_OF(ble_mode_outputs)
        && (int)ble_mode_outputs[current_config_] == output;
}

void InterfaceTask::updateBleOutput() {
    bool connected = ble_.isConnected();
    if (connected != ble_was_connected_) {
        ble_was_connected_ = connected;
        log(connected ? "Bluetooth: connected" : "Bluetooth: disconnected");
    }

    // Wait until the motor has switched to the current mode, so a mode change
    // (which resets the value) isn't mistaken for turning the knob
    if (strcmp(latest_state_.config.text, latest_config_.text) != 0) {
        return;
    }

    int32_t position = latest_state_.current_position;
    if (current_config_ != ble_last_config_) {
        ble_last_config_ = current_config_;
        ble_last_position_ = position;
        ble_scroll_accum_ = 0;
        ble_scroll_last_ms_ = millis();
        ble_volume_resynced_ = false;
        return;
    }

    int32_t steps = position - ble_last_position_;
    ble_last_position_ = position;

    if (inBleMode((int)KnobOutput::VOLUME)) {
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
    } else if (inBleMode((int)KnobOutput::SCROLL)) {
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
    } else if (inBleMode((int)KnobOutput::ARROWS)) {
        if (steps != 0) {
            ble_.arrowSteps(steps);
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
            log("Button: short press (no action yet)");
            break;
        case ButtonEvent::LONG_PRESS:
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
    // Read at startup (after things settle) and then every 30 seconds
    static uint32_t last_read;
    uint32_t now = millis();
    if (now < 2000 || (last_read != 0 && now - last_read < 30000)) {
        return;
    }
    last_read = now;

    // Average a few readings; the board halves the battery voltage (two 330K resistors)
    uint32_t sum_mv = 0;
    for (uint8_t i = 0; i < 16; i++) {
        sum_mv += analogReadMilliVolts(PIN_BATTERY_ADC);
    }
    battery_mv_ = (sum_mv / 16) * 2;

    snprintf(buf_, sizeof(buf_), "Battery: %.2f V (about %d%%)",
        battery_mv_ / 1000.0f, batteryPercentFromMillivolts(battery_mv_));
    log(buf_);
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
    #if SK_BLE
        ble_last_config_ = -1;  // start fresh when an app is shown again, so nothing is sent by mistake
    #endif
}

int32_t InterfaceTask::selectedIndex() {
    // The knob's position is the highlighted item, once the motor has switched to this menu
    if (strcmp(latest_state_.config.text, latest_config_.text) != 0) {
        return latest_config_.position;
    }
    return latest_state_.current_position;
}

void InterfaceTask::goHome() {
    openApp(0);
}

void InterfaceTask::openApp(int app) {
    current_app_ = CLAMP(app, 0, APP_COUNT - 1);
    settings_.app = current_app_;
    setNavScreen(NavScreen::APP);
    const AppDef& def = apps[current_app_];
    if (current_config_ < def.first_config || current_config_ >= def.first_config + def.config_count) {
        current_config_ = def.first_config;
    }
    applyAppPage();
}

void InterfaceTask::applyAppPage() {
    UiScreen ui;
    ui.layout = UiLayout::CONTROL;
    showUi(ui);

    #if SK_BLE
        // Volume picks up where it was last left
        if (current_config_ < (int)COUNT_OF(ble_mode_outputs) && ble_mode_outputs[current_config_] == KnobOutput::VOLUME) {
            configs[current_config_].position = ble_volume_level_;
        }
    #endif
    configs[current_config_].position_nonce = ++nav_nonce_;

    snprintf(buf_, sizeof(buf_), "%s: %s", apps[current_app_].name, configs[current_config_].text);
    log(buf_);
    applyConfig(configs[current_config_], false);
}

void InterfaceTask::openAppsList() {
    setNavScreen(NavScreen::APPS_LIST);
    UiScreen ui;
    ui.layout = UiLayout::LIST;
    copyText(ui.title, "Apps");
    ui.item_count = APP_COUNT;
    for (int i = 0; i < APP_COUNT; i++) {
        copyText(ui.items[i], apps[i].name);
    }
    showUi(ui);
    PB_SmartKnobConfig config = makeListConfig("Apps", APP_COUNT, current_app_, ++nav_nonce_);
    applyConfig(config, false);
}

void InterfaceTask::openSettingsList(int selected) {
    setNavScreen(NavScreen::SETTINGS_LIST);
    UiScreen ui;
    ui.layout = UiLayout::LIST;
    copyText(ui.title, "Settings");
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
    copyText(ui.title, setting_names[item]);
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
            // 20-200% in steps of 10; takes effect when you leave
            ui.layout = UiLayout::VALUE;
            ui.show_number = true;
            ui.number_multiplier = 10;
            copyText(ui.suffix, "%");
            config = makeSliderConfig("Haptics", 2, 20, settings_.haptics_pct / 10, ++nav_nonce_);
            break;
        case SET_TOUCH:
            ui.layout = UiLayout::LIST;
            ui.item_count = COUNT_OF(touch_mode_names);
            for (int i = 0; i < ui.item_count; i++) {
                copyText(ui.items[i], touch_mode_names[i]);
            }
            config = makeListConfig("Touch", ui.item_count, settings_.touch_mode, ++nav_nonce_);
            break;
        case SET_BLUETOOTH: {
            bool connected = false;
            #if SK_BLE
                connected = ble_.isConnected();
            #endif
            ui.layout = UiLayout::LIST;
            ui.item_count = 2;
            copyText(ui.items[0], connected ? "Connected" : "Not connected");
            copyText(ui.items[1], "Forget all");
            config = makeListConfig("Bluetooth", 2, 0, ++nav_nonce_);
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
                    snprintf(ui.line2, UI_TEXT_LEN, "about %d%%", batteryPercentFromMillivolts(battery_mv_));
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
            openApp(sel);
            break;
        case NavScreen::SETTINGS_LIST:
            openSetting(CLAMP(sel, (int32_t)0, (int32_t)SET_COUNT - 1));
            break;
        case NavScreen::SETTING:
            switch (open_setting_) {
                case SET_TOUCH:
                    settings_.touch_mode = CLAMP(sel, (int32_t)0, (int32_t)2);
                    applySettings();
                    saveSettings();
                    openSettingsList(SET_TOUCH);
                    break;
                case SET_BLUETOOTH:
                    if (sel == 1) {
                        #if SK_BLE
                            log("Bluetooth: forgetting all devices and restarting");
                            ble_.forgetAllDevices();
                            saveSettings();
                            delay(200);
                            ESP.restart();
                        #endif
                    }
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
            if (open_setting_ == SET_HAPTICS) {
                applySettings();  // new strength takes effect from here on
            }
            saveSettings();
            openSettingsList(open_setting_);
            break;
        case NavScreen::APPS_LIST:
        case NavScreen::SETTINGS_LIST:
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
        settings_.haptics_pct = CLAMP(position, (int32_t)2, (int32_t)20) * 10;
    }
}

void InterfaceTask::applySettings() {
    #if SK_TOUCH
        RingMode modes[] = {RingMode::TRACK, RingMode::FOLLOW, RingMode::OFF};
        ring_mode_ = modes[CLAMP(settings_.touch_mode, (uint8_t)0, (uint8_t)2)];
    #endif
    // Brightness is applied every loop in updateHardware(); haptics in applyConfig()
}

void InterfaceTask::saveSettings() {
    #if SK_BLE
        settings_.volume = ble_volume_level_;
    #endif
    settings_store_.save(settings_);
}
