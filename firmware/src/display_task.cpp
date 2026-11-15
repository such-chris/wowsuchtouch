#if SK_DISPLAY
#include "display_task.h"
#include "semaphore_guard.h"
#include "util.h"
#include "emoji.h"

static const lv_image_dsc_t* iconImage(UiIcon icon, bool small) {
    switch (icon) {
        case UiIcon::RED:     return &emoji_red;
        case UiIcon::YELLOW:  return &emoji_yellow;
        case UiIcon::GREEN:   return &emoji_green;
        case UiIcon::BLUE:    return &emoji_blue;
        case UiIcon::PURPLE:  return &emoji_purple;
        case UiIcon::STAR:    return small ? &emoji_star_small : &emoji_star;
        case UiIcon::PALETTE: return &emoji_palette_small;
        default:              return nullptr;
    }
}

static const uint8_t LEDC_CHANNEL_LCD_BACKLIGHT = 0;

// Screen geometry
static const int32_t CENTER = TFT_WIDTH / 2;
static const int32_t RING_RADIUS = CENTER - 12;   // where the position dot travels
static const int32_t DOT_SIZE = 14;
static const int32_t LIST_ITEM_WIDTH = 200;
// The list is drawn like a drum (a rolodex seen from the side): items sit on a
// wheel of this radius, one item every LIST_STEP_DEG degrees. Items further
// round the wheel bunch up, shrink and fade, and vanish past LIST_HIDE_DEG.
static const float LIST_RADIUS = 72;              // pixels
static const float LIST_STEP_DEG = 30;
static const float LIST_HIDE_DEG = 80;

// Colors
static const uint32_t COLOR_ACCENT = 0x5064C8;    // blue-violet, from Scott's dot
static const uint32_t COLOR_ACCENT_TEXT = 0x8A9BFF; // lighter accent, readable as text on black
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

    ui_value_queue_ = xQueueCreate(1, sizeof(UiValueText));
    assert(ui_value_queue_ != NULL);

    mutex_ = xSemaphoreCreateMutex();
    assert(mutex_ != NULL);
}

DisplayTask::~DisplayTask() {
    vQueueDelete(knob_state_queue_);
    vQueueDelete(ui_screen_queue_);
    vQueueDelete(ui_status_queue_);
    vQueueDelete(ui_toast_queue_);
    vQueueDelete(ui_value_queue_);
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
        UiValueText value_text;
        if (xQueueReceive(ui_value_queue_, &value_text, 0) == pdTRUE) {
            if (value_text.sub) {
                lv_label_set_text(sub_label_, value_text.text);
            } else {
                memcpy(value_text_, value_text.text, sizeof(value_text_));
                value_text_[sizeof(value_text_) - 1] = 0;
                shown_value_ = INT32_MIN;  // redraw
                updateScreen(last_state_);
            }
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
    // The list column is taller than the screen; keep the screen itself from scrolling (no scroll bar)
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
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

    sub_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sub_label_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sub_label_, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(sub_label_, LV_ALIGN_CENTER, 0, 72);
    lv_label_set_text(sub_label_, "");

    // Spring style: small marks at the two ends of the throw
    for (int i = 0; i < 2; i++) {
        lv_obj_t* mark = lv_obj_create(screen);
        lv_obj_remove_style_all(mark);
        lv_obj_set_size(mark, 8, 8);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(mark, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
        end_marks_[i] = mark;
    }

    // Swipe hints at the edges (left, right, up, down): a word, a symbol or an emoji
    for (int i = 0; i < 4; i++) {
        lv_obj_t* hint = lv_label_create(screen);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_TEXT_DIM), 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(hint, "");
        hint_labels_[i] = hint;
        hint_images_[i] = lv_image_create(screen);
        lv_obj_add_flag(hint_images_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Ring pickers: one emoji per choice around the top edge
    for (int i = 0; i < UI_MAX_ITEMS; i++) {
        picker_images_[i] = lv_image_create(screen);
        lv_obj_add_flag(picker_images_[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Buttons around the edge: words that follow the edge, with dividing lines between
    for (int i = 0; i < UI_MAX_ITEMS; i++) {
        lv_obj_t* button = lv_label_create(screen);
        lv_obj_set_style_text_font(button, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(button, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_transform_pivot_x(button, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(button, LV_PCT(50), 0);
        lv_label_set_text(button, "");
        lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
        button_labels_[i] = button;

        lv_obj_t* line = lv_line_create(screen);
        lv_obj_set_style_line_width(line, 1, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(COLOR_TRACK), 0);
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        button_lines_[i] = line;
    }
    ring_radius_ = RING_RADIUS;

    // Heading for lists and settings
    title_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(COLOR_TEXT_DIM), 0);
    lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(title_label_, "");

    // List: items on a drum that turns with the knob (positions set in the update)
    list_box_ = lv_obj_create(screen);
    lv_obj_remove_style_all(list_box_);
    lv_obj_set_size(list_box_, TFT_WIDTH, TFT_HEIGHT);
    lv_obj_remove_flag(list_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(list_box_, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < UI_MAX_ITEMS; i++) {
        lv_obj_t* item = lv_label_create(list_box_);
        lv_obj_set_style_text_font(item, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(item, lv_color_white(), 0);
        lv_obj_set_style_text_align(item, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(item, LIST_ITEM_WIDTH);
        lv_label_set_long_mode(item, LV_LABEL_LONG_DOT);
        lv_obj_set_x(item, (TFT_WIDTH - LIST_ITEM_WIDTH) / 2);
        list_items_[i] = item;
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

    char text[48] = {};
    const char* bluetooth = status.bluetooth ? LV_SYMBOL_BLUETOOTH "  " : "";
    const char* charging = status.usb ? LV_SYMBOL_CHARGE " " : "";
    if (status.battery_pct >= 0) {
        snprintf(text, sizeof(text), "%s%s%s %d%%", bluetooth, charging, battery, status.battery_pct);
    } else {
        snprintf(text, sizeof(text), "%s%s", bluetooth, charging);
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

    bool ring_pick = list && ui_.list_style != UiListStyle::DRUM;
    bool buttons = (control || ring_pick) && ui_.button_count > 0;

    setVisible(text_label_, control);
    setVisible(value_label_, (control && !ui_.hide_value) || (value && ui_.show_number));

    // Swipe hints: arrow + word at each edge
    const char* hints[4] = {ui_.hint_left, ui_.hint_right, ui_.hint_up, ui_.hint_down};
    for (int i = 0; i < 4; i++) {
        bool show = control && hints[i][0] != 0;
        // ":name:" at the start = a symbol or emoji; any words after it are shown too
        const char* symbol = nullptr;
        UiIcon icon = UiIcon::NONE;
        lv_color_t color = lv_color_hex(COLOR_TEXT_DIM);
        const char* word = hints[i];
        if (strncmp(word, ":x:", 3) == 0) { symbol = LV_SYMBOL_CLOSE; color = lv_color_hex(0x707078); word += 3; }
        else if (strncmp(word, ":check:", 7) == 0) { symbol = LV_SYMBOL_OK; color = lv_color_white(); word += 7; }
        else if (strncmp(word, ":palette:", 9) == 0) { icon = UiIcon::PALETTE; word += 9; }
        else if (strncmp(word, ":star:", 6) == 0) { icon = UiIcon::STAR; word += 6; }
        while (*word == ' ') word++;

        // Symbols are drawn as part of the label: "symbol\nword" at the sides, "symbol word" top/bottom
        bool sides = i < 2;
        if (symbol) {
            lv_label_set_text_fmt(hint_labels_[i], sides ? "%s\n%s" : "%s %s", symbol, word);
        } else {
            lv_label_set_text(hint_labels_[i], word);
        }
        lv_obj_set_style_text_font(hint_labels_[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(hint_labels_[i], color, 0);
        bool has_word = word[0] != 0 || symbol != nullptr;
        setVisible(hint_labels_[i], show && has_word);
        setVisible(hint_images_[i], show && icon != UiIcon::NONE);
        if (!show) continue;
        if (icon != UiIcon::NONE) {
            lv_image_set_src(hint_images_[i], iconImage(icon, true));
        }
        // Emoji at the very edge, its word just inside it
        switch (i) {
            case 0:
                lv_obj_align(hint_images_[i], LV_ALIGN_LEFT_MID, 6, 0);
                lv_obj_align(hint_labels_[i], LV_ALIGN_LEFT_MID, icon != UiIcon::NONE ? 30 : 8, 0);
                break;
            case 1:
                lv_obj_align(hint_images_[i], LV_ALIGN_RIGHT_MID, -6, 0);
                lv_obj_align(hint_labels_[i], LV_ALIGN_RIGHT_MID, icon != UiIcon::NONE ? -30 : -8, 0);
                break;
            case 2:
                lv_obj_align(hint_images_[i], LV_ALIGN_TOP_MID, 0, 6);
                lv_obj_align(hint_labels_[i], LV_ALIGN_TOP_MID, 0, icon != UiIcon::NONE ? 30 : 10);
                break;
            default:
                lv_obj_align(hint_images_[i], LV_ALIGN_BOTTOM_MID, 0, -6);
                lv_obj_align(hint_labels_[i], LV_ALIGN_BOTTOM_MID, 0, icon != UiIcon::NONE ? -30 : -10);
                break;
        }
    }

    // Ring picker: the choices' emoji spread across the top edge
    int icon_count = 0;
    for (int i = 0; i < ui_.item_count; i++) {
        if (ui_.item_icons[i] != UiIcon::NONE) icon_count++;
    }
    for (int i = 0, k = 0; i < UI_MAX_ITEMS; i++) {
        bool show = ring_pick && i < ui_.item_count && ui_.item_icons[i] != UiIcon::NONE;
        setVisible(picker_images_[i], show);
        if (!show) continue;
        lv_image_set_src(picker_images_[i], iconImage(ui_.item_icons[i], false));
        float r = uiPickerAngle(k++, icon_count) * PI / 180;
        int radius = buttons ? UI_PICKER_RADIUS_INNER : UI_PICKER_RADIUS;
        lv_obj_align(picker_images_[i], LV_ALIGN_CENTER,
            (int32_t)(radius * cosf(r)), (int32_t)(radius * sinf(r)));
    }

    // Buttons around the edge: each word turned to follow the edge (never upside
    // down), the chosen one in the accent color, a thin line between neighbours.
    // The value ring moves in to the middle so it doesn't cross the words.
    setRingRadius(RING_RADIUS);   // the ring runs round the outside of the buttons
    for (int i = 0; i < UI_MAX_ITEMS; i++) {
        bool show = buttons && i < ui_.button_count;
        setVisible(button_labels_[i], show);
        setVisible(button_lines_[i], show);
        if (!show) continue;
        lv_label_set_text(button_labels_[i], ui_.buttons[i]);
        bool chosen = i == ui_.button_selected;
        lv_obj_set_style_text_color(button_labels_[i],
            chosen ? lv_color_hex(COLOR_ACCENT_TEXT) : lv_color_hex(COLOR_TEXT_DIM), 0);
        float angle = wrapDegrees(uiButtonAngle(i, ui_.button_count));
        float r = angle * PI / 180;
        lv_obj_align(button_labels_[i], LV_ALIGN_CENTER,
            (int32_t)(UI_BUTTON_RADIUS * cosf(r)), (int32_t)(UI_BUTTON_RADIUS * sinf(r)));
        // Along the edge: +90 in the top half, -90 in the bottom half so it still reads left to right
        float text_angle = (angle > 0 && angle < 180) ? angle - 90 : angle + 90;
        lv_obj_set_style_transform_rotation(button_labels_[i], (int32_t)(text_angle * 10), 0);

        float gap = (uiButtonAngle(i, ui_.button_count) - 180.0f / ui_.button_count) * PI / 180;
        button_line_points_[i][0] = { CENTER + UI_BUTTON_LINE_FROM * cosf(gap), CENTER + UI_BUTTON_LINE_FROM * sinf(gap) };
        button_line_points_[i][1] = { CENTER + UI_BUTTON_LINE_TO * cosf(gap), CENTER + UI_BUTTON_LINE_TO * sinf(gap) };
        lv_line_set_points(button_lines_[i], button_line_points_[i], 2);
    }
    setVisible(list_box_, list && !ring_pick);
    setVisible(info_label_, (value && !ui_.show_number) || ring_pick);
    lv_obj_align(info_label_, LV_ALIGN_CENTER, 0, ring_pick && buttons ? 14 : 6);
    setVisible(title_label_, !ring_pick && !buttons);
    setVisible(status_label_, ui_.show_status);

    if (list) {
        for (int i = 0; i < UI_MAX_ITEMS; i++) {
            setVisible(list_items_[i], i < ui_.item_count);
            if (i < ui_.item_count) {
                lv_label_set_text(list_items_[i], ui_.items[i]);
            }
        }
        shown_scroll_ = -1000;
    }

    lv_label_set_text(title_label_, ui_.title);
    if (value && !ui_.show_number) {
        lv_label_set_text_fmt(info_label_, "%s\n%s", ui_.line1, ui_.line2);
    }
    lv_obj_align(value_label_, LV_ALIGN_CENTER, 0, control ? -18 : 6);
    // With no number, the page name sits in the middle
    lv_obj_align(text_label_, LV_ALIGN_CENTER, 0, ui_.hide_value ? 0 : 40);

    value_text_[0] = 0;  // a new screen starts without the Mac's value
    lv_label_set_text(sub_label_, "");
    setVisible(sub_label_, control);
    // The direction style fades the number; start every screen fully visible
    lv_anim_delete(value_label_, NULL);
    lv_obj_set_style_opa(value_label_, LV_OPA_COVER, 0);

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
    // Bounded modes spread their range evenly either side of the top (or the
    // bottom). Either way the first position is on the left.
    float range_degrees = bounded ? fminf((max_position - min_position) * width_degrees, 340) : 0;
    // Squeezed ranges (more positions than fit round the ring) squeeze the dot's travel too
    if (bounded && max_position > min_position) {
        width_degrees = range_degrees / (max_position - min_position);
    }
    float middle_angle = ui_.ring_at_bottom ? 90 : 270;
    float direction = ui_.ring_at_bottom ? -1 : 1;  // along the bottom, left-to-right is anticlockwise
    float start_angle = middle_angle - direction * range_degrees / 2;
    float end_angle = middle_angle + direction * range_degrees / 2;
    float dot_angle = start_angle + direction * (position - min_position + state.sub_position_unit) * width_degrees;
    float low_angle = fminf(start_angle, end_angle);
    float high_angle = fmaxf(start_angle, end_angle);
    if (bounded && num_positions > 1) {
        dot_angle = CLAMP(dot_angle, low_angle - 8, high_angle + 8);  // a little rubber-band past the ends
    }
    // (Single-position "spring" modes let the dot follow the full deflection)

    // Range arcs (hidden for unbounded modes)
    if (bounded && range_degrees > 0) {
        lv_obj_remove_flag(track_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(fill_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_bg_angles(track_arc_, wrapDegrees(low_angle), wrapDegrees(high_angle));
        // Arcs are drawn clockwise, so the fill goes from whichever end comes first
        float fill_to = CLAMP(dot_angle, low_angle, high_angle);
        if (direction > 0) {
            lv_arc_set_angles(fill_arc_, wrapDegrees(start_angle), wrapDegrees(fill_to));
        } else {
            lv_arc_set_angles(fill_arc_, wrapDegrees(fill_to), wrapDegrees(start_angle));
        }
    } else {
        lv_obj_add_flag(track_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fill_arc_, LV_OBJ_FLAG_HIDDEN);
    }
    // Spring style: the track is one full throw either side of the middle, and the
    // fill grows from the middle the way you push (a speed meter)
    bool spring = ui_.layout == UiLayout::CONTROL && ui_.control_style == UiControlStyle::SPRING;
    if (spring) {
        float low = middle_angle - width_degrees;
        float high = middle_angle + width_degrees;
        lv_obj_remove_flag(track_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(fill_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_bg_angles(track_arc_, wrapDegrees(low), wrapDegrees(high));
        float to = CLAMP(dot_angle, low, high);
        if (to >= middle_angle) {
            lv_arc_set_angles(fill_arc_, wrapDegrees(middle_angle), wrapDegrees(to));
        } else {
            lv_arc_set_angles(fill_arc_, wrapDegrees(to), wrapDegrees(middle_angle));
        }
        for (int i = 0; i < 2; i++) {
            float r = (i == 0 ? low : high) * PI / 180;
            lv_obj_set_pos(end_marks_[i], CENTER + (int32_t)(RING_RADIUS * cosf(r)) - 4,
                                          CENTER + (int32_t)(RING_RADIUS * sinf(r)) - 4);
        }
    }
    for (int i = 0; i < 2; i++) {
        setVisible(end_marks_[i], spring);
    }

    if (ui_.hide_ring) {
        lv_obj_add_flag(track_arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(fill_arc_, LV_OBJ_FLAG_HIDDEN);
    }
    setVisible(dot_, !ui_.hide_ring);

    // Dot on the ring
    float radians = dot_angle * PI / 180;
    int32_t x = CENTER + (int32_t)(ring_radius_ * cosf(radians)) - DOT_SIZE / 2;
    int32_t y = CENTER + (int32_t)(ring_radius_ * sinf(radians)) - DOT_SIZE / 2;
    lv_obj_set_pos(dot_, x, y);

    if (ui_.layout == UiLayout::LIST && ui_.list_style != UiListStyle::DRUM) {
        // Ring picker: brighten the chosen emoji (or all up to it, for stars) and
        // name it in the middle
        int32_t count = ui_.item_count;
        if (count == 0) {
            return;
        }
        int32_t chosen = CLAMP((int32_t)lroundf(position + state.sub_position_unit), (int32_t)0, count - 1);
        if (chosen == (int32_t)shown_scroll_) {
            return;
        }
        shown_scroll_ = chosen;
        bool cumulative = ui_.list_style == UiListStyle::RING_COUNT;
        for (int32_t i = 0; i < count; i++) {
            bool lit = cumulative ? (i <= chosen) : (i == chosen);
            lv_obj_set_style_image_opa(picker_images_[i], lit ? LV_OPA_COVER : (cumulative ? 55 : 90), 0);
        }
        lv_label_set_text(info_label_, ui_.items[chosen]);
        return;
    }

    if (ui_.layout == UiLayout::LIST) {
        // Turn the drum so the knob's position (including the part-way turn
        // between clicks) faces you; items further round shrink and fade
        int32_t count = ui_.item_count;
        if (count == 0) {
            return;
        }
        float scroll = CLAMP(position + state.sub_position_unit, -0.4f, count - 1 + 0.4f);
        if (fabsf(scroll - shown_scroll_) < 0.01f) {
            return;
        }
        shown_scroll_ = scroll;
        for (int32_t i = 0; i < count; i++) {
            float degrees = (i - scroll) * LIST_STEP_DEG;
            lv_obj_t* item = list_items_[i];
            if (fabsf(degrees) > LIST_HIDE_DEG) {
                lv_obj_set_style_text_opa(item, LV_OPA_TRANSP, 0);
                continue;
            }
            float radians = degrees * PI / 180;
            float facing = cosf(radians);  // 1 = facing you, 0 = edge-on

            // Smaller further round (fonts come in fixed sizes, so it steps)
            const lv_font_t* font = facing > 0.93f ? &lv_font_montserrat_24
                                  : facing > 0.6f  ? &lv_font_montserrat_20
                                                   : &lv_font_montserrat_14;
            if (lv_obj_get_style_text_font(item, 0) != font) {
                lv_obj_set_style_text_font(item, font, 0);
            }
            int32_t y = CENTER + (int32_t)(LIST_RADIUS * sinf(radians)) - lv_font_get_line_height(font) / 2;
            lv_obj_set_y(item, y);
            lv_obj_set_style_text_opa(item, (lv_opa_t)(255 * facing * facing), 0);
            // The item that a tap would choose is in the accent color
            bool selected = i == (int32_t)lroundf(scroll);
            lv_obj_set_style_text_color(item, selected ? lv_color_hex(COLOR_ACCENT_TEXT) : lv_color_white(), 0);
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

    // Control layout: value (only redraw when it changes). Text from the Mac
    // (e.g. Lightroom's real slider value) replaces the knob's own count.
    if (ui_.control_style == UiControlStyle::SPRING) {
        // Arrow the way it scrolls, brighter the faster (clockwise scrolls down)
        float deflection = (state.current_position - config.min_position) + state.sub_position_unit;
        const char* arrow = deflection > 0.08f ? LV_SYMBOL_DOWN : (deflection < -0.08f ? LV_SYMBOL_UP : "");
        int32_t shown = deflection > 0.08f ? 1 : (deflection < -0.08f ? -1 : 0);
        if (shown != shown_value_) {
            shown_value_ = shown;
            lv_label_set_text(value_label_, arrow);
        }
        float strength = CLAMP(fabsf(deflection), 0.0f, 1.0f);
        lv_obj_set_style_opa(value_label_, (lv_opa_t)(70 + 185 * strength), 0);
    } else if (ui_.control_style == UiControlStyle::DIRECTION) {
        // Each click flashes an arrow the way you turned, then it fades
        if (state.current_position != shown_value_) {
            if (shown_value_ != INT32_MIN) {
                lv_label_set_text(value_label_, state.current_position > shown_value_ ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
                lv_anim_delete(value_label_, NULL);
                lv_obj_set_style_opa(value_label_, LV_OPA_COVER, 0);
                lv_obj_fade_out(value_label_, 400, 80);
            } else {
                lv_label_set_text(value_label_, "");
            }
            shown_value_ = state.current_position;
        }
    } else if (value_text_[0] != 0) {
        if (shown_value_ != INT32_MIN + 1) {
            shown_value_ = INT32_MIN + 1;
            lv_label_set_text(value_label_, value_text_);
        }
    } else if (state.current_position != shown_value_) {
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

void DisplayTask::setRingRadius(int32_t radius) {
    if (radius == ring_radius_) {
        return;
    }
    ring_radius_ = radius;
    lv_obj_set_size(track_arc_, radius * 2, radius * 2);
    lv_obj_center(track_arc_);
    lv_obj_set_size(fill_arc_, radius * 2, radius * 2);
    lv_obj_center(fill_arc_);
}

void DisplayTask::showScreen(const UiScreen& screen) {
    xQueueOverwrite(ui_screen_queue_, &screen);
}

void DisplayTask::showStatus(const UiStatus& status) {
    xQueueOverwrite(ui_status_queue_, &status);
}

void DisplayTask::showValueText(const char* text, bool sub) {
    UiValueText value;
    value.sub = sub;
    strncpy(value.text, text, sizeof(value.text) - 1);
    xQueueOverwrite(ui_value_queue_, &value);
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
