#include "st7789_display.h"
#if CONFIG_ION_ST7789

#include <stdio.h>

#include "bat.h"
#include "st7789.h"

// On-screen layout: y-position/height/font-scale per line, top to bottom.
#define TITLE_H 14
#define TITLE_SCALE 2
#define VAL_H 21
#define VAL_SCALE 3
#define STATUS_H 14
#define STATUS_SCALE 2

#define SPEED_TITLE_Y 4
#define SPEED_VAL_Y 20
#define ASSIST_TITLE_Y 45
#define ASSIST_VAL_Y 61
#define TRIP_TITLE_Y 86
#define TRIP_VAL_Y 102
#define BAT_TITLE_Y 127
#define BAT_VAL_Y 143
#define STATUS_Y 168

// One dedicated buffer per on-screen line/region: the SPI transfer each
// draw kicks off is asynchronous, so a buffer can't be reused for another
// region until that transfer is done. The charging screen reuses the
// speedTitleBuf/speedValBuf/batTitleBuf slots since it's never drawn in the
// same call as the riding dashboard.
static uint16_t speedTitleBuf[ST7789_WIDTH * TITLE_H];
static uint16_t speedValBuf[ST7789_WIDTH * VAL_H];
static uint16_t assistTitleBuf[ST7789_WIDTH * TITLE_H];
static uint16_t assistValBuf[ST7789_WIDTH * VAL_H];
static uint16_t tripTitleBuf[ST7789_WIDTH * TITLE_H];
static uint16_t tripValBuf[ST7789_WIDTH * VAL_H];
static uint16_t batTitleBuf[ST7789_WIDTH * TITLE_H];
static uint16_t batValBuf[ST7789_WIDTH * VAL_H];
static uint16_t statusBuf[ST7789_WIDTH * STATUS_H];

void initSt7789Display() {
    st7789_init();
}

// Assist mode: 0:'0', 1:'1', 2:'2', 3:'3', 4:'P', 5:'R', 7:'4'.
static const char *assistLevelText(uint8_t assist) {
    switch(assist) {
        case 0: return "0";
        case 1: return "1";
        case 2: return "2";
        case 3: return "3";
        case 4: return "P";
        case 5: return "R";
        case 7: return "4";
        default: return "-";
    }
}

static void clearDashboard() {
    st7789_draw_line(speedTitleBuf, SPEED_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(speedValBuf, SPEED_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(assistTitleBuf, ASSIST_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(assistValBuf, ASSIST_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(tripTitleBuf, TRIP_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(tripValBuf, TRIP_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(batTitleBuf, BAT_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(batValBuf, BAT_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
    st7789_draw_line(statusBuf, STATUS_Y, STATUS_H, STATUS_SCALE, ST7789_COLOR_BLACK, ST7789_COLOR_BLACK, "");
}

static void drawRidingDashboard(bool light, uint8_t assist, uint16_t speed, uint32_t trip1) {
    char buf[16];

    st7789_draw_line(speedTitleBuf, SPEED_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "SPEED");
    snprintf(buf, sizeof(buf), "%u.%u", speed / 10, speed % 10);
    st7789_draw_line(speedValBuf, SPEED_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);

    st7789_draw_line(assistTitleBuf, ASSIST_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "ASSIST");
    st7789_draw_line(assistValBuf, ASSIST_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, assistLevelText(assist));

    // trip1 is in 10m increments, so 100 units == 1.00km exactly.
    st7789_draw_line(tripTitleBuf, TRIP_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "TRIP");
    snprintf(buf, sizeof(buf), "%lu.%02lu", (unsigned long)(trip1 / 100), (unsigned long)(trip1 % 100));
    st7789_draw_line(tripValBuf, TRIP_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);

    st7789_draw_line(batTitleBuf, BAT_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "BATTERY");
    snprintf(buf, sizeof(buf), "%u", getBatPercentage());
    st7789_draw_line(batValBuf, BAT_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);

    st7789_draw_line(statusBuf, STATUS_Y, STATUS_H, STATUS_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, light ? "LIGHT ON" : "LIGHT OFF");
}

static void drawChargingDashboard() {
    char buf[16];

    st7789_draw_line(speedTitleBuf, SPEED_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "CHARGING");
    snprintf(buf, sizeof(buf), "%u", getBatPercentage());
    st7789_draw_line(speedValBuf, SPEED_VAL_Y, VAL_H, VAL_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, buf);
    st7789_draw_line(batTitleBuf, BAT_TITLE_Y, TITLE_H, TITLE_SCALE, ST7789_COLOR_WHITE, ST7789_COLOR_BLACK, "BATTERY");
}

void displayUpdateSt7789(st7789_display_type type, bool screen, bool light, uint8_t assist, uint16_t speed, uint32_t trip1) {
    if(!screen) {
        clearDashboard();
        return;
    }

    if(type == ST7789_DSP_BAT_CHARGE) {
        drawChargingDashboard();
    } else {
        drawRidingDashboard(light, assist, speed, trip1);
    }
}

#endif
