/*
 * mochi — M1: boot screen, then the eye engine.
 *
 * Sequence:
 *   1. bring up I2C0 on GP4/GP5 and scan the bus
 *   2. init the SH1106
 *   3. show the boot screen for two seconds (proves display + font + Cyrillic)
 *   4. hand the screen over to the procedural eye engine
 *
 * The frame loop runs at 30 fps and pushes only the dirty region reported by
 * the eye engine, so a blink costs one or two pages of I2C traffic instead of
 * a full 1 KB frame.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/i2c.h"

#include "board.h"
#include "buzzer.h"
#include "display/font5x7.h"
#include "display/sh1106.h"
#include "eyes.h"
#include "mood.h"
#include "touch.h"

#define FPS_TARGET   50
#define BOOT_SCREEN_MS 2000

// "РУКИ УБРАЛ!" banner. It sits on the top row, above the brows: the face owns
// the middle of the screen and a bottom banner kept clipping the chin of the
// eyes during a squint.
#define WARN_TEXT      "РУКИ УБРАЛ!"
#define WARN_Y         0
#define WARN_SPACING   3
#define WARN_AFTER_MS  2000
#define WARN_PAGE      0            // rows 0..7
#define WARN_LAST_ROW  6            // 5x7 font at y=0 ends here

// --- Boot screen ----------------------------------------------------------

static void draw_border(void)
{
    for (int x = 0; x < SH1106_WIDTH; x++) {
        sh1106_set_pixel(x, 0, true);
        sh1106_set_pixel(x, SH1106_HEIGHT - 1, true);
    }
    for (int y = 0; y < SH1106_HEIGHT; y++) {
        sh1106_set_pixel(0, y, true);
        sh1106_set_pixel(SH1106_WIDTH - 1, y, true);
    }
}

static void draw_centered(int y, const char *s, int scale)
{
    int x = (SH1106_WIDTH - font_str_width(s, scale)) / 2;
    if (x < 0) x = 0;
    font_draw_str(x, y, s, scale);
}

static int i2c_scan(i2c_inst_t *i2c)
{
    int found = -1;

    for (int addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        // A 1-byte read is the cheapest reliable probe: the SDK refuses
        // zero-length transfers outright, so this is what actually drives the
        // address phase and reports a NACK.
        if (i2c_read_blocking(i2c, (uint8_t)addr, &dummy, 1, false) >= 0) {
            printf("I2C found 0x%02X\n", addr);
            if (found < 0) found = addr;
        }
    }

    if (found < 0) printf("I2C: no devices\n");
    return found;
}

static void draw_boot_screen(int addr)
{
    sh1106_clear();
    draw_border();

    draw_centered(4, "MOCHI", 2);
    draw_centered(23, "RP2350", 1);

    char buf[16];
    if (addr > 0) snprintf(buf, sizeof(buf), "I2C %02X", addr);
    else          snprintf(buf, sizeof(buf), "I2C --");
    draw_centered(33, buf, 1);

    draw_centered(44, "ПРИВЕТ", 1);

    sh1106_flush();
}

// --- Eye style ------------------------------------------------------------

// The moods describe expression (openness, blink rate, gaze). The build-time
// style decides the silhouette: capsule bars, or round eyes with a star pupil.
static void apply_eye_style(eyes_config_t *cfg)
{
#if EYES_STYLE == 1
    cfg->shape = EYE_SHAPE_STAR_RING;
#elif EYES_STYLE == 2
    cfg->shape = EYE_SHAPE_STAR_SOLID;
#else
    (void)cfg;
    return;
#endif

#if   EYES_PUPIL == 1
    cfg->pupil = PUPIL_ASTERISK;
#elif EYES_PUPIL == 2
    cfg->pupil = PUPIL_SCRIBBLE;
#elif EYES_PUPIL == 3
    cfg->pupil = PUPIL_DOT;
#elif EYES_PUPIL == 4
    cfg->pupil = PUPIL_BLOB;
#else
    cfg->pupil = PUPIL_ASTROID;
#endif
}

// --- Hold warning ---------------------------------------------------------
// Draw the banner into the framebuffer. Kept separate from the flush so the
// caller can decide whether the band actually has to go over I2C.
static void warn_render(void)
{
    int w = font_str_width_ex(WARN_TEXT, 1, WARN_SPACING);
    int x = (SH1106_WIDTH - w) / 2;
    if (x < 0) x = 0;
    font_draw_str_ex(x, WARN_Y, WARN_TEXT, 1, WARN_SPACING);
}

static void warn_erase(void)
{
    // Only the banner band: erasing to the bottom of the panel would wipe the
    // eyes, which share page 0 by one row once the brows are up.
    for (int y = WARN_Y; y <= WARN_LAST_ROW; y++)
        for (int x = 0; x < SH1106_WIDTH; x++)
            sh1106_set_pixel(x, y, false);
}

static void warn_flush(void)
{
    sh1106_flush_region(0, WARN_PAGE, SH1106_WIDTH, 1);
}

// --- Main -----------------------------------------------------------------

int main(void)
{
    stdio_init_all();
    sleep_ms(1500); // let USB CDC enumerate before the first printf

    printf("\nboot: mochi M1\n");

    i2c_init(i2c0, I2C_BAUD);
    gpio_set_function(PIN_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C_SDA);
    gpio_pull_up(PIN_I2C_SCL);

    int addr = i2c_scan(i2c0);
    if (addr < 0) addr = OLED_I2C_ADDR;
    printf("display: SH1106 @ 0x%02X\n", addr);

    sh1106_init(i2c0, (uint8_t)addr);
    draw_boot_screen(addr);

    sleep_ms(BOOT_SCREEN_MS);

    sh1106_clear();
    sh1106_flush();

    eyes_init((uint32_t)to_ms_since_boot(get_absolute_time()));
    touch_init();
    buzzer_init();
    mood_init((uint32_t)to_ms_since_boot(get_absolute_time()));
    {
        eyes_config_t cfg = mood_params(MOOD_IDLE)->eyes;
        apply_eye_style(&cfg);
        eyes_set_config(&cfg);
    }
    buzzer_play(&buzzer_startup);

    const uint32_t frame_us = 1000000 / FPS_TARGET;
    absolute_time_t next = get_absolute_time();
    absolute_time_t fps_mark = next;

    int frames = 0;
    int updates = 0;
    bool s_squinting = false;
    bool s_grabbed   = false;
    bool warn_shown = false;
    uint32_t last_held = 0;
    mood_t prev_mood = MOOD_IDLE;

    while (true) {
        next = delayed_by_us(next, frame_us);
        sleep_until(next);

        uint32_t now = (uint32_t)to_ms_since_boot(get_absolute_time());

        // Touch feeds the mood machine; the mood decides how the face looks.
        touch_state_t ts = touch_update(now);
        if (ts.pressed) last_held = ts.held_ms;
        if (ts.just_pressed) {
            eyes_react_tap();
            buzzer_play(&buzzer_tap);
            mood_tap(now);
            printf("touch: tap\n");
        }
        if (ts.pressed && ts.held_ms >= 1500 && !s_grabbed) {
            s_grabbed = true;
            mood_hold_long(now);
            printf("touch: grab %.1fs\n", (double)ts.held_ms / 1000.0);
        }
        if (ts.pressed && ts.held_ms >= 700 && !s_squinting) {
            s_squinting = true;
            eyes_set_squint(true);
            buzzer_tone(196, 600000);  // low drone until release
            mood_hold(now);
            printf("touch: hold\n");
        }
        if (ts.just_released) {
            if (s_squinting) { s_squinting = false; eyes_set_squint(false); }
            s_grabbed = false;
            eyes_blink_now();
            buzzer_stop();
            buzzer_play(&buzzer_release);
            mood_release(now);
            printf("touch: release %lums\n", (unsigned long)last_held);
        }

        mood_t m;
        if (mood_update(now, &m)) {
            eyes_config_t cfg = mood_params(m)->eyes;
            apply_eye_style(&cfg);
            eyes_set_config(&cfg);
            printf("mood: %s\n", mood_params(m)->name);

            if (prev_mood == MOOD_SLEEPY && m != MOOD_SLEEPY) {
                eyes_yawn();
                buzzer_play(&buzzer_yawn);
                printf("wake: yawn\n");
            } else if (m == MOOD_SLEEPY) {
                buzzer_play(&buzzer_sleep);
            }
            prev_mood = m;
        }

        buzzer_update(now);
        eyes_update(now);
        eyes_rect_t r = eyes_draw();

        if (r.w > 0 && r.h > 0) {
            int p0 = r.y / 8;
            int p1 = (r.y + r.h - 1) / 8;
            // The eyes' rect starts below the banner, and the banner lives in
            // the framebuffer, so any page-0 flush already carries it. Nothing
            // to repaint here.
            sh1106_flush_region(r.x, p0, r.w, p1 - p0 + 1);
            updates++;
        }

        // Held long enough: put the nag on screen. It is painted after the eyes
        // so it always wins, and erased the moment the hand comes off.
        bool warn = ts.pressed && ts.held_ms >= WARN_AFTER_MS;
        if (warn != warn_shown) {
            warn_shown = warn;
            if (warn) {
                warn_render();
            } else {
                warn_erase();
            }
            warn_flush();
            printf("warn: %s\n", warn ? "on" : "off");
        }

        frames++;
        absolute_time_t t = get_absolute_time();
        if (absolute_time_diff_us(fps_mark, t) >= 1000000) {
            printf("fps=%d upd=%d\n", frames, updates);
            frames = 0;
            updates = 0;
            fps_mark = t;
        }
    }
}
