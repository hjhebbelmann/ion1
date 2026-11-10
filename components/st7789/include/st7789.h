#pragma once

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "soc/soc_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_WIDTH 320
#define ST7789_HEIGHT 240

/*
 * XPT2046 resistive touch of the ESP32-2432S028R ("CYD"), on its own SPI bus
 * (SPI3). Only available on chips with a third SPI controller (ESP32, not C3).
 * These pins can't be used for anything else while the display is enabled.
 */
#if SOC_SPI_PERIPH_NUM > 2
#define ST7789_HAS_TOUCH 1
#define ST7789_TOUCH_PIN_SCLK 25
#define ST7789_TOUCH_PIN_MOSI 32
#define ST7789_TOUCH_PIN_MISO 39
#define ST7789_TOUCH_PIN_CS 33
#else
#define ST7789_HAS_TOUCH 0
#endif

/*
 * Sets up SPI bus, panel IO, ST7789 panel and backlight, then starts LVGL
 * (via esp_lvgl_port, which runs its own LVGL task) and registers the panel
 * as an LVGL display, and the touch controller (if any) as an LVGL input
 * device. The active screen starts out black.
 *
 * Safe to call more than once; later calls just return the same display.
 *
 * LVGL is not thread safe: any lv_* call made from outside LVGL callbacks
 * (lv_timer, event handlers) must be wrapped in
 * lvgl_port_lock(timeout_ms) / lvgl_port_unlock().
 */
lv_display_t *st7789_init(void);

/* The touch input device, or NULL if there is none (or it failed to start). */
lv_indev_t *st7789_get_touch(void);

#ifdef __cplusplus
}
#endif
