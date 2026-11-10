#pragma once

/**
 * Events of the board button (CONFIG_ION_BUTTON_BOARD_PIN). Everything that
 * should act exactly like that button feeds its events through here: the
 * board button itself, the external button (CONFIG_ION_BUTTON_EXTERNAL_PIN)
 * and the on-screen start button of the ST7789 display.
 *
 *  BOARD_BUTTON_UP:   released, after any press length -> "mode short press"
 *  BOARD_BUTTON_HELD: still held after the long press time
 *                     (CONFIG_ESP32_BUTTON_LONG_PRESS_DURATION_MS)
 *                     -> "light long press", once per press; repeats are ignored
 */
enum board_button_event { BOARD_BUTTON_UP, BOARD_BUTTON_HELD };

// Safe to call from any task.
void boardButtonEvent(board_button_event event);
