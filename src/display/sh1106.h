/*
 * Minimal SH1106 OLED driver (128x64, I2C).
 *
 * Pure C11, no malloc, no dependencies beyond pico-sdk.
 *
 * SH1106 quirks (vs SSD1306):
 *  - The controller RAM is 132 columns wide; the visible 128 columns start at
 *    RAM column 2, so every column address must be biased by SH1106_COL_OFFSET.
 *  - There is no horizontal addressing mode: data is written page by page,
 *    and each page write must be preceded by setting page + column address.
 */

#ifndef MOCHI_DISPLAY_SH1106_H
#define MOCHI_DISPLAY_SH1106_H

#include <stdbool.h>
#include <stdint.h>

#include "pico/types.h"
#include "hardware/i2c.h"

#define SH1106_WIDTH      128
#define SH1106_HEIGHT     64
#define SH1106_PAGES      (SH1106_HEIGHT / 8)
#define SH1106_COL_OFFSET 2   // visible columns start at RAM column 2

// Configure the driver (does not touch the bus until the first flush).
void sh1106_init(i2c_inst_t *i2c, uint8_t addr); // addr = 0x3C (7-bit)

void sh1106_clear(void);

// 8*128 bytes, page-major, bit0 = top row of the page.
uint8_t *sh1106_framebuffer(void);

void sh1106_set_pixel(int x, int y, bool on);

// Push the whole framebuffer to the display.
void sh1106_flush(void);

// Partial update: `width` pixels starting at column `x`, `pages` pages
// starting at page `page`. Out-of-range input is clamped/ignored.
void sh1106_flush_region(int x, int page, int width, int pages);

#endif // MOCHI_DISPLAY_SH1106_H
