#pragma once

#include <stdint.h>

// getMotorDeciVolts() value when no motor voltage reading is available.
#define MOTOR_VOLTAGE_UNKNOWN 0xFFFF
// getMotorDeciDegrees() value when no motor temperature reading is available.
#define MOTOR_TEMP_UNKNOWN INT16_MIN
// getMotorTotal() value when no motor total distance reading is available.
#define MOTOR_TOTAL_UNKNOWN 0xFFFFFFFF

void initMotor();
void motorUpdate();
void startMotorUpdates();
void stopMotorUpdates();
bool handleMotorUpdate();

/**
 * Last motor voltage read from the motor (GET DATA 14:45), in 100mV,
 * or MOTOR_VOLTAGE_UNKNOWN. Polled every 4s while motor updates run (ST7789 builds only).
 */
uint16_t getMotorDeciVolts();

/**
 * Last motor temperature read from the motor (GET DATA 14:46), in 0.1 degrees C,
 * or MOTOR_TEMP_UNKNOWN. Polled every 4s while motor updates run (ST7789 builds only).
 */
int16_t getMotorDeciDegrees();

/**
 * Last total distance read from the motor (GET DATA 08:80), in 10m increments,
 * or MOTOR_TOTAL_UNKNOWN. Polled every 4s while motor updates run (ST7789 builds only).
 */
uint32_t getMotorTotal();
