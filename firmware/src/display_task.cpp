#if SK_DISPLAY
#include "display_task.h"
#include "semaphore_guard.h"
#include "util.h"

static const uint8_t LEDC_CHANNEL_LCD_BACKLIGHT = 0;

// Screen geometry
static const int32_t CENTER = TFT_WIDTH / 2;
static const int32_t RING_RADIUS = CENTER - 12;   // where the position dot travels
static const int32_t DOT_SIZE = 14;

// Colors
static const uint32_t COLOR_ACCENT = 0x5064C8;    // blue-violet, from Scott's dot
static const uint32_t COLOR_TRACK = 0x2A2A33;
static const uint32_t COLOR_TEXT_DIM = 0x9A9AA8;
static const uint32_t COLOR_TEXT_FAINT = 0x4A4A55;

// LVGL draws into this buffer a strip at a time; each strip is then sent to the screen.
// 24 rows: 240 x 24 pixels x 2 bytes = 11.5 KB
static const uint16_t DRAW_BUFFER_ROWS = 24;
static uint16_t draw_buffer[TFT_WIDTH * DRAW_BUFFER_ROWS];

static TFT_eSPI* flush_tft = nullptr;

// Called by LVGL with a finished strip of pixels
static void flushToScreen(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    uint32_t width = area->x2 - area->x1 + 1;
    uint32_t height = area->y2 - area->y1 + 1;
    flush_tft->startWrite();
    flush_tft->setAddrWindow(area->x1, area->y1, width, height);
    flush_tft->pushColors((uint16_t*)pixels, width * height, true);  // true = swap byte order
    flush_tft->endWrite();
    lv_display_flush_ready(display);
}

// Keep an angle within 0-360 (LVGL arcs expect that; they still draw across the top)
static int32_t wrapDegrees(float degrees) {
    int32_t wrapped = ((int32_t)lroundf(degrees)) % 360;
    return wrapped < 0 ? wrapped + 360 : wrapped;
}

static uint32_t lvglTick() {
    return millis();
}

DisplayTask::DisplayTask(const uint8_t task_core) : Task{"Display", 8192, 1, task_core} {
    knob_state_queue_ = xQueueCreate(1, sizeof(PB_SmartKnobState));
    assert(knob_state_queue_ != NULL);

    ui_screen_queue_ = xQueueCreate(1, sizeof(UiScreen));
    assert(ui_screen_queue_ != NULL);

    ui_status_queue_ = xQueueCreate(1, sizeof(UiStatus));
    assert(ui_status_queue_ != NULL);

    ui_toast_queue_ = xQueueCreate(1, sizeof(UiToast));
    assert(ui_toast_queue_ != NULL);

    mutex_ = xSemaphoreCreateMutex();
    assert(mutex_ != NULL);
}

DisplayTask::~DisplayTask() {
    vQueueDelete(knob_state_queue_);
    vQueueDelete(ui_screen_queue_);
    vQueueDelete(ui_status_queue_);
    vQueueDelete(ui_toast_queue_);
    vSemaphoreDelete(mutex_);
}

void DisplayTask::run() {
    tft_.begin();
    tft_.invertDisplay(1);
    tft_.setRotation(SK_DISPLAY_ROTATION);
    tft_.fillScreen(TFT_BLACK);

    ledcSetup(LEDC_CHANNEL_LCD_BACKLIGHT, 5000, SK_BACKLIGHT_BIT_DEPTH);
    ledcAttachPin(PIN_LCD_BACKLIGHT, LEDC_CHANNEL_LCD_BACKLIGHT);
    ledcWrite(LEDC_CHANNEL_LCD_BACKLIGHT, (1 << SK_BACKLIGHT_BIT_DEPTH) - 1);

    lv_init();
    lv_tick_set_cb(lvglTick);
    flush_tft = &tft_;
    lv_display_t* display = lv_display_create(TFT_WIDTH, TFT_HEIGHT);
    lv_display_set_flush_cb(display, flushToScreen);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    buildScreen();
    applyLayout();
    log("Display: LVGL running");

    while (1) {
        if (xQueueReceive(ui_screen_queue_, &ui_, 0) == pdTRUE) {
            applyLayout();
            updateScreen(last_state_);
        }
        if (xQueueReceive(knob_state_queue_, &last_state_, 0) == pdTRUE) {
            updateScreen(last_state_);
        }

        UiStatus status;
        if (xQueueReceive(ui_status_queue_, &status, 0) == pdTRUE) {
            applyStatus(status);
        }

        // Pop-up: show it, then fade it out when its time is up
        UiToast toast;
        if (xQueueReceive(ui_toast_queue_, &toast, 0) == pdTRUE) {
            lv_anim_delete(toast_label_, NULL);  // stop any fade already running
            lv_label_set_text(toast_label_, toast.text);
            lv_obj_set_style_opa(toast_label_, LV_OPA_COVER, 0);
            lv_obj_remove_flag(toast_label_, LV_OBJ_FLAG_HIDDEN);
            toast_hide_ms_ = millis() + toast.duration_ms;
        }
        if (toast_hide_ms_ != 0 && (int32_t)(millis() - toast_hide_ms_) >= 0) {
            toast_hide_ms_ = 0;
            lv_obj_fade_out(toast_label_, 300, 0);
        }

        lv_timer_handler();  // LVGL redraws whatever changed

        {
            SemaphoreGuard lock(mutex_);
            ledcWrite(LEDC_CHANNEL_LCD_BACKLIGHT, brightness_);
        }
        delay(5);
    }
}

void DisplayTask::buildScreen() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Range arcs: a dim track and a bright fill, drawn just inside the edge
    track_arc_ = lv_arc_create(screen);
    lv_obj_set_size(track_arc_, RING_RADIUS * 2, RING_RADIUS * 2);
    lv_obj_center(track_arc_);
    lv_obj_remove_flag(track_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(track_arc_, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(track_arc_, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(track_arc_, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(track_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(track_arc_, 0, LV_PART_KNOB);

    fill_arc_ = lv_arc_create(screen);
    lv_obj_set_size(fill_arc_, RING_RADIUS * 2, RING_RADIUS * 2);
    lv_obj_center(fill_arc_);
    lv_obj_remove_flag(fill_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(fill_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(fill_arc_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(fill_arc_, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(fill_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(fill_arc_, 0, LV_PART_KNOB);

    // Position dot
    dot_ = lv_obj_create(screen);
    lv_obj_remove_style_all(dot_);
    lv_obj_set_size(dot_, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot_, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, 0);

    // Big value in the middle, mode name underneath
    value_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(value_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(value_label_, lv_color_white(), 0);
    lv_obj_align(value_label_, LV_ALIGN_CENTER, 0, -18);
    lv_label_set_text(value_label_, "");

    text_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(text_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(text_label_, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_align(text_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(text_label_, LV_ALIGN_CENTER, 0, 40);
    lv_label_set_text(text_label_, "");

    // Heading for lists and settings
    title_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(title_label_, "");

    // List: selected item big in the middle, neighbours dimmed above and below
    list_prev_ = lv_label_create(screen);
    lv_obj_set_style_text_font(list_prev_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(list_prev_, lv_color_hex(COLOR_TEXT_FAINT), 0);
    lv_obj_align(list_prev_, LV_ALIGN_CENTER, 0, -46);

    list_sel_ = lv_label_create(screen);
    lv_obj_set_style_text_font(list_sel_, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(list_sel_, lv_color_white(), 0);
    lv_obj_align(list_sel_, LV_ALIGN_CENTER, 0, 0);

    list_next_ = lv_label_create(screen);
    lv_obj_set_style_text_font(list_next_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(list_next_, lv_color_hex(COLOR_TEXT_FAINT), 0);
    lv_obj_align(list_next_, LV_ALIGN_CENTER, 0, 46);

    // List items longer than the screen scroll sideways instead of being cut off
    lv_obj_t* list_labels[] = {list_prev_, list_sel_, list_next_};
    for (lv_obj_t* label : list_labels) {
        lv_obj_set_width(label, 190);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    // Value layout: two lines of info under the heading
    info_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(info_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(info_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(info_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info_label_, LV_ALIGN_CENTER, 0, 6);

    // Status line at the very top: Bluetooth and battery
    status_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 12);
    lv_label_set_text(status_label_, "");

    // Pop-up message: a rounded pill near the bottom, on top of everything
    toast_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(toast_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(toast_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(toast_label_, lv_color_hex(0x26263A), 0);
    lv_obj_set_style_bg_opa(toast_label_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(toast_label_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(toast_label_, 14, 0);
    lv_obj_set_style_pad_ver(toast_label_, 6, 0);
    lv_obj_align(toast_label_, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_flag(toast_label_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayTask::applyStatus(const UiStatus& status) {
    // Built-in LVGL symbols: Bluetooth, and a battery that empties in 4 steps
    const char* battery = LV_SYMBOL_BATTERY_EMPTY;
    if (status.battery_pct >= 80) {
        battery = LV_SYMBOL_BATTERY_FULL;
    } else if (status.battery_pct >= 55) {
        battery = LV_SYMBOL_BATTERY_3;
    } else if (status.battery_pct >= 30) {
        battery = LV_SYMBOL_BATTERY_2;
    } else if (status.battery_pct >= 10) {
        battery = LV_SYMBOL_BATTERY_1;
    }

    char text[40] = {};
    if (status.battery_pct >= 0) {
        snprintf(text, sizeof(text), "%s  %s %d%%",
            status.bluetooth ? LV_SYMBOL_BLUETOOTH : "", battery, status.battery_pct);
    } else {
        snprintf(text, sizeof(text), "%s", status.bluetooth ? LV_SYMBOL_BLUETOOTH : "");
    }
    lv_label_set_text(status_label_, text);
}

static void setVisible(lv_obj_t* obj, bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// Show the pieces the current layout uses and hide the rest
void DisplayTask::applyLayout() {
    bool control = ui_.layout == UiLayout::CONTROL;
    bool list = ui_.layout == UiLayout::LIST;
    bool value = ui_.layout == UiLayout::VALUE;

    setVisible(text_label_, control);
    setVisible(value_label_, control || (value && ui_.show_number));
    setVisible(title_label_, true);  // app name on control screens, heading elsewhere
    setVisible(list_prev_, list);
    setVisible(list_sel_, list);
    setVisible(list_next_, list);
    setVisible(info_label_, value && !ui_.show_number);

    lv_label_set_text(title_label_, ui_.title);
    if (value && !ui_.show_number) {
        lv_label_set_text_fmt(info_label_, "%s\n%s", ui_.line1, ui_.line2);
    }
    lv_obj_align(value_label_, LV_ALIGN_CENTER, 0, control ? -18 : 6);

    // Force text to be redrawn for the new layout
    shown_value_ = INT32_MIN;
    shown_text_[0] = 0;
}

void DisplayTask::updateScreen(const PB_SmartKnobState& state) {
    const PB_SmartKnobConfig& config = state.config;
    float width_degrees = config.position_width_radians * 180 / PI;

    // Position and range the ring shows. A looping list turns forever, so show
    // where you are within the list instead (first item at the left end).
    int32_t position = state.current_position;
    int32_t min_position = config.min_position;
    int32_t max_position = config.max_position;
    bool looping_list = ui_.layout == UiLayout::LIST && ui_.wrap && ui_.item_count > 0;
    if (looping_list) {
        int32_t count = ui_.item_count;
        position = ((position % count) + count) % count;
        min_position = 0;
        max_position = count - 1;
    }
    int32_t num_positions = max_position - min_position + 1;
    bool bounded = num_positions > 0;

    // Angles in LVGL degrees: 0 = right, 90 = bottom, 270 = top, clockwise.
    // Bounded modes spread their range evenly either side of the top.
    float range_degrees = bounded ? fminf((max_position - min_position) * width_degrees, 340) : 0;
    float start_angle = 270 - range_degrees / 2;
    float dot_angle = start_angle + (position - min_position + state.sub_position_unit) * width_degrees;
    if (bounded && num_positions > 1) {
        dot_angle = CLAMP(dot_angle, start_angle - 8, start_angle + range_degrees + 8);  // a little rubber-band past the ends
    }
    // (Single-position "spring" modes let the dot follow the full deflection)

    // Range arcs (hidden for unbounded modes)
    if (bounded && range_degrees > 0) {
        lv_obj_remove_flag(track_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(fill_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_bg_angles(track_arc_, wrapDegrees(start_angle), wrapDegrees(start_angle + range_degrees));
        float fill_end = CLAMP(dot_angle, start_angle, start_angle + range_degrees);
        lv_arc_set_angles(fill_arc_, wrapDegrees(start_angle), wrapDegrees(fill_end));
    } else {
        lv_obj_add_flag(track_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fill_arc_, LV_OBJ_FLAG_HIDDEN);
    }

    // Dot on the ring
    float radians = dot_angle * PI / 180;
    int32_t x = CENTER + (int32_t)(RING_RADIUS * cosf(radians)) - DOT_SIZE / 2;
    int32_t y = CENTER + (int32_t)(RING_RADIUS * sinf(radians)) - DOT_SIZE / 2;
    lv_obj_set_pos(dot_, x, y);

    if (ui_.layout == UiLayout::LIST) {
        // Selected item and its neighbours (only redraw when the selection moves)
        int32_t count = ui_.item_count;
        int32_t sel = CLAMP(position, (int32_t)0, count - 1);
        if (count > 0 && sel != shown_value_) {
            shown_value_ = sel;
            // Looping lists show the neighbours around the ends too
            int32_t prev = sel > 0 ? sel - 1 : (ui_.wrap && count > 2 ? count - 1 : -1);
            int32_t next = sel < count - 1 ? sel + 1 : (ui_.wrap && count > 2 ? 0 : -1);
            lv_label_set_text(list_prev_, prev >= 0 ? ui_.items[prev] : "");
            lv_label_set_text(list_sel_, ui_.items[sel]);
            lv_label_set_text(list_next_, next >= 0 ? ui_.items[next] : "");
        }
        return;
    }

    if (ui_.layout == UiLayout::VALUE) {
        if (ui_.show_number && state.current_position != shown_value_) {
            shown_value_ = state.current_position;
            lv_label_set_text_fmt(value_label_, "%d%s", (int)(state.current_position * ui_.number_multiplier), ui_.suffix);
        }
        return;
    }

    // Control layout: value (only redraw when it changes)
    if (state.current_position != shown_value_) {
        shown_value_ = state.current_position;
        lv_label_set_text_fmt(value_label_, "%d", (int)state.current_position);
    }

    // Mode name; Scott's demo modes start with "SKDEMO_", which we hide
    const char* text = config.text;
    if (strncmp(text, "SKDEMO_", 7) == 0) {
        text += 7;
    }
    if (strcmp(text, shown_text_) != 0) {
        strncpy(shown_text_, text, sizeof(shown_text_) - 1);
        lv_label_set_text(text_label_, text);
    }
}

void DisplayTask::showScreen(const UiScreen& screen) {
    xQueueOverwrite(ui_screen_queue_, &screen);
}

void DisplayTask::showStatus(const UiStatus& status) {
    xQueueOverwrite(ui_status_queue_, &status);
}

void DisplayTask::showToast(const UiToast& toast) {
    xQueueOverwrite(ui_toast_queue_, &toast);
}

QueueHandle_t DisplayTask::getKnobStateQueue() {
    return knob_state_queue_;
}

void DisplayTask::setBrightness(uint16_t brightness) {
    SemaphoreGuard lock(mutex_);
    brightness_ = brightness >> (16 - SK_BACKLIGHT_BIT_DEPTH);
}

void DisplayTask::setLogger(Logger* logger) {
    logger_ = logger;
}

void DisplayTask::log(const char* msg) {
    if (logger_ != nullptr) {
        logger_->log(msg);
    }
}

#endif
