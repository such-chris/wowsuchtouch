#pragma once

#include <Arduino.h>

// Minimal driver for the CST816S capacitive touch chip (one finger only).
// Based on Makerfabs' MaTouch example (example/matouch_knob/touch.cpp).
class CST816STouch {
    public:
        // Start the I2C bus the chip is on. Call once before read().
        void begin(int pin_sda, int pin_scl);

        // Returns true while a finger is on the screen, and sets x/y to its
        // position in screen pixels (0-239). Returns false otherwise.
        bool read(uint16_t& x, uint16_t& y);

    private:
        static const uint8_t I2C_ADDRESS = 0x15;
};
