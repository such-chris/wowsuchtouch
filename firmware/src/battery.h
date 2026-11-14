#pragma once

#include <stdint.h>

// Rough charge level of a single-cell lithium battery from its voltage.
// Resting voltage only: readings sag under load and rise while charging.
uint8_t batteryPercentFromMillivolts(uint32_t millivolts);
