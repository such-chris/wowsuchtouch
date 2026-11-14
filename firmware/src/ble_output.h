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

        // Call often: lets go of held modifier keys once the knob has been still a moment
        void update(uint32_t now_ms);

    private:
        // Fine volume needs Shift+Option held. Pressing them for every click makes
        // rapid Option taps, which apps can read as a shortcut (e.g. "double-tap Option"
        // opens Claude). So they're held down for a whole burst and let go after a pause.
        void holdModifiers(uint8_t modifiers);
        void releaseModifiers();
        uint8_t held_modifiers_ = 0;
        uint32_t last_hold_use_ms_ = 0;

        void tapMediaKey(uint32_t media_key);
        void tapKey(uint8_t key);

        BleCompositeHID* hid_ = nullptr;
        KeyboardDevice* keyboard_ = nullptr;
        MouseDevice* mouse_ = nullptr;
};
