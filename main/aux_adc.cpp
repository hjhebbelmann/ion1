#include "aux_adc.h"
#if CONFIG_ION_ST7789_ADS1115

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include "ads1115.h"
#include "st7789.h"

static const char *TAG = "aux_adc";

#define PIN_SDA CONFIG_ION_ST7789_ADS1115_SDA
#define PIN_SCL CONFIG_ION_ST7789_ADS1115_SCL
#define I2C_PORT I2C_NUM_0

// Single-ended inputs can't go above the ADS1115's supply (3.3V on the CYD),
// so +/-4.096V covers the whole usable range.
#define AUX_FSR ADS1115_FSR_4V096

#define PROBE_PERIOD_MS 2000
#define POLL_PERIOD_MS 250

// Catch configs that put the I2C bus on a pin that's already in use.
#define IS_AUX_PIN(p) ((p) == PIN_SDA || (p) == PIN_SCL)
#if PIN_SDA == PIN_SCL
#error "CONFIG_ION_ST7789_ADS1115_SDA and _SCL must differ"
#endif
#if IS_AUX_PIN(CONFIG_ION_RXD) || IS_AUX_PIN(CONFIG_ION_TXD)
#error "The ADS1115 I2C pins collide with the motor bus UART (CONFIG_ION_RXD/TXD)"
#endif
#if IS_AUX_PIN(CONFIG_ION_LED_PIN)
#error "The ADS1115 I2C pins collide with CONFIG_ION_LED_PIN"
#endif
#if (CONFIG_ION_LIGHT && IS_AUX_PIN(CONFIG_ION_LIGHT_PIN)) || (CONFIG_ION_RELAY && IS_AUX_PIN(CONFIG_ION_RELAY_PIN)) || \
    (CONFIG_ION_CHARGE && IS_AUX_PIN(CONFIG_ION_CHARGE_PIN))
#error "The ADS1115 I2C pins collide with the light, relay or charge pin"
#endif
#if CONFIG_ION_BUTTON && (IS_AUX_PIN(CONFIG_ION_BUTTON_BOARD_PIN) || IS_AUX_PIN(CONFIG_ION_BUTTON_EXTERNAL_PIN))
#error "The ADS1115 I2C pins collide with a button pin"
#endif
#if ST7789_HAS_TOUCH && (IS_AUX_PIN(ST7789_TOUCH_PIN_SCLK) || IS_AUX_PIN(ST7789_TOUCH_PIN_MOSI) || \
                         IS_AUX_PIN(ST7789_TOUCH_PIN_MISO) || IS_AUX_PIN(ST7789_TOUCH_PIN_CS))
#error "The ADS1115 I2C pins collide with the CYD touch controller"
#endif
// CYD display: SPI 12/13/14/15, DC 2, backlight 21.
#if IS_AUX_PIN(2) || IS_AUX_PIN(12) || IS_AUX_PIN(13) || IS_AUX_PIN(14) || IS_AUX_PIN(15) || IS_AUX_PIN(21)
#error "The ADS1115 I2C pins collide with the CYD display"
#endif

static const ads1115_mux_t channels[AUX_ADC_CHANNELS] = {
    ADS1115_MUX_AIN0_GND, ADS1115_MUX_AIN1_GND, ADS1115_MUX_AIN2_GND, ADS1115_MUX_AIN3_GND,
};

static portMUX_TYPE valuesMux = portMUX_INITIALIZER_UNLOCKED;
static bool found = false;
static bool valid = false;
static float values[AUX_ADC_CHANNELS];

bool auxAdcFound() {
    taskENTER_CRITICAL(&valuesMux);
    bool result = found;
    taskEXIT_CRITICAL(&valuesMux);
    return result;
}

bool auxAdcGetVolts(float volts[AUX_ADC_CHANNELS]) {
    taskENTER_CRITICAL(&valuesMux);
    bool result = valid;
    for(int i = 0; i < AUX_ADC_CHANNELS; i++) {
        volts[i] = values[i];
    }
    taskEXIT_CRITICAL(&valuesMux);
    return result;
}

static void auxAdcTask(void *pvParameter) {
    i2c_master_bus_config_t busConfig = {};
    busConfig.i2c_port = I2C_PORT;
    busConfig.sda_io_num = (gpio_num_t)PIN_SDA;
    busConfig.scl_io_num = (gpio_num_t)PIN_SCL;
    busConfig.clk_source = I2C_CLK_SRC_DEFAULT;
    busConfig.glitch_ignore_cnt = 7;
    busConfig.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&busConfig, &bus);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    // Wait for an ADS1115 to show up (it may also be connected later).
    while(i2c_master_probe(bus, ADS1115_I2C_ADDR_GND, 50) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));
    }

    ads1115_config_t adsCfg = {};
    adsCfg.bus = bus;
    adsCfg.i2c_addr = ADS1115_I2C_ADDR_GND;
    ads1115_handle_t ads = NULL;
    err = ads1115_init(&adsCfg, &ads);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ADS1115 found on SDA %d / SCL %d", PIN_SDA, PIN_SCL);
    taskENTER_CRITICAL(&valuesMux);
    found = true;
    taskEXIT_CRITICAL(&valuesMux);

    while(true) {
        float volts[AUX_ADC_CHANNELS];
        bool ok = true;
        for(int i = 0; i < AUX_ADC_CHANNELS && ok; i++) {
            int16_t raw = 0;
            err = ads1115_read_single(ads, channels[i], AUX_FSR, &raw);
            if(err == ESP_OK) {
                volts[i] = ads1115_to_volts(raw, AUX_FSR);
            } else {
                ESP_LOGW(TAG, "ADS1115 read A%d failed: %s", i, esp_err_to_name(err));
                ok = false;
            }
        }

        taskENTER_CRITICAL(&valuesMux);
        valid = ok;
        if(ok) {
            for(int i = 0; i < AUX_ADC_CHANNELS; i++) {
                values[i] = volts[i];
            }
        }
        taskEXIT_CRITICAL(&valuesMux);

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void initAuxAdc() {
    // Low priority, and off the core running the time-sensitive motor loop.
#if SOC_CPU_CORES_NUM > 1
    xTaskCreatePinnedToCore(auxAdcTask, "auxAdc", 3072, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
#else
    xTaskCreate(auxAdcTask, "auxAdc", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
#endif
}

#endif
