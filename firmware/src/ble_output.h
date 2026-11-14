#pragma once

#include <stdint.h>

class BleCompositeHID;
class KeyboardDevice;
class MouseDevice;

// The knob as a Bluetooth keyboard + media keys + mouse wheel, all in one
// device. Pairs with a Mac or iPad like any Bluetooth keyboard.
class BleOutput {
    public:
        void begin(const char* device_name);
        bool isConnected();

        // Positive steps = up / right / clockwise.
        void volumeSteps(int32_t steps, bool fine);  // fine = Shift+Option quarter steps (macOS)
        void scrollSteps(int32_t steps);             // mouse wheel ticks
        void arrowSteps(int32_t steps);              // right / left arrow keys

        // Forget every paired Mac/iPad (they'll need to pair again)
        void forgetAllDevices();

    private:
        void tapMediaKey(uint32_t media_key, uint8_t modifiers);
        void tapKey(uint8_t key);

        BleCompositeHID* hid_ = nullptr;
        KeyboardDevice* keyboard_ = nullptr;
        MouseDevice* mouse_ = nullptr;
};
