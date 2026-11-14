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

    mutex_ = xSemaphoreCreateMutex();
    assert(mutex_ != NULL);
}

DisplayTask::~DisplayTask() {
    vQueueDelete(knob_state_queue_);
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
    log("Display: LVGL running");

    PB_SmartKnobState state;
    while (1) {
        if (xQueueReceive(knob_state_queue_, &state, 0) == pdTRUE) {
            updateScreen(state);
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
}

void DisplayTask::updateScreen(const PB_SmartKnobState& state) {
    const PB_SmartKnobConfig& config = state.config;
    float width_degrees = config.position_width_radians * 180 / PI;
    int32_t num_positions = config.max_position - config.min_position + 1;
    bool bounded = num_positions > 0;

    // Angles in LVGL degrees: 0 = right, 90 = bottom, 270 = top, clockwise.
    // Bounded modes spread their range evenly either side of the top.
    float range_degrees = bounded ? fminf((config.max_position - config.min_position) * width_degrees, 340) : 0;
    float start_angle = 270 - range_degrees / 2;
    float dot_angle = start_angle + (state.current_position - config.min_position + state.sub_position_unit) * width_degrees;
    if (bounded) {
        dot_angle = CLAMP(dot_angle, start_angle - 8, start_angle + range_degrees + 8);  // a little rubber-band past the ends
    }

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

    // Value (only redraw when it changes)
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
