#pragma once

#include "sdkconfig.h"
#if CONFIG_ION_ST7789_ADS1115

#define AUX_ADC_CHANNELS 4

/**
 * Starts a background task that looks for an ADS1115 on the I2C bus
 * (CONFIG_ION_ST7789_ADS1115_SDA/SCL) and, once found, keeps reading the
 * voltages of A0..A3 (single-ended). Doesn't block.
 */
void initAuxAdc();

// True once an ADS1115 has been found.
bool auxAdcFound();

/**
 * Copies the latest readings (volts, A0..A3) into `volts`.
 * Returns false if there is no valid reading (not found yet, or the last read failed).
 * Safe to call from any task.
 */
bool auxAdcGetVolts(float volts[AUX_ADC_CHANNELS]);

#endif
