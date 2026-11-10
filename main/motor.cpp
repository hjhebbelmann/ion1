#include <sys/unistd.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "bytes.h"
#include "bat.h"
#include "bow.h"
#include "cmds.h"
#include "motor.h"

static EventGroupHandle_t eventGroupHandle;
static TimerHandle_t motorUpdateTimer;

static const int MOTOR_UPDATE_BIT = BIT0;

static void motorUpdateTimerCallback(TimerHandle_t xTimer) { xEventGroupSetBits(eventGroupHandle, MOTOR_UPDATE_BIT); }

#if CONFIG_ION_ST7789
// Motor voltage, temperature and total distance are only polled when there is a display to show them on.
#define MOTOR_DATA_POLL

static TimerHandle_t motorDataTimer;

static const int MOTOR_VOLTAGE_BIT = BIT1;
static const int MOTOR_TEMP_BIT = BIT2;
static const int MOTOR_TOTAL_BIT = BIT3;
static const int MOTOR_DATA_BITS = MOTOR_VOLTAGE_BIT | MOTOR_TEMP_BIT | MOTOR_TOTAL_BIT;

// Register 14:45, motor voltage in 100mV. MOTOR_VOLTAGE_UNKNOWN until the motor answered.
static volatile uint16_t motorDeciVolts = MOTOR_VOLTAGE_UNKNOWN;
// Register 14:46, motor temperature in 0.1 degrees C. MOTOR_TEMP_UNKNOWN until the motor answered.
static volatile int16_t motorDeciDegrees = MOTOR_TEMP_UNKNOWN;
// Register 08:80, motor total distance in 10m increments. MOTOR_TOTAL_UNKNOWN until the motor answered.
static volatile uint32_t motorTotal = MOTOR_TOTAL_UNKNOWN;

static void motorDataTimerCallback(TimerHandle_t xTimer) { xEventGroupSetBits(eventGroupHandle, MOTOR_DATA_BITS); }
#endif

void initMotor() {
    eventGroupHandle = xEventGroupCreate();
    motorUpdateTimer = xTimerCreate("motorUpdateTimer", (10000 / portTICK_PERIOD_MS), pdTRUE, (void *)0, motorUpdateTimerCallback);
#ifdef MOTOR_DATA_POLL
    motorDataTimer = xTimerCreate("motorDataTimer", (4000 / portTICK_PERIOD_MS), pdTRUE, (void *)0, motorDataTimerCallback);
#endif
}

/**
 * Put data from bat. to motor. Sent initially, and then every 10, 15, 50 seconds? (no good recording with timging yet).
 * Values:
 * b0 - Almost always 2500, only seen it lower on low bat uphill.
 * b1 - Volts in 100mv Goes up and down, so likely current voltage, even under load.
 */
void motorUpdate() {
    uint16_t unknown = 2500; // Normally 2500, very sometimes much lower, on low battery up hill? Amp limit in 10ma??
    uint16_t volts = getBatMv() / 100; // Volts, in 100mv

    uint8_t payload[] = {
            0x94, 0xb0,
            FROM_UINT16(unknown),
            0x14, 0xb1,
            FROM_UINT16(volts)};
    messageType response = {};
    readResult result = exchange(cmdReq(MSG_MOTOR, MSG_BMS, CMD_PUT_DATA, payload, sizeof(payload)), &response, 225 / portTICK_PERIOD_MS);
}

#ifdef MOTOR_DATA_POLL
/**
 * GET DATA of a single register from the motor. The descriptor's low nibble gives the
 * value size: 0x_4 is 2 bytes (e.g. 14:45), 0x_8 is 4 bytes (e.g. 08:80).
 * Request:  10 01 22 08 <descriptor> <id> <crc>
 * Response: 10 42 0<3+size> 08 00 <descriptor> <id> <value, big endian> <crc>
 *
 * @return true when the motor answered with that register's value.
 */
static bool motorGetData(uint8_t descriptor, uint8_t id, uint32_t *value) {
    const size_t size = (descriptor & 0x0f) == 0x08 ? 4 : 2;
    uint8_t payload[] = {descriptor, id};
    messageType response = {};
    readResult result = exchange(cmdReq(MSG_MOTOR, MSG_BMS, CMD_GET_DATA, payload, sizeof(payload)), &response, 225 / portTICK_PERIOD_MS, 3);
    if(result == MSG_OK && response.type == MSG_CMD_RESP && response.command == CMD_GET_DATA && response.payloadSize == 3 + size &&
       response.payload[0] == 0x00 && response.payload[1] == descriptor && response.payload[2] == id) {
        *value = size == 4 ? toUint32(response.payload, 3) : toUint16(response.payload, 3);
        return true;
    }
    return false;
}

/**
 * GET DATA 1445 14:45 (Motor voltage), e.g. 0x00ce = 206 = 20.6V
 * Keeps the last value if the motor does not answer properly.
 */
static void motorVoltageRequest() {
    uint32_t value;
    if(motorGetData(0x14, 0x45, &value)) {
        motorDeciVolts = (uint16_t) value;
    }
}

/**
 * GET DATA 1446 14:46 (Motor temperature), e.g. 0x00b0 = 176 = 17.6 degrees C
 * Read as signed, in case it goes below zero.
 * Keeps the last value if the motor does not answer properly.
 */
static void motorTempRequest() {
    uint32_t value;
    if(motorGetData(0x14, 0x46, &value)) {
        motorDeciDegrees = (int16_t) (uint16_t) value;
    }
}

/**
 * GET DATA 0880 08:80 (Motor total distance), e.g. 0x0011afc4 = 1159108 = 11591.08 km
 * Keeps the last value if the motor does not answer properly.
 */
static void motorTotalRequest() {
    uint32_t value;
    if(motorGetData(0x08, 0x80, &value)) {
        motorTotal = value;
    }
}
#endif

uint16_t getMotorDeciVolts() {
#ifdef MOTOR_DATA_POLL
    return motorDeciVolts;
#else
    return MOTOR_VOLTAGE_UNKNOWN;
#endif
}

int16_t getMotorDeciDegrees() {
#ifdef MOTOR_DATA_POLL
    return motorDeciDegrees;
#else
    return MOTOR_TEMP_UNKNOWN;
#endif
}

uint32_t getMotorTotal() {
#ifdef MOTOR_DATA_POLL
    return motorTotal;
#else
    return MOTOR_TOTAL_UNKNOWN;
#endif
}

void startMotorUpdates() {
    xTimerStart(motorUpdateTimer, 0);
#ifdef MOTOR_DATA_POLL
    // First readings right away, then every 4 seconds.
    xEventGroupSetBits(eventGroupHandle, MOTOR_DATA_BITS);
    xTimerStart(motorDataTimer, 0);
#endif
}

void stopMotorUpdates() {
    xTimerStop(motorUpdateTimer, 0);
#ifdef MOTOR_DATA_POLL
    xTimerStop(motorDataTimer, 0);
    // Don't poll a motor that is turning off, and don't show stale values.
    xEventGroupClearBits(eventGroupHandle, MOTOR_DATA_BITS);
    motorDeciVolts = MOTOR_VOLTAGE_UNKNOWN;
    motorDeciDegrees = MOTOR_TEMP_UNKNOWN;
    motorTotal = MOTOR_TOTAL_UNKNOWN;
#endif
}

/**
 * Called from the main loop when the battery owns the bus, sends at most one message.
 */
bool handleMotorUpdate() {
    EventBits_t bitsToCheck = MOTOR_UPDATE_BIT;
#ifdef MOTOR_DATA_POLL
    bitsToCheck |= MOTOR_DATA_BITS;
#endif
    EventBits_t bits = xEventGroupWaitBits(eventGroupHandle, bitsToCheck, false, false, 0);
    if((bits & MOTOR_UPDATE_BIT) != 0) {
        xEventGroupClearBits(eventGroupHandle, MOTOR_UPDATE_BIT);
        motorUpdate();
        return true;
    }
#ifdef MOTOR_DATA_POLL
    if((bits & MOTOR_VOLTAGE_BIT) != 0) {
        xEventGroupClearBits(eventGroupHandle, MOTOR_VOLTAGE_BIT);
        motorVoltageRequest();
        return true;
    }
    if((bits & MOTOR_TEMP_BIT) != 0) {
        xEventGroupClearBits(eventGroupHandle, MOTOR_TEMP_BIT);
        motorTempRequest();
        return true;
    }
    if((bits & MOTOR_TOTAL_BIT) != 0) {
        xEventGroupClearBits(eventGroupHandle, MOTOR_TOTAL_BIT);
        motorTotalRequest();
        return true;
    }
#endif

    return false;
}
