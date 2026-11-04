/*
 * mirror_test — verifies that the dirty-rect flush actually converges.
 *
 * The eye engine only sends the rectangle it reports from eyes_draw(), so
 * anything drawn outside that rectangle reaches the framebuffer but never the
 * panel. On hardware that shows up as elements that appear once and then stay
 * behind, or that double when they move: exactly what a stale brow looks like.
 *
 * This keeps a second framebuffer standing in for the panel and copies into it
 * only the reported rect, then compares the two. Any mismatch is a region the
 * driver would have left stale.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "display/sh1106.h"
#include "eyes.h"

static unsigned char fb[SH1106_HEIGHT][SH1106_WIDTH];
static unsigned char panel[SH1106_HEIGHT][SH1106_WIDTH];

void sh1106_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SH1106_WIDTH || y < 0 || y >= SH1106_HEIGHT) return;
    fb[y][x] = on ? 1 : 0;
}

static void flush(eyes_rect_t r)
{
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            if (x >= 0 && x < SH1106_WIDTH && y >= 0 && y < SH1106_HEIGHT)
                panel[y][x] = fb[y][x];
}

static int mismatches(void)
{
    int bad = 0;
    for (int y = 0; y < SH1106_HEIGHT; y++)
        for (int x = 0; x < SH1106_WIDTH; x++)
            if (panel[y][x] != fb[y][x]) bad++;
    return bad;
}

static const eyes_config_t k_face[3] = {
    // idle: brows 3 px above the eye
    { .open_rest = 1.00f, .gaze_amp = 1.00f, .shape = EYE_SHAPE_STAR_SOLID,
      .blink_min_ms = 2200, .blink_max_ms = 6500, .pupil = PUPIL_BLOB,
      .brow_len = 76, .brow_lift = 3, .brow_tilt = 0 },
    // surprised: brows 6 px up, same eyes
    { .open_rest = 1.00f, .gaze_amp = 1.00f, .shape = EYE_SHAPE_STAR_SOLID,
      .blink_min_ms = 2200, .blink_max_ms = 6500, .pupil = PUPIL_BLOB,
      .brow_len = 62, .brow_lift = 6, .brow_tilt = 0 },
    // angry: monobrow sitting right on the eye
    { .open_rest = 1.00f, .gaze_amp = 1.00f, .shape = EYE_SHAPE_STAR_SOLID,
      .blink_min_ms = 1100, .blink_max_ms = 2400, .pupil = PUPIL_BLOB,
      .lid_pct = 26, .lid_angle = 18,
      .brow_len = 84, .brow_lift = 0, .brow_tilt = 2, .brow_mono = 1 },
};

int main(void)
{
    memset(fb, 0, sizeof(fb));
    memset(panel, 0, sizeof(panel));

    eyes_init(1);
    eyes_set_config(&k_face[0]);

    uint32_t t = 0;
    int worst = 0;

    // Run each face for a while, changing face every 40 frames, and watch how
    // far the simulated panel drifts from the framebuffer.
    for (int step = 0; step < 240; step++) {
        if (step % 40 == 0) eyes_set_config(&k_face[(step / 40) % 3]);

        eyes_update(t);
        flush(eyes_draw());

        int bad = mismatches();
        if (bad > worst) worst = bad;
        t += 20;
    }

    printf("worst mismatch: %d pixels\n", worst);
    return worst == 0 ? 0 : 1;
}
