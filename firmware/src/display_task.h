#pragma once

#if SK_DISPLAY

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

#include "logger.h"
#include "proto_gen/smartknob.pb.h"
#include "task.h"

// Owns the screen. LVGL (the UI toolkit) is only ever touched from this task;
// other tasks send it knob state through a queue.
class DisplayTask : public Task<DisplayTask> {
    friend class Task<DisplayTask>; // Allow base Task to invoke protected run()

    public:
        DisplayTask(const uint8_t task_core);
        ~DisplayTask();

        QueueHandle_t getKnobStateQueue();

        void setBrightness(uint16_t brightness);
        void setLogger(Logger* logger);

    protected:
        void run();

    private:
        TFT_eSPI tft_ = TFT_eSPI();

        QueueHandle_t knob_state_queue_;

        SemaphoreHandle_t mutex_;
        uint16_t brightness_;
        Logger* logger_;
        void log(const char* msg);

        // Screen pieces (built once, then updated)
        lv_obj_t* track_arc_ = nullptr;   // dim arc showing the mode's range
        lv_obj_t* fill_arc_ = nullptr;    // bright arc from the start of the range to the value
        lv_obj_t* dot_ = nullptr;         // marker at the current position
        lv_obj_t* value_label_ = nullptr;
        lv_obj_t* text_label_ = nullptr;
        void buildScreen();
        void updateScreen(const PB_SmartKnobState& state);

        // Last values shown, so unchanged text isn't redrawn
        int32_t shown_value_ = INT32_MIN;
        char shown_text_[51] = {};
};

#else

class DisplayTask {};

#endif
