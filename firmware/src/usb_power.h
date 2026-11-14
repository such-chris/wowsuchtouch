#pragma once

#include <stdint.h>

// Notices when the knob is plugged into a computer.
//
// The ESP32-S3's built-in USB port counts "start of frame" signals, which a
// computer sends every millisecond. If that count is moving, a computer is
// connected (and the battery is charging). A plain wall charger sends no
// frames, so it can't be detected this way.
class UsbPower {
    public:
        // Call every few hundred milliseconds. Returns true while a computer is connected.
        bool update(uint32_t now_ms);
        bool connected() const { return connected_; }

    private:
        bool connected_ = false;
        uint32_t last_frame_ = 0;
        uint32_t last_change_ms_ = 0;
};
