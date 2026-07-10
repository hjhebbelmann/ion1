#include "st7789.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"

#include "font5x7.h"

/* ESP32-2432S028R ("CYD") pinout */
#define LCD_HOST SPI2_HOST
#define PIN_LCD_SCLK 14
#define PIN_LCD_MOSI 13
#define PIN_LCD_MISO 12
#define PIN_LCD_CS 15
#define PIN_LCD_DC 2
#define PIN_LCD_RST -1 /* no dedicated reset line on most CYD boards */
#define PIN_LCD_BL 21

#define LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define CLEAR_CHUNK_LINES 40

/*
 * CYD boards from different batches are not all wired/glassed identically.
 * If the image comes up rotated/mirrored or red and blue are swapped,
 * flip these and reflash.
 */
#define CYD_SWAP_XY true
#define CYD_MIRROR_X true
#define CYD_MIRROR_Y false
#define CYD_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR

static esp_lcd_panel_handle_t s_panel;

static void init_backlight(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(PIN_LCD_BL, 1);
}

static void clear_screen(void)
{
    static uint16_t clear_buf[ST7789_WIDTH * CLEAR_CHUNK_LINES];
    memset(clear_buf, 0x00, sizeof(clear_buf)); /* 0x0000 = black */

    for (int y = 0; y < ST7789_HEIGHT; y += CLEAR_CHUNK_LINES) {
        int lines = CLEAR_CHUNK_LINES;
        if (y + lines > ST7789_HEIGHT) {
            lines = ST7789_HEIGHT - y;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, y, ST7789_WIDTH, y + lines, clear_buf));
    }
}

void st7789_init(void)
{
    init_backlight();

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST7789_WIDTH * CLEAR_CHUNK_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .pclk_hz = LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = CYD_RGB_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, CYD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, CYD_MIRROR_X, CYD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    clear_screen();
}

void st7789_draw_line(uint16_t *buf, int y, int height, int scale, uint16_t fg_color, uint16_t bg_color,
                       const char *text)
{
    for (int i = 0; i < ST7789_WIDTH * height; i++) {
        buf[i] = bg_color;
    }

    int cell_w = (FONT5X7_WIDTH + 1) * scale;
    int text_len = (int)strlen(text);
    int total_w = text_len * cell_w;
    int start_x = (ST7789_WIDTH - total_w) / 2;
    if (start_x < 0) {
        start_x = 0;
    }

    for (int i = 0; i < text_len; i++) {
        const uint8_t *glyph = font5x7_glyph(text[i]);
        int base_x = start_x + i * cell_w;

        for (int row = 0; row < FONT5X7_HEIGHT; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT5X7_WIDTH; col++) {
                if (!(bits & (1 << (FONT5X7_WIDTH - 1 - col)))) {
                    continue;
                }
                for (int sy = 0; sy < scale; sy++) {
                    int py = row * scale + sy;
                    if (py < 0 || py >= height) {
                        continue;
                    }
                    for (int sx = 0; sx < scale; sx++) {
                        int px = base_x + col * scale + sx;
                        if (px < 0 || px >= ST7789_WIDTH) {
                            continue;
                        }
                        buf[py * ST7789_WIDTH + px] = fg_color;
                    }
                }
            }
        }
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, y, ST7789_WIDTH, y + height, buf));
}
