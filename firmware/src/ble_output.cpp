#if SK_BLE

#include "ble_output.h"

#include <Arduino.h>

#include <BleCompositeHID.h>
#include <KeyboardDevice.h>
#include <MouseDevice.h>
#include <NimBLEDevice.h>
#include <HIDTypes.h>   // descriptor macros
#include <stdlib.h>

// ---------------- Data link (a vendor report inside the keyboard) ----------------
// macOS hides Bluetooth keyboards from apps, so a separate data service can't be
// reached. Keyboards may carry extra "vendor" reports though, and apps can read
// and write those for one device. One report = 32 bytes of a text line; lines are
// split across reports and padded with zeros.
static const uint8_t LINK_REPORT_ID = 0x50;
static const uint8_t LINK_REPORT_BYTES = 32;

class LinkConfiguration : public BaseCompositeDeviceConfiguration {
    public:
        LinkConfiguration() : BaseCompositeDeviceConfiguration(LINK_REPORT_ID) {}
        const char* getDeviceName() const override { return "Link"; }
        uint8_t getDeviceReportSize() const override { return LINK_REPORT_BYTES; }
        size_t makeDeviceReport(uint8_t* buffer, size_t bufferSize) const override {
            static const uint8_t descriptor[] = {
                USAGE_PAGE(2),      0x60, 0xFF,          // vendor page 0xFF60
                USAGE(1),           0x61,
                COLLECTION(1),      0x01,                // application
                REPORT_ID(1),       LINK_REPORT_ID,
                USAGE(1),           0x62,
                LOGICAL_MINIMUM(1), 0x00,
                LOGICAL_MAXIMUM(2), 0xFF, 0x00,
                REPORT_SIZE(1),     0x08,
                REPORT_COUNT(1),    LINK_REPORT_BYTES,
                HIDINPUT(1),        0x02,                // knob -> Mac
                USAGE(1),           0x63,
                REPORT_COUNT(1),    LINK_REPORT_BYTES,
                HIDOUTPUT(1),       0x02,                // Mac -> knob
                END_COLLECTION(0),
            };
            if (bufferSize < sizeof(descriptor)) return 0;
            memcpy(buffer, descriptor, sizeof(descriptor));
            return sizeof(descriptor);
        }
};

class LinkDevice : public BaseCompositeDevice, public NimBLECharacteristicCallbacks {
    public:
        void init(NimBLEHIDDevice* hid) override {
            NimBLECharacteristic* input = hid->getInputReport(LINK_REPORT_ID);
            NimBLECharacteristic* output = hid->getOutputReport(LINK_REPORT_ID);
            output->setCallbacks(this);
            setCharacteristics(input, output);
            mutex_ = xSemaphoreCreateMutex();
        }
        const BaseCompositeDeviceConfiguration* getDeviceConfig() const override { return &config_; }

        void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
            // Bytes from the Mac (another task: lock). Zeros are padding.
            NimBLEAttValue value = c->getValue();
            xSemaphoreTake(mutex_, portMAX_DELAY);
            for (size_t i = 0; i < value.size(); i++) {
                if (value[i] != 0 && incoming_.length() < 4096) incoming_ += (char)value[i];
            }
            xSemaphoreGive(mutex_);
        }
        void send(const char* line) {
            NimBLECharacteristic* input = getInput();
            if (input == nullptr) return;
            String s(line);
            s += '\n';
            uint8_t report[LINK_REPORT_BYTES];
            for (size_t i = 0; i < s.length(); i += LINK_REPORT_BYTES) {
                memset(report, 0, sizeof(report));
                size_t n = min((size_t)LINK_REPORT_BYTES, s.length() - i);
                memcpy(report, s.c_str() + i, n);
                input->notify(report, sizeof(report));
                delay(2);
            }
        }
        bool readLine(String& line) {
            if (mutex_ == nullptr) return false;
            xSemaphoreTake(mutex_, portMAX_DELAY);
            int nl = incoming_.indexOf('\n');
            if (nl < 0) {
                xSemaphoreGive(mutex_);
                return false;
            }
            line = incoming_.substring(0, nl);
            incoming_.remove(0, nl + 1);
            xSemaphoreGive(mutex_);
            line.trim();
            return true;
        }
    private:
        LinkConfiguration config_;
        SemaphoreHandle_t mutex_ = nullptr;
        String incoming_;
};

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

    link_ = new LinkDevice();
    hid_ = new BleCompositeHID(device_name, "SmartKnob", 100);
    hid_->addDevice(keyboard_);
    hid_->addDevice(mouse_);
    hid_->addDevice(link_);
    hid_->begin();  // starts Bluetooth in the background; see update()
    begin_ms_ = millis();
}

void BleOutput::applyAdvertising() {
    if (!ready_) {
        return;  // applied once Bluetooth has finished starting
    }
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
    bonds_at_open_ = ready_ ? NimBLEDevice::getNumBonds() : 0;
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
    return ready_ ? NimBLEDevice::getNumBonds() : 0;
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

    if (hid_ == nullptr) {
        return PairingEvent::NONE;  // Bluetooth not started (e.g. charging screen)
    }

    if (!ready_) {
        // Wait for the library's background start-up to finish (it starts
        // advertising last), then apply our pairing rule once
        bool started = NimBLEDevice::isInitialized()
            && (NimBLEDevice::getAdvertising()->isAdvertising() || isConnected() || now_ms - begin_ms_ > 3000);
        if (!started) {
            return PairingEvent::NONE;
        }
        ready_ = true;
        if (NimBLEDevice::getNumBonds() == 0) {
            openPairing(0);  // nothing paired yet: stay open until the first device pairs
        } else {
            bonds_at_open_ = NimBLEDevice::getNumBonds();
            applyAdvertising();
        }
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

void BleOutput::sendKey(uint8_t modifiers, uint8_t key, int32_t times) {
    if (!isConnected() || key == 0) return;
    releaseModifiers();
    times = limitSteps(abs(times));
    for (int32_t i = 0; i < times; i++) {
        if (modifiers) keyboard_->modifierKeyPress(modifiers);
        keyboard_->keyPress(key);
        keyboard_->sendKeyReport();
        keyboard_->keyRelease(key);
        if (modifiers) keyboard_->modifierKeyRelease(modifiers);
        keyboard_->sendKeyReport();
    }
}

bool BleOutput::linkSubscribed() {
    return link_ != nullptr && isConnected();
}

void BleOutput::linkSend(const char* line) {
    if (link_ != nullptr) link_->send(line);
}

bool BleOutput::linkReadLine(String& line) {
    return link_ != nullptr && link_->readLine(line);
}

void BleOutput::sendMediaKey(uint32_t media_key) {
    if (!isConnected() || media_key == 0) return;
    releaseModifiers();
    tapMediaKey(media_key);
}

void BleOutput::forgetAllDevices() {
    if (!ready_) {
        return;
    }
    NimBLEDevice::deleteAllBonds();
    openPairing(0);  // nothing paired any more, so stay open for a new device
}

#endif // SK_BLE
