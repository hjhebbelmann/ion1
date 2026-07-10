#include "font5x7.h"

/*
 * Minimal hand-drawn 5x7 font covering only the characters this project's UI
 * strings actually use: digits, a handful of letters, space, '.', '-', ':'.
 * Bit 4 = leftmost column, bit 0 = rightmost column, one byte per row.
 */

static const uint8_t GLYPH_SPACE[FONT5X7_HEIGHT] = {
    0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000,
};
static const uint8_t GLYPH_DOT[FONT5X7_HEIGHT] = {
    0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00110,
};
static const uint8_t GLYPH_DASH[FONT5X7_HEIGHT] = {
    0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000,
};
static const uint8_t GLYPH_COLON[FONT5X7_HEIGHT] = {
    0b00000, 0b00110, 0b00110, 0b00000, 0b00110, 0b00110, 0b00000,
};
static const uint8_t GLYPH_0[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110,
};
static const uint8_t GLYPH_1[FONT5X7_HEIGHT] = {
    0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110,
};
static const uint8_t GLYPH_2[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111,
};
static const uint8_t GLYPH_3[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b00001, 0b00110, 0b00001, 0b10001, 0b01110,
};
static const uint8_t GLYPH_4[FONT5X7_HEIGHT] = {
    0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010,
};
static const uint8_t GLYPH_5[FONT5X7_HEIGHT] = {
    0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110,
};
static const uint8_t GLYPH_6[FONT5X7_HEIGHT] = {
    0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110,
};
static const uint8_t GLYPH_7[FONT5X7_HEIGHT] = {
    0b11111, 0b00001, 0b00010, 0b00100, 0b00100, 0b00100, 0b00100,
};
static const uint8_t GLYPH_8[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110,
};
static const uint8_t GLYPH_9[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100,
};
static const uint8_t GLYPH_A[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001,
};
static const uint8_t GLYPH_B[FONT5X7_HEIGHT] = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110,
};
static const uint8_t GLYPH_C[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110,
};
static const uint8_t GLYPH_D[FONT5X7_HEIGHT] = {
    0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110,
};
static const uint8_t GLYPH_E[FONT5X7_HEIGHT] = {
    0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111,
};
static const uint8_t GLYPH_F[FONT5X7_HEIGHT] = {
    0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000,
};
static const uint8_t GLYPH_G[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111,
};
static const uint8_t GLYPH_I[FONT5X7_HEIGHT] = {
    0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111,
};
static const uint8_t GLYPH_L[FONT5X7_HEIGHT] = {
    0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111,
};
static const uint8_t GLYPH_N[FONT5X7_HEIGHT] = {
    0b10001, 0b11001, 0b10101, 0b10101, 0b10011, 0b10001, 0b10001,
};
static const uint8_t GLYPH_O[FONT5X7_HEIGHT] = {
    0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110,
};
static const uint8_t GLYPH_R[FONT5X7_HEIGHT] = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001,
};
static const uint8_t GLYPH_S[FONT5X7_HEIGHT] = {
    0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110,
};
static const uint8_t GLYPH_V[FONT5X7_HEIGHT] = {
    0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100,
};
static const uint8_t GLYPH_W[FONT5X7_HEIGHT] = {
    0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001,
};
static const uint8_t GLYPH_P[FONT5X7_HEIGHT] = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000,
};
static const uint8_t GLYPH_H[FONT5X7_HEIGHT] = {
    0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001,
};
static const uint8_t GLYPH_T[FONT5X7_HEIGHT] = {
    0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100,
};
static const uint8_t GLYPH_Y[FONT5X7_HEIGHT] = {
    0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100,
};

const uint8_t *font5x7_glyph(char c)
{
    switch (c) {
        case '0': return GLYPH_0;
        case '1': return GLYPH_1;
        case '2': return GLYPH_2;
        case '3': return GLYPH_3;
        case '4': return GLYPH_4;
        case '5': return GLYPH_5;
        case '6': return GLYPH_6;
        case '7': return GLYPH_7;
        case '8': return GLYPH_8;
        case '9': return GLYPH_9;
        case 'A': return GLYPH_A;
        case 'B': return GLYPH_B;
        case 'C': return GLYPH_C;
        case 'D': return GLYPH_D;
        case 'E': return GLYPH_E;
        case 'F': return GLYPH_F;
        case 'G': return GLYPH_G;
        case 'H': return GLYPH_H;
        case 'I': return GLYPH_I;
        case 'L': return GLYPH_L;
        case 'N': return GLYPH_N;
        case 'O': return GLYPH_O;
        case 'P': return GLYPH_P;
        case 'R': return GLYPH_R;
        case 'S': return GLYPH_S;
        case 'T': return GLYPH_T;
        case 'V': return GLYPH_V;
        case 'W': return GLYPH_W;
        case 'Y': return GLYPH_Y;
        case '.': return GLYPH_DOT;
        case '-': return GLYPH_DASH;
        case ':': return GLYPH_COLON;
        case ' ':
        default: return GLYPH_SPACE;
    }
}
