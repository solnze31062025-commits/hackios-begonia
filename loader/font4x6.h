/*
 * Minimal 4x6 block font. Each glyph: 6 rows x 4 columns,
 * packed as 3 bytes = 6 nibbles (row: MSB = leftmost column).
 * Bootstrap legibility only, not beauty.
 */
#ifndef LOADER_FONT4X6_H
#define LOADER_FONT4X6_H

#define FONT4X6_ROWS   6
#define FONT4X6_COLS   4
#define FONT4X6_BYTES  3

extern const unsigned char font4x6_glyphs[][FONT4X6_BYTES];

#endif /* LOADER_FONT4X6_H */
