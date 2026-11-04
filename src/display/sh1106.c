#include "display/sh1106.h"

#include <string.h>

#include "pico/stdlib.h"

// --- Driver state (single display, no malloc) ---

static i2c_inst_t *s_i2c;
static uint8_t     s_addr;                                   // 7-bit
static uint8_t     s_fb[SH1106_PAGES * SH1106_WIDTH];        // 8 * 128

// --- Low-level I2C helpers ---

// Send a command (or a short command+argument sequence) with control byte 0x00.
static void sh1106_cmd(const uint8_t *cmd, unsigned n)
{
    uint8_t buf[9];
    buf[0] = 0x00;                       // Co=0, D/C#=0 -> following bytes are commands
    memcpy(&buf[1], cmd, n);
    i2c_write_blocking(s_i2c, s_addr, buf, n + 1, false);
}

// --- Public API ---

void sh1106_init(i2c_inst_t *i2c, uint8_t addr)
{
    s_i2c  = i2c;
    s_addr = addr;

    memset(s_fb, 0, sizeof(s_fb));

    // Init sequence: U8g2-proven SH1106 "noname" sequence, in this exact order.
    // Each entry: { command, argument } with an explicit length — note that
    // 0xD3 has a legitimate 0x00 argument, so lengths cannot be inferred.
    // (0x40 with control byte 0x00 is the "start line 0" command, not data.)
    static const struct { uint8_t cmd, arg, len; } seq[] = {
        { 0xAE, 0x00, 1 }, // display off
        { 0xD5, 0x80, 2 }, // clock divide
        { 0xA8, 0x3F, 2 }, // multiplex = 64
        { 0xD3, 0x00, 2 }, // display offset = 0
        { 0x40, 0x00, 1 }, // start line = 0
        { 0xAD, 0x8B, 2 }, // charge pump: enable, high-level
        { 0x33, 0x00, 1 }, // VPP 9V
        { 0xA1, 0x00, 1 }, // segment remap
        { 0xC8, 0x00, 1 }, // COM scan direction remapped
        { 0xDA, 0x12, 2 }, // COM pins hardware config
        { 0x81, 0xCF, 2 }, // contrast
        { 0xD9, 0xF1, 2 }, // precharge period
        { 0xDB, 0x40, 2 }, // VCOMH deselect
        { 0xA4, 0x00, 1 }, // resume RAM display
        { 0xA6, 0x00, 1 }, // normal (not inverted)
        { 0x2E, 0x00, 1 }, // deactivate scroll
        { 0xAF, 0x00, 1 }, // display on
    };

    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        uint8_t buf[2] = { seq[i].cmd, seq[i].arg };
        sh1106_cmd(buf, seq[i].len);
    }

    // Let the charge pump settle before the first flush.
    sleep_ms(100);
}

void sh1106_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

uint8_t *sh1106_framebuffer(void)
{
    return s_fb;
}

void sh1106_set_pixel(int x, int y, bool on)
{
    if (!s_i2c) return;
    if (x < 0 || x >= SH1106_WIDTH)  return;
    if (y < 0 || y >= SH1106_HEIGHT) return;

    unsigned idx  = (unsigned)(y >> 3) * SH1106_WIDTH + (unsigned)x;
    uint8_t  mask = (uint8_t)(1u << (y & 7));   // bit0 = top row of the page
    if (on) s_fb[idx] |= mask;
    else    s_fb[idx] &= (uint8_t)~mask;
}

void sh1106_flush(void)
{
    sh1106_flush_region(0, 0, SH1106_WIDTH, SH1106_PAGES);
}

void sh1106_flush_region(int x, int page, int width, int pages)
{
    if (!s_i2c) return;

    // Clamp/validate the region against the 128x64 framebuffer.
    if (x < 0)          { width += x; x = 0; }
    if (page < 0)       { pages += page; page = 0; }
    if (width <= 0 || pages <= 0) return;
    if (x >= SH1106_WIDTH || page >= SH1106_PAGES) return;
    if (x + width  > SH1106_WIDTH)  width  = SH1106_WIDTH  - x;
    if (page + pages > SH1106_PAGES) pages = SH1106_PAGES - page;

    // 129 bytes: control byte + at most 128 pixels.
    static uint8_t buf[129];

    for (int p = page; p < page + pages; p++) {
        // Per-page addressing: set page + column (with the 2-column RAM bias)
        // in a single command transaction.
        int col = x + SH1106_COL_OFFSET;
        buf[0] = 0x00;                                  // control: commands
        buf[1] = (uint8_t)(0xB0 | (p & 0x0F));          // page address
        buf[2] = (uint8_t)(0x00 | (col & 0x0F));        // column low nibble
        buf[3] = (uint8_t)(0x10 | ((col >> 4) & 0x0F)); // column high nibble
        i2c_write_blocking(s_i2c, s_addr, buf, 4, false);

        // Then the pixel data for this page slice.
        buf[0] = 0x40;                                  // control: data
        memcpy(&buf[1], &s_fb[p * SH1106_WIDTH + x], (size_t)width);
        i2c_write_blocking(s_i2c, s_addr, buf, (size_t)width + 1, false);
    }
}
