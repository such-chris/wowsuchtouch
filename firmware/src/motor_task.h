#pragma once

#include <Arduino.h>
#include <SimpleFOC.h>
#include <vector>

#include "configuration.h"
#include "logger.h"
#include "proto_gen/smartknob.pb.h"
#include "task.h"


enum class CommandType {
    CALIBRATE,
    CONFIG,
    HAPTIC,
};

struct HapticData {
    bool press;
};

struct Command {
    CommandType command_type;
    union CommandData {
        uint8_t unused;
        PB_SmartKnobConfig config;
        HapticData haptic;
    };
    CommandData data;
};

class MotorTask : public Task<MotorTask> {
    friend class Task<MotorTask>; // Allow base Task to invoke protected run()

    public:
        MotorTask(const uint8_t task_core, Configuration& configuration);
        ~MotorTask();

        void setConfig(const PB_SmartKnobConfig& config);
        void playHaptic(bool press);

        // Rim "follow" mode: while active, the motor turns the knob so it tracks
        // the finger. delta_radians = finger travel since the slide started
        // (+ = direction that raises the value).
        void setFollow(bool active, float delta_radians);

        // Sleep: no holding force, the knob turns freely (its position is still tracked)
        void setRelaxed(bool relaxed);
        void runCalibration();

        void addListener(QueueHandle_t queue);
        void setLogger(Logger* logger);

    protected:
        void run();

    private:
        Configuration& configuration_;
        QueueHandle_t queue_;
        Logger* logger_;
        std::vector<QueueHandle_t> listeners_;
        char buf_[72];

        volatile bool follow_active_ = false;
        volatile bool relaxed_ = false;
        volatile float follow_delta_radians_ = 0;

        // BLDC motor & driver instance
        BLDCMotor motor = BLDCMotor(1);
#if SK_MOTOR_3PWM
        BLDCDriver3PWM driver = BLDCDriver3PWM(PIN_MOTOR_A, PIN_MOTOR_B, PIN_MOTOR_C);
#else
        BLDCDriver6PWM driver = BLDCDriver6PWM(PIN_UH, PIN_UL, PIN_VH, PIN_VL, PIN_WH, PIN_WL);
#endif

        void publish(const PB_SmartKnobState& state);
        void calibrate();
        void checkSensorError();
        void log(const char* msg);
};
