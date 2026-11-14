#pragma once

// Tuning for the 3205B gimbal motor in the Makerfabs MaTouch knob.
// Values from Makerfabs' firmware (example/matouch_knob/motor_task.h).

#define FOC_PID_P 2
#define FOC_PID_I 0
#define FOC_PID_D 0.08
#define FOC_PID_OUTPUT_RAMP 10000
#define FOC_PID_LIMIT 10

#define FOC_VOLTAGE_LIMIT 5
