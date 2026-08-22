#ifndef _FONT_H
#define _FONT_H

#include <stdint.h>

#define FONT_WIDTH       8
#define FONT_HEIGHT      8
#define FONT_FIRST_CHAR  0x20
#define FONT_LAST_CHAR   0x7E
#define FONT_GLYPH_COUNT (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)

extern const uint8_t font8x8[FONT_GLYPH_COUNT][FONT_HEIGHT];

const uint8_t *font_glyph(char c);

#endif
