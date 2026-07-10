#include "ads1115.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ads1115";

#define ADS1115_REG_CONVERSION 0x00
#define ADS1115_REG_CONFIG 0x01

#define ADS1115_OS_START_SINGLE (1 << 15)
#define ADS1115_OS_READY_MASK 0x80 /* bit 7 of the config register's high byte == bit 15 (OS) */
#define ADS1115_MODE_SINGLE_SHOT 1
#define ADS1115_DR_128SPS 0b100
#define ADS1115_COMP_QUE_DISABLE 0b11

#define I2C_TIMEOUT_MS 1000
#define CONVERSION_POLL_DELAY_MS 1
#define CONVERSION_MAX_POLLS 50 /* ~50ms cap, well above the ~8ms conversion time at 128SPS */

struct ads1115_dev {
    i2c_master_dev_handle_t dev;
};

static float ads1115_lsb_volts(ads1115_fsr_t fsr)
{
    static const float lsb_by_fsr[] = {
        [ADS1115_FSR_6V144] = 6.144f / 32768.0f,
        [ADS1115_FSR_4V096] = 4.096f / 32768.0f,
        [ADS1115_FSR_2V048] = 2.048f / 32768.0f,
        [ADS1115_FSR_1V024] = 1.024f / 32768.0f,
        [ADS1115_FSR_0V512] = 0.512f / 32768.0f,
        [ADS1115_FSR_0V256] = 0.256f / 32768.0f,
    };
    return lsb_by_fsr[fsr];
}

esp_err_t ads1115_init(const ads1115_config_t *config, ads1115_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config && out_handle, ESP_ERR_INVALID_ARG, TAG, "null arg");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->i2c_addr,
        .scl_speed_hz = 400000,
    };

    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(config->bus, &dev_cfg, &dev), TAG, "add device");

    struct ads1115_dev *handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no mem");
    handle->dev = dev;

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t ads1115_read_single(ads1115_handle_t handle, ads1115_mux_t mux, ads1115_fsr_t fsr, int16_t *out_raw)
{
    ESP_RETURN_ON_FALSE(handle && out_raw, ESP_ERR_INVALID_ARG, TAG, "null arg");

    uint16_t cfg_reg = ADS1115_OS_START_SINGLE | (mux << 12) | (fsr << 9) | (ADS1115_MODE_SINGLE_SHOT << 8) |
                        (ADS1115_DR_128SPS << 5) | ADS1115_COMP_QUE_DISABLE;

    uint8_t write_buf[3] = {
        ADS1115_REG_CONFIG,
        (uint8_t)(cfg_reg >> 8),
        (uint8_t)(cfg_reg & 0xFF),
    };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(handle->dev, write_buf, sizeof(write_buf), I2C_TIMEOUT_MS), TAG,
                         "write config");

    uint8_t reg = ADS1115_REG_CONFIG;
    uint8_t status[2];
    bool ready = false;
    for (int i = 0; i < CONVERSION_MAX_POLLS; i++) {
        ESP_RETURN_ON_ERROR(
            i2c_master_transmit_receive(handle->dev, &reg, sizeof(reg), status, sizeof(status), I2C_TIMEOUT_MS), TAG,
            "poll config");
        if (status[0] & ADS1115_OS_READY_MASK) {
            ready = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CONVERSION_POLL_DELAY_MS));
    }
    ESP_RETURN_ON_FALSE(ready, ESP_ERR_TIMEOUT, TAG, "conversion timed out");

    reg = ADS1115_REG_CONVERSION;
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(handle->dev, &reg, sizeof(reg), data, sizeof(data), I2C_TIMEOUT_MS), TAG,
        "read conversion");

    *out_raw = (int16_t)((data[0] << 8) | data[1]);
    return ESP_OK;
}

float ads1115_to_volts(int16_t raw, ads1115_fsr_t fsr)
{
    return raw * ads1115_lsb_volts(fsr);
}
