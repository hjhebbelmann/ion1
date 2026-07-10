#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_WIDTH 320
#define ST7789_HEIGHT 240

#define ST7789_COLOR_WHITE 0xFFFF
#define ST7789_COLOR_BLACK 0x0000

/*
 * Sets up SPI bus, panel IO, ST7789 panel and backlight, and blanks the
 * screen. No LVGL involved: text is drawn directly with esp_lcd_panel_draw_bitmap.
 */
void st7789_init(void);

/*
 * Renders `text` centered horizontally into the row [y, y+height) using the
 * built-in 5x7 font scaled up by `scale`, then pushes it straight to the
 * panel. `buf` must point to caller-owned storage of at least
 * ST7789_WIDTH * height uint16_t elements; each on-screen line should use
 * its own dedicated buffer (not one shared/reused between different lines)
 * since the SPI transfer this kicks off runs asynchronously.
 */
void st7789_draw_line(uint16_t *buf, int y, int height, int scale, uint16_t fg_color, uint16_t bg_color,
                       const char *text);

#ifdef __cplusplus
}
#endif
