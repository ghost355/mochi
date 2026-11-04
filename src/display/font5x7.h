/*
 * 5x7 bitmap font with Cyrillic.
 *
 * Glyphs are 5 px wide, 7 px tall, 6 px advance. Everything is drawn through
 * sh1106_set_pixel(), so the caller owns the framebuffer and flushing.
 *
 * Cyrillic is uppercase-only: lowercase input (U+0430..U+044F, U+0451) maps
 * onto the uppercase glyphs because lowercase Cyrillic is unreadable in a
 * 5x7 cell. UTF-8 input is decoded; anything unmappable renders as '?'.
 */

#ifndef MOCHI_DISPLAY_FONT5X7_H
#define MOCHI_DISPLAY_FONT5X7_H

#define FONT_CHAR_W   5
#define FONT_CHAR_H   7
#define FONT_ADVANCE  6

void font_draw_char(int x, int y, char c, int scale);

// UTF-8 aware. `scale` 1 or 2 (higher works too, just check it fits).
void font_draw_str(int x, int y, const char *s, int scale);

// Same, but with `spacing` extra pixels between glyphs — used for banner text
// that has to stretch across the full 128 px width.
void font_draw_str_ex(int x, int y, const char *s, int scale, int spacing);
int  font_str_width_ex(const char *s, int scale, int spacing);

// Width in pixels of the string (UTF-8 aware), advance included.
int font_str_width(const char *s, int scale);

// Diagnostic: the 33 Cyrillic glyphs in table order.
const char *font5x7_cyr_order(void);

#endif // MOCHI_DISPLAY_FONT5X7_H
