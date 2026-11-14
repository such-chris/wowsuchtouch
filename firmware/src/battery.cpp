#include "battery.h"

// Typical lithium-polymer discharge curve (millivolts -> percent)
static const uint16_t CURVE_MV[]  = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
static const uint8_t  CURVE_PCT[] = {   0,    3,    8,   20,   42,   62,   79,   90,  100};
static const int CURVE_POINTS = sizeof(CURVE_MV) / sizeof(CURVE_MV[0]);

uint8_t batteryPercentFromMillivolts(uint32_t millivolts) {
    if (millivolts <= CURVE_MV[0]) return 0;
    if (millivolts >= CURVE_MV[CURVE_POINTS - 1]) return 100;
    for (int i = 1; i < CURVE_POINTS; i++) {
        if (millivolts <= CURVE_MV[i]) {
            // Straight line between the two nearest points
            uint32_t span_mv = CURVE_MV[i] - CURVE_MV[i - 1];
            uint32_t into_mv = millivolts - CURVE_MV[i - 1];
            return CURVE_PCT[i - 1] + (CURVE_PCT[i] - CURVE_PCT[i - 1]) * into_mv / span_mv;
        }
    }
    return 100;
}
