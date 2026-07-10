#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADS1115_I2C_ADDR_GND 0x48 /* ADDR pin tied to GND (default) */

typedef enum {
    ADS1115_MUX_AIN0_AIN1 = 0b000, /* differential */
    ADS1115_MUX_AIN0_AIN3 = 0b001, /* differential */
    ADS1115_MUX_AIN1_AIN3 = 0b010, /* differential */
    ADS1115_MUX_AIN2_AIN3 = 0b011, /* differential */
    ADS1115_MUX_AIN0_GND = 0b100,  /* single-ended */
    ADS1115_MUX_AIN1_GND = 0b101,  /* single-ended */
    ADS1115_MUX_AIN2_GND = 0b110,  /* single-ended */
    ADS1115_MUX_AIN3_GND = 0b111,  /* single-ended */
} ads1115_mux_t;

typedef enum {
    ADS1115_FSR_6V144 = 0b000, /* +/- 6.144V, LSB = 187.5uV */
    ADS1115_FSR_4V096 = 0b001, /* +/- 4.096V, LSB = 125uV */
    ADS1115_FSR_2V048 = 0b010, /* +/- 2.048V, LSB = 62.5uV (default) */
    ADS1115_FSR_1V024 = 0b011, /* +/- 1.024V, LSB = 31.25uV */
    ADS1115_FSR_0V512 = 0b100, /* +/- 0.512V, LSB = 15.625uV */
    ADS1115_FSR_0V256 = 0b101, /* +/- 0.256V, LSB = 7.8125uV */
} ads1115_fsr_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    uint16_t i2c_addr;
} ads1115_config_t;

typedef struct ads1115_dev *ads1115_handle_t;

esp_err_t ads1115_init(const ads1115_config_t *config, ads1115_handle_t *out_handle);

/*
 * Starts a single-shot conversion on `mux` at gain `fsr`, blocks (polling
 * the config register's OS bit) until it completes, and returns the raw
 * signed 16-bit result. Since mux/gain are set per call, this is what lets
 * one ADS1115 time-share multiple inputs with different ranges.
 */
esp_err_t ads1115_read_single(ads1115_handle_t handle, ads1115_mux_t mux, ads1115_fsr_t fsr, int16_t *out_raw);

/* Converts a raw reading to volts for the gain setting it was taken at. */
float ads1115_to_volts(int16_t raw, ads1115_fsr_t fsr);

#ifdef __cplusplus
}
#endif
