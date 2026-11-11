/**
 * Project-local LVGL 9.1 configuration for seed146.
 *
 * This header intentionally contains only preprocessor definitions. LVGL also
 * includes its configuration while compiling optional assembler files; a C
 * header such as <stdint.h> would be invalid in that assembler context.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* The DHR55146D and LovyanGFX exchange pixels in 16-bit RGB565 format. */
#define LV_COLOR_DEPTH 16

/* seed146 currently runs LVGL cooperatively from Arduino's loop(). */
#define LV_USE_OS LV_OS_NONE

/* Keep only the widgets needed by this validation screen and future baseline. */
#define LV_USE_ARC 1
#define LV_USE_BUTTON 1
#define LV_USE_IMAGE 1
#define LV_USE_IMAGEBUTTON 1
#define LV_USE_LABEL 1

/* Fonts enabled for the current prototype screens. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Layout and theme support retained for later seed146 screens. */
#define LV_USE_THEME_DEFAULT 1
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* Compatibility alias used by the planned SquareLine-generated UI. */
#define LV_SCREEN_LOAD_ANIM_FADE_ON LV_SCR_LOAD_ANIM_FADE_ON

/* Exclude all bundled LVGL demonstrations from the firmware. */
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_RENDER 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

/* Retain inexpensive allocation and null-pointer assertions while prototyping. */
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#endif /* LV_CONF_H */
