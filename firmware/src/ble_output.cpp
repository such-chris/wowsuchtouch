#if SK_BLE

#include "ble_output.h"

#include <BleCompositeHID.h>
#include <KeyboardDevice.h>
#include <MouseDevice.h>
#include <stdlib.h>

// Most key repeats sent for one update, so a fast spin can't flood the connection
static const int32_t MAX_STEPS_PER_UPDATE = 20;

static int32_t limitSteps(int32_t steps) {
    if (steps > MAX_STEPS_PER_UPDATE) return MAX_STEPS_PER_UPDATE;
    if (steps < -MAX_STEPS_PER_UPDATE) return -MAX_STEPS_PER_UPDATE;
    return steps;
}

void BleOutput::begin(const char* device_name) {
    // We send reports ourselves (autoReport off), so each key is one clean press + release
    KeyboardConfiguration keyboard_config;
    keyboard_config.setAutoReport(false);
    keyboard_config.setUseMediaKeys(true);
    keyboard_ = new KeyboardDevice(keyboard_config);

    MouseConfiguration mouse_config;
    mouse_config.setAutoReport(false);
    mouse_ = new MouseDevice(mouse_config);

    hid_ = new BleCompositeHID(device_name, "SmartKnob", 100);
    hid_->addDevice(keyboard_);
    hid_->addDevice(mouse_);
    hid_->begin();
}

bool BleOutput::isConnected() {
    return hid_ != nullptr && hid_->isConnected();
}

void BleOutput::tapMediaKey(uint32_t media_key, uint8_t modifiers) {
    if (modifiers) {
        keyboard_->modifierKeyPress(modifiers);
        keyboard_->sendKeyReport();
    }
    keyboard_->mediaKeyPress(media_key);
    keyboard_->sendMediaKeyReport();
    keyboard_->mediaKeyRelease(media_key);
    keyboard_->sendMediaKeyReport();
    if (modifiers) {
        keyboard_->modifierKeyRelease(modifiers);
        keyboard_->sendKeyReport();
    }
}

void BleOutput::tapKey(uint8_t key) {
    keyboard_->keyPress(key);
    keyboard_->sendKeyReport();
    keyboard_->keyRelease(key);
    keyboard_->sendKeyReport();
}

void BleOutput::volumeSteps(int32_t steps, bool fine) {
    if (!isConnected()) return;
    steps = limitSteps(steps);
    uint32_t key = steps > 0 ? KEY_MEDIA_VOLUMEUP : KEY_MEDIA_VOLUMEDOWN;
    uint8_t modifiers = fine ? (KEY_MOD_LSHIFT | KEY_MOD_LALT) : 0;
    for (int32_t i = 0; i < abs(steps); i++) {
        tapMediaKey(key, modifiers);
    }
}

void BleOutput::scrollSteps(int32_t steps) {
    if (!isConnected()) return;
    steps = limitSteps(steps);
    // Clockwise scrolls down the page (wheel value is negative for "down")
    mouse_->mouseMove(0, 0, 0, (signed char)(-steps));
    mouse_->sendMouseReport();
    mouse_->mouseMove(0, 0, 0, 0);
}

void BleOutput::arrowSteps(int32_t steps) {
    if (!isConnected()) return;
    steps = limitSteps(steps);
    uint8_t key = steps > 0 ? KEY_RIGHT : KEY_LEFT;
    for (int32_t i = 0; i < abs(steps); i++) {
        tapKey(key);
    }
}

#endif // SK_BLE
