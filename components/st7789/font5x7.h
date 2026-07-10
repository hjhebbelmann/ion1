#pragma once

#include <stdint.h>

#define FONT5X7_WIDTH 5
#define FONT5X7_HEIGHT 7

/*
 * Returns 7 bytes, one per pixel row (top to bottom). In each byte, bit 4 is
 * the leftmost column and bit 0 is the rightmost column of the glyph.
 * Covers only the characters this project's UI strings use: digits, space,
 * '.', '-', ':', and letters A B C D E F G H I L N O P R S T V W Y. Anything
 * else falls back to a blank (space) glyph.
 */
const uint8_t *font5x7_glyph(char c);
