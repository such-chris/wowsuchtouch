#include "cst816s_touch.h"

#include <Wire.h>

void CST816STouch::begin(int pin_sda, int pin_scl) {
    Wire.begin(pin_sda, pin_scl);
    Wire.setClock(400000);
}

bool CST816STouch::read(uint16_t& x, uint16_t& y) {
    // Ask for 5 bytes starting at register 0x02:
    //   [0] number of fingers
    //   [1] event (top 2 bits) + X high bits
    //   [2] X low byte
    //   [3] Y high bits
    //   [4] Y low byte
    Wire.beginTransmission(I2C_ADDRESS);
    Wire.write(0x02);
    if (Wire.endTransmission(true) != 0) {
        // No answer: the chip naps when idle. Treat as "no finger".
        return false;
    }
    if (Wire.requestFrom((uint16_t)I2C_ADDRESS, (size_t)5, true) != 5) {
        return false;
    }

    uint8_t data[5];
    for (uint8_t i = 0; i < 5; i++) {
        data[i] = Wire.read();
    }

    uint8_t fingers = data[0];
    uint8_t event = data[1] >> 6; // 0 = just touched, 1 = lifted, 2 = still touching
    if (fingers == 0 || event == 1) {
        return false;
    }

    x = ((data[1] & 0x0F) << 8) | data[2];
    y = ((data[3] & 0x0F) << 8) | data[4];
    return true;
}
