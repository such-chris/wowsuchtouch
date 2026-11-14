#if SK_BLE

#include "ble_output.h"

#include <Arduino.h>

#include <BleCompositeHID.h>
#include <KeyboardDevice.h>
#include <MouseDevice.h>
#include <NimBLEDevice.h>
#include <stdlib.h>

// Only let paired devices connect when not pairing. If a paired Mac/iPad ever
// can't reconnect, set this to false (or use Settings > Bluetooth > Add device).
static const bool PAIRED_ONLY = true;

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

    // Nothing paired yet: stay open until the first device pairs
    if (NimBLEDevice::getNumBonds() == 0) {
        openPairing(0);
    } else {
        applyAdvertising();
    }
}

void BleOutput::applyAdvertising() {
    // Rebuild the allowed list from the devices we've paired with
    while (NimBLEDevice::getWhiteListCount() > 0) {
        NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0));
    }
    int bonds = NimBLEDevice::getNumBonds();
    for (int i = 0; i < bonds; i++) {
        NimBLEDevice::whiteListAdd(NimBLEDevice::getBondedAddress(i));
    }

    // Restart the "I'm here" signal with the new rule
    bool paired_only = PAIRED_ONLY && !pairing_ && bonds > 0;
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->stop();
    advertising->setScanFilter(false, paired_only);
    advertising->start();
}

void BleOutput::openPairing(uint32_t duration_ms) {
    pairing_ = true;
    bonds_at_open_ = NimBLEDevice::getNumBonds();
    pairing_end_ms_ = duration_ms == 0 ? 0 : millis() + duration_ms;
    if (pairing_end_ms_ == 0 && duration_ms != 0) {
        pairing_end_ms_ = 1;  // (0 means "no time limit")
    }
    applyAdvertising();
}

uint32_t BleOutput::pairingSecondsLeft(uint32_t now_ms) const {
    if (!pairing_ || pairing_end_ms_ == 0) {
        return 0;
    }
    int32_t left_ms = (int32_t)(pairing_end_ms_ - now_ms);
    return left_ms > 0 ? (left_ms + 999) / 1000 : 0;
}

int BleOutput::pairedCount() {
    return NimBLEDevice::getNumBonds();
}

bool BleOutput::isConnected() {
    return hid_ != nullptr && hid_->isConnected();
}

// Let go of held modifiers after the knob has been still this long
static const uint32_t MODIFIER_RELEASE_MS = 600;

void BleOutput::holdModifiers(uint8_t modifiers) {
    if (modifiers != held_modifiers_) {
        releaseModifiers();
        if (modifiers) {
            keyboard_->modifierKeyPress(modifiers);
            keyboard_->sendKeyReport();
            held_modifiers_ = modifiers;
        }
    }
}

void BleOutput::releaseModifiers() {
    if (held_modifiers_) {
        keyboard_->modifierKeyRelease(held_modifiers_);
        keyboard_->sendKeyReport();
        held_modifiers_ = 0;
    }
}

PairingEvent BleOutput::update(uint32_t now_ms) {
    if (held_modifiers_ && now_ms - last_hold_use_ms_ > MODIFIER_RELEASE_MS) {
        releaseModifiers();
    }

    if (pairing_) {
        bool new_device = NimBLEDevice::getNumBonds() > bonds_at_open_;
        bool timed_out = pairing_end_ms_ != 0 && (int32_t)(now_ms - pairing_end_ms_) >= 0;
        if (new_device || timed_out) {
            pairing_ = false;
            applyAdvertising();
            return new_device ? PairingEvent::PAIRED : PairingEvent::CLOSED;
        }
    }
    return PairingEvent::NONE;
}

void BleOutput::tapMediaKey(uint32_t media_key) {
    keyboard_->mediaKeyPress(media_key);
    keyboard_->sendMediaKeyReport();
    keyboard_->mediaKeyRelease(media_key);
    keyboard_->sendMediaKeyReport();
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
    holdModifiers(fine ? (KEY_MOD_LSHIFT | KEY_MOD_LALT) : 0);
    last_hold_use_ms_ = millis();
    for (int32_t i = 0; i < abs(steps); i++) {
        tapMediaKey(key);
    }
}

void BleOutput::scrollSteps(int32_t steps) {
    if (!isConnected()) return;
    releaseModifiers();  // a held Shift would turn this into sideways scrolling
    steps = limitSteps(steps);
    // Clockwise scrolls down the page (wheel value is negative for "down")
    mouse_->mouseMove(0, 0, 0, (signed char)(-steps));
    mouse_->sendMouseReport();
    mouse_->mouseMove(0, 0, 0, 0);
}

void BleOutput::arrowSteps(int32_t steps) {
    if (!isConnected()) return;
    releaseModifiers();
    steps = limitSteps(steps);
    uint8_t key = steps > 0 ? KEY_RIGHT : KEY_LEFT;
    for (int32_t i = 0; i < abs(steps); i++) {
        tapKey(key);
    }
}

void BleOutput::forgetAllDevices() {
    NimBLEDevice::deleteAllBonds();
    openPairing(0);  // nothing paired any more, so stay open for a new device
}

#endif // SK_BLE
