/*
 * mochi — host preview renderer.
 *
 * Runs the real eye engine on the host, captures its output through a stub
 * sh1106_set_pixel, and writes a contact sheet of interesting states as a PPM
 * (convert with `ffmpeg -i preview.ppm preview.png`, or read it directly).
 *
 * No hardware, no flashing — for iterating on eye art.
 *
 *   make preview && ./preview preview.ppm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display/font5x7.h"
#include "display/sh1106.h"
#include "eyes.h"

// --- Framebuffer sink ------------------------------------------------------

static uint8_t fb[SH1106_HEIGHT][SH1106_WIDTH];

void sh1106_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SH1106_WIDTH || y < 0 || y >= SH1106_HEIGHT) return;
    fb[y][x] = on ? 1 : 0;
}

// --- Canvas ----------------------------------------------------------------

#define SCALE  4
#define PAD    8
#define BORDER 2

static uint8_t *g_canvas;
static int      g_W, g_H;

static void px(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= g_W || y < 0 || y >= g_H) return;
    uint8_t *p = g_canvas + ((size_t)y * g_W + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}

static void rect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            px(x, y, r, g, b);
}

// 3x5 digits, kept for the debug dump.
static const uint8_t digit3x5[10][5] = {
    { 0b111, 0b101, 0b101, 0b101, 0b111 },  // 0
    { 0b010, 0b110, 0b010, 0b010, 0b111 },  // 1
    { 0b111, 0b001, 0b111, 0b100, 0b111 },  // 2
    { 0b111, 0b001, 0b111, 0b001, 0b111 },  // 3
    { 0b101, 0b101, 0b111, 0b001, 0b001 },  // 4
    { 0b111, 0b100, 0b111, 0b001, 0b111 },  // 5
    { 0b111, 0b100, 0b111, 0b101, 0b111 },  // 6
    { 0b111, 0b001, 0b010, 0b010, 0b010 },  // 7
    { 0b111, 0b101, 0b111, 0b101, 0b111 },  // 8
    { 0b111, 0b101, 0b111, 0b001, 0b111 },  // 9
};

__attribute__((unused))
static void draw_number(int x, int y, int n, int scale)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", n);

    for (const char *c = buf; *c; c++) {
        int d = *c - '0';
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (digit3x5[d][row] & (1 << (2 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, 200, 200, 200);
        x += 4 * scale;
    }
}

// --- Shots -----------------------------------------------------------------

#define LABEL_SCALE 3
#define LABEL_H     (FONT_CHAR_H * LABEL_SCALE + 8)

#define CELL_W (SH1106_WIDTH * SCALE)
#define CELL_H (SH1106_HEIGHT * SCALE + LABEL_H)

typedef struct {
    const char   *label;
    eye_shape_t   shape;
    pupil_style_t pupil;
    float         open_rest;
    uint32_t      seed;
    uint32_t      t_ms;     // how long the engine runs before the capture
    bool          blink;    // force a blink at capture time
    float         glance_x, glance_y;
    bool          glance;
    int8_t        lid_pct;
    int8_t        lid_angle;
    int8_t        lid_bottom_pct;
    int8_t        brow_len;
    int8_t        brow_lift;
    int8_t        brow_tilt;
    int8_t        brow_thick;
    int8_t        brow_mono;
} shot_t;

static const shot_t k_shots[] = {
    // 0: idle — filled eyes, blob pupils, straight brows
    { .label = "IDLE", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .brow_len = 62, .brow_lift = 3, .brow_tilt = 0, .brow_thick = 3 },
    // 1: curious — wide eyes, brows up
    { .label = "CURIOUS", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.12f, .seed = 1, .t_ms = 300,
      .brow_len = 62, .brow_lift = 6, .brow_tilt = -1 },
    // 2: happy — brows level
    { .label = "HAPPY", .shape = EYE_SHAPE_HAPPY, .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .brow_len = 60, .brow_lift = 4, .brow_tilt = 0, .brow_thick = 3 },
    // 3: annoyed — tilted lids, brows pulled down
    { .label = "ANNOYED", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .lid_pct = 32, .lid_angle = 22, .brow_len = 64, .brow_lift = 3, .brow_tilt = 6, .brow_thick = 3 },
    // 4: angry — monobrow
    { .label = "ANGRY", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .lid_pct = 34, .lid_angle = 28, .brow_len = 72, .brow_lift = 4, .brow_tilt = 10, .brow_thick = 3, .brow_mono = 1 },
    // 5: pleading — brows raised and pinched at the bridge
    { .label = "PLEADING", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.10f, .seed = 1, .t_ms = 300,
      .brow_len = 60, .brow_lift = 2, .brow_tilt = -4, .brow_thick = 3 },
    // 5b: sleepy — half lid
    { .label = "SLEEPY", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .lid_pct = 48, .brow_len = 70, .brow_lift = 1, .brow_tilt = 2 },
    // 6: idle, mid blink
    { .label = "BLINK", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .blink = true, .brow_len = 62, .brow_lift = 3, .brow_tilt = 0, .brow_thick = 3 },
    // 7: idle, looking left
    { .label = "GLANCE", .shape = EYE_SHAPE_STAR_SOLID, .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .glance = true, .glance_x = -6.0f, .glance_y = 0.0f,
      .brow_len = 62, .brow_lift = 3, .brow_tilt = 0, .brow_thick = 3 },
    // 8: ring style, same face
    { .label = "RING", .shape = EYE_SHAPE_STAR_RING,  .pupil = PUPIL_BLOB, .open_rest = 1.00f, .seed = 1, .t_ms = 300,
      .brow_len = 62, .brow_lift = 3, .brow_tilt = 0, .brow_thick = 3 },
};

#define N_SHOTS ((int)(sizeof(k_shots) / sizeof(k_shots[0])))
#define COLS    5

static void render_cell(int index, int ox, int oy)
{
    const shot_t *s = &k_shots[index];
    eyes_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.open_rest   = s->open_rest;
    cfg.gaze_amp    = 1.0f;
    cfg.shape       = s->shape;
    cfg.blink_min_ms = 2200;
    cfg.blink_max_ms = 6500;
    cfg.pupil       = s->pupil;
    cfg.lid_pct     = s->lid_pct;
    cfg.lid_angle   = s->lid_angle;
    cfg.lid_bottom_pct = s->lid_bottom_pct;
    cfg.brow_len    = s->brow_len;
    cfg.brow_lift   = s->brow_lift;
    cfg.brow_tilt   = s->brow_tilt;
    cfg.brow_thick  = s->brow_thick ? s->brow_thick : 3;
    cfg.brow_mono   = s->brow_mono;

    memset(fb, 0, sizeof(fb));
    eyes_init(s->seed);
    eyes_set_config(&cfg);

    for (uint32_t t = 0; t <= s->t_ms; t += 20) {
        eyes_update(t);
        eyes_draw();
    }

    if (s->glance) {
        eyes_glance(s->glance_x, s->glance_y);
        for (int i = 0; i < 20; i++) {          // let the gaze settle
            eyes_update(s->t_ms + (uint32_t)(i * 20ull));
            eyes_draw();
        }
    }

    if (s->blink) {
        eyes_blink_now();
        for (int i = 1; i <= 3; i++) {
            eyes_update(s->t_ms + (uint32_t)(i * 20ull));
            eyes_draw();
        }
    }

    int eye_h = SH1106_HEIGHT * SCALE;

    rect(ox, oy, CELL_W, CELL_H, 0, 0, 0);
    for (int y = 0; y < eye_h; y++) {
        for (int x = 0; x < CELL_W; x++) {
            bool on = fb[y / SCALE][x / SCALE] != 0;
            // A little blue push so it reads as an OLED rather than paper.
            if (on) px(ox + x, oy + y, 190, 220, 255);
            else    px(ox + x, oy + y, 6, 8, 14);
        }
    }

    // Caption band. The font draws through the same pixel sink as the eyes, so
    // it is rendered into a cleared buffer and then blitted below the cell.
    if (s->label) {
        memset(fb, 0, sizeof(fb));
        int tw = font_str_width(s->label, 1);
        font_draw_str((SH1106_WIDTH - tw) / 2, 0, s->label, 1);

        int base = oy + eye_h + 4;
        for (int y = 0; y < FONT_CHAR_H; y++)
            for (int x = 0; x < SH1106_WIDTH; x++)
                if (fb[y][x])
                    rect(ox + x * LABEL_SCALE, base + y * LABEL_SCALE,
                         LABEL_SCALE, LABEL_SCALE, 210, 210, 210);
    }

    (void)index;
    for (int i = 0; i < BORDER; i++)
        ;
}

// --- Animation mode --------------------------------------------------------
//
// Writes a numbered sequence of 1:1 frames so ffmpeg can turn it into a GIF:
//   ./preview --anim frames 2 1
//   ffmpeg -i frames/f%03d.ppm -vf palettegen=stats_mode=single pal.png
static void anim(const char *dir, eye_shape_t shape, pupil_style_t pupil)
{
    eyes_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.open_rest    = 1.0f;
    cfg.gaze_amp     = 1.0f;
    cfg.shape        = shape;
    cfg.blink_min_ms = 2200;
    cfg.blink_max_ms = 6500;
    cfg.pupil        = pupil;
    cfg.brow_len     = 76;
    cfg.brow_lift    = 3;
    cfg.brow_tilt    = 0;
    cfg.brow_thick   = 3;

    eyes_init(3);
    eyes_set_config(&cfg);

    const int frames = 40;          // 2 s at 20 fps
    for (int f = 0; f < frames; f++) {
        uint32_t t = (uint32_t)f * 50;

        if (f == 8)  eyes_blink_now();
        if (f == 26) eyes_glance(-6.0f, 1.0f);

        eyes_update(t);
        eyes_draw();

        char path[512];
        snprintf(path, sizeof(path), "%s/f%03d.ppm", dir, f);

        FILE *fp = fopen(path, "wb");
        if (!fp) { perror(path); return; }
        fprintf(fp, "P6\n%d %d\n255\n", SH1106_WIDTH, SH1106_HEIGHT);
        for (int y = 0; y < SH1106_HEIGHT; y++)
            for (int x = 0; x < SH1106_WIDTH; x++) {
                bool on = fb[y][x] != 0;
                fputc(on ? 190 : 6, fp);
                fputc(on ? 220 : 8, fp);
                fputc(on ? 255 : 14, fp);
            }
        fclose(fp);
    }

    printf("wrote %d frames to %s\n", frames, dir);
}

int main(int argc, char **argv)
{
    if (argc >= 5 && strcmp(argv[1], "--anim") == 0) {
        anim(argv[2], (eye_shape_t)atoi(argv[3]), (pupil_style_t)atoi(argv[4]));
        return 0;
    }

    const char *path = (argc > 1) ? argv[1] : "preview.ppm";

    int rows = (N_SHOTS + COLS - 1) / COLS;
    g_W = COLS * (CELL_W + PAD) + PAD;
    g_H = rows * (CELL_H + PAD) + PAD;

    g_canvas = calloc((size_t)g_W * g_H * 3, 1);
    if (!g_canvas) { perror("calloc"); return 1; }
    memset(g_canvas, 18, (size_t)g_W * g_H * 3);

    for (int i = 0; i < N_SHOTS; i++) {
        int col = i % COLS, row = i / COLS;
        render_cell(i, PAD + col * (CELL_W + PAD), PAD + row * (CELL_H + PAD));
    }

    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", g_W, g_H);
    fwrite(g_canvas, 1, (size_t)g_W * g_H * 3, f);
    fclose(f);

    printf("wrote %s (%dx%d), %d cells of %dx%d\n", path, g_W, g_H, N_SHOTS, CELL_W, CELL_H);
    for (int i = 0; i < N_SHOTS; i++)
        printf("  %d: shape=%d pupil=%d open=%.2f t=%ums\n",
               i, k_shots[i].shape, k_shots[i].pupil,
               k_shots[i].open_rest, k_shots[i].t_ms);
    return 0;
}
