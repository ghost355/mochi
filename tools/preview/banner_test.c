/*
 * banner_test — the eyes must never touch rows 0..6.
 *
 * The "РУКИ УБРАЛ!" banner lives on the top row and the eye engine clears its
 * dirty rectangle every frame. When that rectangle reached into the banner
 * band, the bottom row of every glyph disappeared: the text showed up and then
 * quietly got eaten.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "display/sh1106.h"
#include "eyes.h"
#include "font5x7.h"

static unsigned char fb[64][128];
void sh1106_set_pixel(int x, int y, bool on){ if(x<0||x>=128||y<0||y>=64) return; fb[y][x]=on?1:0; }

static int banner_pixels(void){ int n=0; for(int y=0;y<7;y++) for(int x=0;x<128;x++) if(fb[y][x]) n++; return n; }

int main(void){
    static const struct { const char *name; eyes_config_t c; } faces[] = {
      {"idle",   {.open_rest=1.0f,.gaze_amp=1.0f,.shape=EYE_SHAPE_STAR_SOLID,.blink_min_ms=2200,.blink_max_ms=6500,.pupil=PUPIL_BLOB,.brow_len=76,.brow_lift=3,.brow_tilt=0}},
      {"curious",{.open_rest=1.12f,.gaze_amp=1.6f,.shape=EYE_SHAPE_STAR_SOLID,.blink_min_ms=1500,.blink_max_ms=3000,.pupil=PUPIL_BLOB,.brow_len=62,.brow_lift=6,.brow_tilt=0}},
      {"angry",  {.open_rest=1.0f,.gaze_amp=0.3f,.shape=EYE_SHAPE_STAR_SOLID,.blink_min_ms=1100,.blink_max_ms=2400,.pupil=PUPIL_BLOB,.lid_pct=34,.lid_angle=28,.brow_len=84,.brow_lift=4,.brow_tilt=12,.brow_mono=1}},
      {"sleepy", {.open_rest=1.0f,.gaze_amp=0.5f,.shape=EYE_SHAPE_STAR_SOLID,.blink_min_ms=4000,.blink_max_ms=9000,.pupil=PUPIL_BLOB,.lid_pct=46,.lid_bottom_pct=18,.brow_len=70,.brow_lift=1}},
      // Panic scales the face past the screen edges; the top of the circles
      // and the brows both want rows that belong to the banner.
      {"panic",  {.open_rest=1.0f,.gaze_amp=2.4f,.shape=EYE_SHAPE_STAR_SOLID,.blink_min_ms=500,.blink_max_ms=1200,.pupil=PUPIL_DOT,.pupil_scale=40,.scale=132,.saccade_ms=200,.brow_len=70,.brow_lift=12,.brow_tilt=-3,.brow_thick=3}},
    };

    eyes_init(1);
    int worst = 0;
    int leaks = 0;
    uint32_t t = 0;
    unsigned char band[8][128];

    for (unsigned f = 0; f < sizeof(faces)/sizeof(faces[0]); f++) {
        for (int drift = 0; drift < 60; drift++) {
            eyes_set_config(&faces[f].c);

            // redraw the banner as main() does every time it is shown
            const char *txt = "РУКИ УБРАЛ!";
            int w = font_str_width_ex(txt, 1, 3); int x = (128 - w) / 2; if (x < 0) x = 0;
            font_draw_str_ex(x, 0, txt, 1, 3);
            int before = banner_pixels();

            // Snapshot the banner page: whatever the face does, rows 0..7 must
            // come out of eyes_draw() exactly as they went in.
            memcpy(band, fb, sizeof band);

            eyes_update(t);
            eyes_rect_t r = eyes_draw();

            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 128; x++)
                    if (fb[y][x] != band[y][x]) leaks++;

            // The eye rect must start on the page boundary: a rect that starts
            // inside page 0 makes the driver send the whole page, dragging any
            // stray eye ink above the face onto the panel.
            if (r.w > 0 && r.y < 8) {
                printf("face %u frame %d: eye rect starts at row %d, inside the banner page\n",
                       f, drift, r.y);
                return 1;
            }

            int after = banner_pixels();
            if (after < before && before - after > worst) worst = before - after;
            t += 20;
        }
    }
    printf("worst banner pixels lost to the eyes: %d\n", worst);
    printf("pixels written into the banner page: %d\n", leaks);
    return (worst == 0 && leaks == 0) ? 0 : 1;
}
