#ifndef FONT8X16_H
#define FONT8X16_H

/* 95 printable ASCII glyphs (32..126), 8 pixels wide, 16 rows tall.
 * Each row is one byte, bit 7 = leftmost pixel. See font8x16.c for provenance. */
extern const unsigned char font8x16[95][16];

#define FONT_W 8
#define FONT_H 16
#define FONT_FIRST 32
#define FONT_LAST  126

#endif
