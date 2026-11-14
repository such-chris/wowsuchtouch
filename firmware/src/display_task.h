#pragma once

#if SK_DISPLAY

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

#include "logger.h"
#include "proto_gen/smartknob.pb.h"
#include "task.h"
#include "ui_screen.h"

// Owns the screen. LVGL (the UI toolkit) is only ever touched from this task;
// other tasks send it knob state through a queue.
class DisplayTask : public Task<DisplayTask> {
    friend class Task<DisplayTask>; // Allow base Task to invoke protected run()

    public:
        DisplayTask(const uint8_t task_core);
        ~DisplayTask();

        QueueHandle_t getKnobStateQueue();

        void setBrightness(uint16_t brightness);

        // Switch what the screen shows (control / list / value). Safe to call from other tasks.
        void showScreen(const UiScreen& screen);
        void showStatus(const UiStatus& status);
        void showToast(const UiToast& toast);
        void setLogger(Logger* logger);

    protected:
        void run();

    private:
        TFT_eSPI tft_ = TFT_eSPI();

        QueueHandle_t knob_state_queue_;
        QueueHandle_t ui_screen_queue_;
        QueueHandle_t ui_status_queue_;
        QueueHandle_t ui_toast_queue_;

        SemaphoreHandle_t mutex_;
        uint16_t brightness_;
        Logger* logger_;
        void log(const char* msg);

        // Screen pieces (built once, then updated)
        lv_obj_t* track_arc_ = nullptr;   // dim arc showing the mode's range
        lv_obj_t* fill_arc_ = nullptr;    // bright arc from the start of the range to the value
        lv_obj_t* dot_ = nullptr;         // marker at the current position
        lv_obj_t* value_label_ = nullptr;  // big number (control and value layouts)
        lv_obj_t* text_label_ = nullptr;   // mode name (control layout)
        lv_obj_t* title_label_ = nullptr;  // small heading (list and value layouts)
        lv_obj_t* list_box_ = nullptr;     // list: column of items that slides with the knob
        lv_obj_t* list_items_[UI_MAX_ITEMS] = {};
        float shown_scroll_ = -1000;       // list position last drawn (in items)
        lv_obj_t* info_label_ = nullptr;   // value layout: two lines of info
        lv_obj_t* status_label_ = nullptr; // top: Bluetooth + battery
        lv_obj_t* toast_label_ = nullptr;  // pop-up message
        uint32_t toast_hide_ms_ = 0;       // when the current pop-up should fade (0 = none showing)
        void applyStatus(const UiStatus& status);
        void buildScreen();
        void applyLayout();
        void updateScreen(const PB_SmartKnobState& state);

        UiScreen ui_;                   // current layout and its text
        PB_SmartKnobState last_state_ = {};

        // Last values shown, so unchanged text isn't redrawn
        int32_t shown_value_ = INT32_MIN;
        char shown_text_[51] = {};
};

#else

class DisplayTask {};

#endif
