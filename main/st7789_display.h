#pragma once

#include "sdkconfig.h"
#if CONFIG_ION_ST7789

#include <sys/unistd.h>

// Analog to cu3.h's display_type: which of the local dashboard screens to draw.
enum st7789_display_type { ST7789_DSP_SCREEN = 0, ST7789_DSP_BAT_CHARGE };

// Brings up the panel + LVGL and builds the UI, starting on the start screen.
void initSt7789Display();

/**
 * Switches back to the start screen (the dashboard-off state), e.g. when the
 * motor is being turned off. Non-blocking, safe to call from the main loop.
 */
void showStartScreenSt7789();

/**
 * Updates the local ST7789 (LVGL) dashboard. Analog to displayUpdateCu3(), but renders
 * to the local screen instead of pushing a message over the CU3 UART protocol.
 *
 * Non-blocking: only stores a snapshot of the values; the LVGL task picks it
 * up within ~50ms and redraws. Safe to call from the time-sensitive main loop.
 *
 * @param type The screen to draw (normal riding dashboard, or the charging screen)
 * @param screen Screen on or off (off blanks the dashboard, same meaning as for CU3)
 * @param light Light on or off
 * @param assist Assist mode: 0:'0', 1:'1', 2:'2', 3:'3', 4:'P', 5:'R', 7:'4'
 * @param speed Speed in 100m/h increments (km/h * 10)
 * @param trip1 Trip 1 distance in 10m increments
 */
void displayUpdateSt7789(st7789_display_type type, bool screen, bool light, uint8_t assist, uint16_t speed, uint32_t trip1);

#endif
