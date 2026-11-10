#pragma once

#include "esp_lvgl_port.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_WIDTH 320
#define ST7789_HEIGHT 240

/*
 * Sets up SPI bus, panel IO, ST7789 panel and backlight, then starts LVGL
 * (via esp_lvgl_port, which runs its own LVGL task) and registers the panel
 * as an LVGL display. The active screen starts out black.
 *
 * Safe to call more than once; later calls just return the same display.
 *
 * LVGL is not thread safe: any lv_* call made from outside LVGL callbacks
 * (lv_timer, event handlers) must be wrapped in
 * lvgl_port_lock(timeout_ms) / lvgl_port_unlock().
 */
lv_display_t *st7789_init(void);

#ifdef __cplusplus
}
#endif
