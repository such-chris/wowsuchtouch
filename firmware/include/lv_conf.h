// LVGL settings for the MaTouch knob. Anything not set here uses LVGL's default
// (see lv_conf_template.h in the LVGL library for every option).
#ifndef LV_CONF_H
#define LV_CONF_H

// 16-bit color (RGB565), what the round GC9A01 screen uses
#define LV_COLOR_DEPTH 16

// Memory LVGL keeps for screens, labels, arcs, etc.
#define LV_MEM_SIZE (64 * 1024U)

// Redraw up to ~60 times per second
#define LV_DEF_REFR_PERIOD 16

// Fonts (Montserrat, built into LVGL) in the sizes the UI uses
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

#endif // LV_CONF_H
