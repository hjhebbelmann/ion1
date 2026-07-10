#include "ext_monitor.h"

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

// On-screen layout: y-position/height/font-scale per line, top to bottom.
#define TITLE_Y 6
#define TITLE_H 14
#define TITLE_SCALE 2

#define LINE_H 21
#define LINE_SCALE 3
#define VOLT_Y 30
#define CURR_Y 58
#define POWER_Y 86
#define ENERGY_Y 114

#define STATUS_Y 200
#define STATUS_H 14
#define STATUS_SCALE 2

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
    st7789_init();

    static uint16_t titleBuf[ST7789_WIDTH * TITLE_H];
    static uint16_t voltBuf[ST7789_WIDTH * LINE_H];
    static uint16_t currBuf[ST7789_WIDTH * LINE_H];
    static uint16_t powerBuf[ST7789_WIDTH * LINE_H];
    static uint16_t energyBuf[ST7789_WIDTH * LINE_H];
    static uint16_t statusBuf[ST7789_WIDTH * STATUS_H];

    st7789_draw_line(titleBuf, TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "POWER");

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

        char buf[40];
        if(vErr == ESP_OK && iErr == ESP_OK) {
            float volts = ads1115_to_volts(rawV, VOLTAGE_FSR);
            float shuntVolts = ads1115_to_volts(rawI, CURRENT_FSR);
            float amps = shuntVolts / SHUNT_OHMS;
            float watts = volts * amps;
            energyJoules += (double)watts * (double)dtS;
            float energyWh = (float)(energyJoules / 3600.0);

            snprintf(buf, sizeof(buf), "V %.3fV", volts);
            st7789_draw_line(voltBuf, VOLT_Y, LINE_H, LINE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);
            snprintf(buf, sizeof(buf), "I %.3fA", amps);
            st7789_draw_line(currBuf, CURR_Y, LINE_H, LINE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);
            snprintf(buf, sizeof(buf), "P %.3fW", watts);
            st7789_draw_line(powerBuf, POWER_Y, LINE_H, LINE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);
            snprintf(buf, sizeof(buf), "E %.4fWH", energyWh);
            st7789_draw_line(energyBuf, ENERGY_Y, LINE_H, LINE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);
            st7789_draw_line(statusBuf, STATUS_Y, STATUS_H, STATUS_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "");
        } else {
            st7789_draw_line(statusBuf, STATUS_Y, STATUS_H, STATUS_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "I2C ERROR");
            ESP_LOGW(TAG, "ADS1115 read failed: v=%s i=%s", esp_err_to_name(vErr), esp_err_to_name(iErr));
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void initExtMonitor() {
    // Runs as its own task (not pinned to the motor-control core) so the
    // ADS1115's I2C polling (up to ~50ms per read) never delays the
    // time-sensitive motor/BMS UART handoff loop in main.cpp.
    xTaskCreate(extMonitorTask, "extMonitor", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}
