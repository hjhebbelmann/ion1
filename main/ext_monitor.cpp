#include "ext_monitor.h"

#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ads1115.h"
#include "st7789.h"

static const char *TAG = "ext_monitor";

// I2C wiring for the ADS1115, on the CYD board's CN1 expansion header.
#define I2C_PORT I2C_NUM_0
#define PIN_I2C_SDA 27
#define PIN_I2C_SCL 22

// A0 measures the battery voltage directly (single-ended vs. GND). A2/A3
// measure the voltage drop across a current-sense shunt resistor
// (differential, A2 minus A3 - swap the shunt's A2/A3 wiring if the current
// reads with the wrong sign). Tune to the actual wiring:
//  - VOLTAGE_FSR must cover the highest voltage A0 will see (pack voltage).
//  - SHUNT_OHMS is the actual resistance of the current-sense shunt.
//  - CURRENT_FSR must cover the shunt's max voltage drop (I_max * SHUNT_OHMS)
//    - pick the smallest range that still fits it for the best resolution.
#define VOLTAGE_FSR ADS1115_FSR_4V096
#define CURRENT_FSR ADS1115_FSR_2V048
#define SHUNT_OHMS 0.1f

#define POLL_PERIOD_MS 200

// On-screen layout: a title bar, then one full-width row per quantity
// (name on the left, value right-aligned), and a status line at the bottom.
#define TITLE_H 30
#define STATUS_H 24
#define ROW_H ((ST7789_HEIGHT - TITLE_H - STATUS_H) / 4)
#define ROW_PAD_X 10

#define COLOR_TITLE lv_color_hex(0x9E9E9E)
#define COLOR_VALUE lv_color_white()
#define COLOR_ERROR lv_color_hex(0xE53935)
#define COLOR_ROW_ALT lv_color_hex(0x1C1C1C)

struct MonitorUi {
    lv_obj_t *volt;
    lv_obj_t *curr;
    lv_obj_t *power;
    lv_obj_t *energy;
    lv_obj_t *status;
};

static lv_obj_t *createLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

// One full-width row: name on the left, value right-aligned. Returns the value label.
static lv_obj_t *createRow(lv_obj_t *scr, int index, const char *name) {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(row, 0, TITLE_H + index * ROW_H);
    lv_obj_set_size(row, ST7789_WIDTH, ROW_H);
    lv_obj_set_style_pad_hor(row, ROW_PAD_X, 0);
    if(index % 2 == 0) {
        lv_obj_set_style_bg_color(row, COLOR_ROW_ALT, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    }

    lv_obj_t *nameLabel = createLabel(row, &lv_font_montserrat_16, COLOR_TITLE, name);
    lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *valueLabel = createLabel(row, &lv_font_montserrat_32, COLOR_VALUE, "-");
    lv_obj_align(valueLabel, LV_ALIGN_RIGHT_MID, 0, 0);
    return valueLabel;
}

static void createUi(lv_display_t *disp, MonitorUi *ui) {
    lv_obj_t *scr = lv_display_get_screen_active(disp);

    lv_obj_t *title = createLabel(scr, &lv_font_montserrat_16, COLOR_TITLE, "POWER");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, (TITLE_H - 16) / 2);

    ui->volt = createRow(scr, 0, "VOLTAGE");
    ui->curr = createRow(scr, 1, "CURRENT");
    ui->power = createRow(scr, 2, "POWER");
    ui->energy = createRow(scr, 3, "ENERGY");

    ui->status = createLabel(scr, &lv_font_montserrat_16, COLOR_ERROR, "");
    lv_obj_align(ui->status, LV_ALIGN_BOTTOM_MID, 0, -(STATUS_H - 16) / 2);
}

static i2c_master_bus_handle_t initI2cBus() {
    i2c_master_bus_config_t busConfig = {};
    busConfig.i2c_port = I2C_PORT;
    busConfig.sda_io_num = (gpio_num_t)PIN_I2C_SDA;
    busConfig.scl_io_num = (gpio_num_t)PIN_I2C_SCL;
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t busHandle = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&busConfig, &busHandle));
    return busHandle;
}

static void extMonitorTask(void *pvParameter) {
    lv_display_t *disp = st7789_init();

    MonitorUi ui = {};
    lvgl_port_lock(0);
    createUi(disp, &ui);
    lvgl_port_unlock();

    i2c_master_bus_handle_t i2cBus = initI2cBus();

    ads1115_config_t adsCfg = {};
    adsCfg.bus = i2cBus;
    adsCfg.i2c_addr = ADS1115_I2C_ADDR_GND;

    ads1115_handle_t ads = NULL;
    ESP_ERROR_CHECK(ads1115_init(&adsCfg, &ads));

    double energyJoules = 0.0;
    int64_t lastUs = esp_timer_get_time();

    while(true) {
        int16_t rawV = 0;
        int16_t rawI = 0;
        esp_err_t vErr = ads1115_read_single(ads, ADS1115_MUX_AIN0_GND, VOLTAGE_FSR, &rawV);
        esp_err_t iErr = ads1115_read_single(ads, ADS1115_MUX_AIN2_AIN3, CURRENT_FSR, &rawI);

        int64_t nowUs = esp_timer_get_time();
        float dtS = (float)(nowUs - lastUs) / 1e6f;
        lastUs = nowUs;

        // Do the slow I2C work and formatting outside the LVGL lock; only hold
        // it while handing the new texts to LVGL.
        char voltText[16], currText[16], powerText[16], energyText[20];
        const bool ok = vErr == ESP_OK && iErr == ESP_OK;
        if(ok) {
            float volts = ads1115_to_volts(rawV, VOLTAGE_FSR);
            float shuntVolts = ads1115_to_volts(rawI, CURRENT_FSR);
            float amps = shuntVolts / SHUNT_OHMS;
            float watts = volts * amps;
            energyJoules += (double)watts * (double)dtS;
            float energyWh = (float)(energyJoules / 3600.0);

            snprintf(voltText, sizeof(voltText), "%.3f V", volts);
            snprintf(currText, sizeof(currText), "%.3f A", amps);
            snprintf(powerText, sizeof(powerText), "%.3f W", watts);
            snprintf(energyText, sizeof(energyText), "%.4f Wh", energyWh);
        } else {
            ESP_LOGW(TAG, "ADS1115 read failed: v=%s i=%s", esp_err_to_name(vErr), esp_err_to_name(iErr));
        }

        lvgl_port_lock(0);
        if(ok) {
            lv_label_set_text(ui.volt, voltText);
            lv_label_set_text(ui.curr, currText);
            lv_label_set_text(ui.power, powerText);
            lv_label_set_text(ui.energy, energyText);
            lv_label_set_text(ui.status, "");
        } else {
            lv_label_set_text(ui.status, "I2C ERROR");
        }
        lvgl_port_unlock();

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void initExtMonitor() {
    // Runs as its own task (not pinned to the motor-control core) so the
    // ADS1115's I2C polling (up to ~50ms per read) never delays the
    // time-sensitive motor/BMS UART handoff loop in main.cpp. LVGL rendering
    // itself happens in the esp_lvgl_port task (see st7789.c).
    xTaskCreate(extMonitorTask, "extMonitor", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}
