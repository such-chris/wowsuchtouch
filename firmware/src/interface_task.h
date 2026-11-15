#pragma once

#ifndef SK_BLE
    #define SK_BLE 0
#endif

#include <AceButton.h>
#include <Arduino.h>

#include "configuration.h"
#include "display_task.h"
#include "logger.h"
#include "motor_task.h"
#include "serial/serial_protocol_plaintext.h"
#include "serial/serial_protocol_protobuf.h"
#include "serial/uart_stream.h"
#include "task.h"
#include "settings_store.h"
#include "ui_screen.h"
#include "usb_power.h"

#if SK_TOUCH
#include "cst816s_touch.h"
#include "touch_gestures.h"
#include "touch_ring.h"
#include "ring_stepper.h"
#endif

#if SK_BLE
#include "ble_output.h"
#endif

#ifdef PIN_POWER_BUTTON
#include "power_button.h"
#endif

#ifdef PIN_BATTERY_ADC
#include "battery.h"
#endif

#ifndef SK_FORCE_UART_STREAM
    #define SK_FORCE_UART_STREAM 0
#endif

class InterfaceTask : public Task<InterfaceTask>, public Logger {
    friend class Task<InterfaceTask>; // Allow base Task to invoke protected run()

    public:
        InterfaceTask(const uint8_t task_core, MotorTask& motor_task, DisplayTask* display_task);
        virtual ~InterfaceTask();

        void log(const char* msg) override;
        void setConfiguration(Configuration* configuration);

        // Started by a charger being plugged in: show the charging screen only
        void setChargingMode(bool charging) { charging_mode_ = charging; }

    protected:
        void run();

    private:
    #if defined(CONFIG_IDF_TARGET_ESP32S3) && !SK_FORCE_UART_STREAM
        HWCDC stream_;
    #else
        UartStream stream_;
    #endif
        MotorTask& motor_task_;
        DisplayTask* display_task_;
        char buf_[128];

        SemaphoreHandle_t mutex_;
        Configuration* configuration_ = nullptr; // protected by mutex_

        PB_PersistentConfiguration configuration_value_;
        bool configuration_loaded_ = false;

        uint8_t strain_calibration_step_ = 0;
        int32_t strain_reading_ = 0;

        SerialProtocol* current_protocol_ = nullptr;
        bool remote_controlled_ = false;
        int current_config_ = 0;
        uint8_t press_count_ = 1;

        #if SK_TOUCH
        CST816STouch touch_;
        TouchGestures touch_gestures_;
        TouchRing touch_ring_;
        RingStepper ring_stepper_;

        // How sliding around the rim behaves (Settings > Touch)
        enum class RingMode { TRACK, FOLLOW, OFF };
        RingMode ring_mode_ = RingMode::TRACK;
        bool ring_was_active_ = false;
        bool ring_is_slide_ = false;  // moved far enough to count as a slide, not a tap
        void updateTouch();
        #endif

        #if SK_BLE
        BleOutput ble_;
        int ble_last_config_ = -1;          // mode the last sent position belongs to
        int32_t ble_last_position_ = 0;     // value when keys were last sent
        bool ble_was_connected_ = false;
        int32_t ble_volume_level_ = 32;     // last volume shown, restored when returning to Volume
        bool ble_volume_resynced_ = false;  // already re-synced for this push into an end stop
        float ble_scroll_accum_ = 0;        // fractional wheel ticks not yet sent
        uint32_t ble_scroll_last_ms_ = 0;
        float ring_scroll_accum_degrees_ = 0;
        bool inBleMode(int output);
        void updateBleOutput();
        #endif

        #ifdef PIN_POWER_BUTTON
        PowerButton power_button_;
        void updatePowerButton();
        void powerOff();
        #endif

        #ifdef PIN_BATTERY_ADC
        uint32_t battery_mv_ = 0;      // smoothed battery voltage in millivolts (0 = none yet)
        int8_t battery_pct_shown_ = -1;  // level shown on screen (-1 = none yet)
        void updateBattery();
        #endif

        PB_SmartKnobState latest_state_ = {};
        PB_SmartKnobConfig latest_config_ = {};

        QueueHandle_t log_queue_;
        QueueHandle_t knob_state_queue_;
        SerialProtocolPlaintext plaintext_protocol_;
        SerialProtocolProtobuf proto_protocol_;

        void changeConfig(bool next);

        // ---- Navigation: apps, the apps list and settings ----
        enum class NavScreen { APP, APPS_LIST, SETTINGS_LIST, SETTING };
        NavScreen nav_screen_ = NavScreen::APP;
        int current_app_ = 0;      // index into apps[]
        int open_setting_ = 0;     // which setting is open (SETTING screen)
        uint8_t nav_nonce_ = 0;    // makes the motor apply each new menu position
        int nav_list_count_ = 0;   // items in the open looping list (0 = not a looping list)
        bool battery_low_warned_ = false;
        UsbPower usb_power_;
        void updateUsbPower();

        // Sleep: 0 = awake, 1 = dimmed, 2 = screen off + motor relaxed
        uint8_t sleep_stage_ = 0;
        uint32_t last_activity_ms_ = 0;
        bool auto_off_tried_ = false;
        bool swallow_touch_ = false;          // the touch that woke the screen does nothing else
        int32_t activity_position_ = 0;
        float activity_sub_position_ = 0;
        void noteActivity();
        void checkKnobActivity();
        void updateSleep();

        bool charging_mode_ = false;
        void showChargingScreen();
        void leaveChargingMode();
        KnobSettings settings_;
        SettingsStore settings_store_;

        void setNavScreen(NavScreen screen);
        void showUi(const UiScreen& screen);
        void sendStatus();
        void toast(const char* text);
        void goHome();
        void openApp(int app);
        void applyAppPage();
        void openAppsList(int selected);
        void openSettingsList(int selected);
        void openSetting(int item);
        void navSelect();   // tap in a menu
        void navBack();     // double tap in a menu
        void updateLiveSetting();  // sliders that apply while you turn
        int32_t selectedIndex();
        void applySettings();
        void saveSettings();
        void fillBluetoothList(UiScreen& ui);
        void refreshBluetoothScreen();
        void updateHardware();
        void publishState();
        void applyConfig(PB_SmartKnobConfig& config, bool from_remote);
};
