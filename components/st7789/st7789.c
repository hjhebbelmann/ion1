#include "st7789.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "soc/soc_caps.h"

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

/* LVGL renders into two partial buffers of this many lines each (DMA, internal RAM). */
#define LVGL_DRAW_BUF_LINES 40

/*
 * The time-sensitive motor/BMS loop in main.cpp is pinned to the second core
 * at priority 5. Keep LVGL rendering off that core and below that priority.
 */
#define LVGL_TASK_PRIORITY 2
#if SOC_CPU_CORES_NUM > 1
#define LVGL_TASK_CORE 0
#else
#define LVGL_TASK_CORE -1
#endif

/*
 * CYD boards from different batches are not all wired/glassed identically.
 * If the image comes up rotated/mirrored or red and blue are swapped,
 * flip these and reflash. If black and white are swapped, set CYD_INVERT_COLOR.
 */
#define CYD_SWAP_XY true
#define CYD_MIRROR_X true
#define CYD_MIRROR_Y false
#define CYD_RGB_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#define CYD_INVERT_COLOR false

static lv_display_t *s_disp;

static void init_backlight(void)
{
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    /* Keep it dark until the panel is initialized, to hide power-up garbage. */
    gpio_set_level(PIN_LCD_BL, 0);
}

lv_display_t *st7789_init(void)
{
    if (s_disp != NULL) {
        return s_disp;
    }

    init_backlight();

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = ST7789_WIDTH * LVGL_DRAW_BUF_LINES * sizeof(uint16_t),
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

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = CYD_RGB_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, CYD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, CYD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, CYD_MIRROR_X, CYD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = LVGL_TASK_PRIORITY;
    port_cfg.task_affinity = LVGL_TASK_CORE;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel,
        .buffer_size = ST7789_WIDTH * LVGL_DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = ST7789_WIDTH,
        .vres = ST7789_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = CYD_SWAP_XY,
            .mirror_x = CYD_MIRROR_X,
            .mirror_y = CYD_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            /* SPI panels expect RGB565 big-endian; LVGL renders little-endian. */
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(s_disp != NULL ? ESP_OK : ESP_FAIL);

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(s_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lvgl_port_unlock();

    gpio_set_level(PIN_LCD_BL, 1);

    return s_disp;
}
