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
#include "focus.h"
#include "mood.h"
#include "touch.h"

#define FPS_TARGET   50

// The pomodoro bar lives in the last page, under the eyes: the face stops at
// row 58, so three rows are free without touching the eyes' dirty rect.
#define FOCUS_BAR_TOP  61
#define FOCUS_BAR_ROW  63

// "РУКИ УБРАЛ!" banner. It sits on the top row, above the brows: the face owns
// the middle of the screen and a bottom banner kept clipping the chin of the
// eyes during a squint.
#define WARN_TEXT      "РУКИ УБРАЛ!"
#define WARN_Y         0
#define WARN_SPACING   3
#define WARN_AFTER_MS  2000
#define WARN_PAGE      0            // rows 0..7
#define WARN_LAST_ROW  6            // 5x7 font at y=0 ends here

// Three taps plus a held third one — the pomodoro gesture. It has to outrun the
// squint (700 ms) and the grab (1500 ms), otherwise the timer would arrive with
// the drone and the angry face still attached.
#define FOCUS_HOLD_MS  500

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

    // A mood that asked for a specific pupil keeps it: panic shrinks the eye
    // to a pinprick and needs a dot, not the build-time default.
    if (cfg->pupil_scale > 0) return;

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

// --- Focus bar ------------------------------------------------------------

static int s_focus_px = -1;      // filled width currently on the panel

static void focus_bar_paint(int permille)
{
    int w = (SH1106_WIDTH * permille) / 1000;

    // A one-pixel baseline across the whole width, so an empty bar still reads
    // as a timer rather than as nothing at all.
    for (int x = 0; x < SH1106_WIDTH; x++) sh1106_set_pixel(x, FOCUS_BAR_ROW, true);
    for (int y = FOCUS_BAR_TOP; y <= FOCUS_BAR_ROW; y++)
        for (int x = 0; x < w; x++) sh1106_set_pixel(x, y, true);
}

static void focus_bar_clear(void)
{
    for (int y = FOCUS_BAR_TOP; y <= FOCUS_BAR_ROW; y++)
        for (int x = 0; x < SH1106_WIDTH; x++) sh1106_set_pixel(x, y, false);
}

// Push only the columns that changed. The bar grows a pixel every twelve
// seconds, so the usual transfer is one byte wide; the full-width flush happens
// when it appears or disappears.
static void focus_bar_flush(int from_px, int to_px)
{
    if (to_px < from_px) { int t = from_px; from_px = to_px; to_px = t; }
    if (from_px < 0) from_px = 0;
    if (to_px > SH1106_WIDTH) to_px = SH1106_WIDTH;
    if (to_px <= from_px) return;

    sh1106_flush_region(from_px, FOCUS_BAR_ROW / 8, to_px - from_px, 1);
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
    sh1106_clear();
    sh1106_flush();

    uint32_t boot_at = (uint32_t)to_ms_since_boot(get_absolute_time());
    eyes_init(boot_at);
    touch_init();
    buzzer_init();
    mood_init(boot_at);
    {
        eyes_config_t cfg = mood_params(MOOD_IDLE)->eyes;
        apply_eye_style(&cfg);
        eyes_set_config(&cfg);
    }

    // Power-on: the face arrives asleep, opens its eyes and focuses. The
    // arpeggio starts on the same clock, so it lands as the lids come up.
    eyes_boot(boot_at);
    buzzer_play(&buzzer_startup);

    const uint32_t frame_us = 1000000 / FPS_TARGET;
    absolute_time_t next = get_absolute_time();
    absolute_time_t fps_mark = next;

    int frames = 0;
    int updates = 0;
    bool s_squinting = false;
    bool s_grabbed   = false;
    bool s_focus_arm = false;       // the last tap was the third of a series
    bool s_focus_gesture = false;   // ...and it is being held: this is the timer
    bool warn_shown = false;
    uint32_t last_held = 0;
    mood_t prev_mood = MOOD_IDLE;

    while (true) {
        next = delayed_by_us(next, frame_us);
        sleep_until(next);

        uint32_t now = (uint32_t)to_ms_since_boot(get_absolute_time());

        // Touch feeds the mood machine; the mood decides how the face looks.
        // During the power-on the robot is not listening yet.
        touch_state_t ts = touch_update(now);
        if (eyes_booting()) {
            ts.pressed = false;
            ts.just_pressed = false;
            ts.just_released = false;
        }
        if (ts.pressed) last_held = ts.held_ms;
        if (ts.just_pressed) {
            eyes_react_tap();
            buzzer_chirp(now);
            int taps = mood_tap(now);
            printf("touch: tap (%d)\n", taps);

            // Arm the pomodoro gesture: three taps are just attention, but
            // keeping the third one down starts or cancels the timer. Fast
            // four-to-six tap series never arm it, because only the third tap
            // of the window sets this flag.
            s_focus_arm = (taps == 3);
        }

        if (ts.pressed && s_focus_arm && !s_focus_gesture &&
            ts.held_ms >= FOCUS_HOLD_MS) {
            s_focus_gesture = true;

            if (focus_running()) {
                focus_stop();
                mood_focus_leave(now);
                focus_bar_clear();
                focus_bar_flush(0, SH1106_WIDTH);
                s_focus_px = -1;
                buzzer_play(&buzzer_sad);
                printf("focus: cancelled\n");
            } else {
                focus_start(now);
                mood_focus_enter(now);
                buzzer_play(&buzzer_startup);
                printf("focus: start 25 min\n");
            }
        }

        if (!s_focus_gesture && ts.pressed && ts.held_ms >= 1500 && !s_grabbed) {
            s_grabbed = true;
            mood_hold_long(now);
            printf("touch: grab %.1fs\n", (double)ts.held_ms / 1000.0);
        }
        if (!s_focus_gesture && ts.pressed && ts.held_ms >= 700 && !s_squinting) {
            s_squinting = true;
            eyes_set_squint(true);
            buzzer_tone(196, 600000);  // low drone until release
            mood_hold(now);
            printf("touch: hold\n");
        }
        if (ts.just_released) {
            s_focus_arm = false;
            s_focus_gesture = false;
            if (s_squinting) { s_squinting = false; eyes_set_squint(false); }
            s_grabbed = false;
            eyes_blink_now();
            buzzer_stop();
            buzzer_play(&buzzer_release);
            mood_release(now);
            printf("touch: release %lums\n", (unsigned long)last_held);
        }

        mood_t m;
        bool mood_changed = mood_update(now, &m);

        if (focus_running()) {
            mood_keep_awake(now);

            // A tap pulls the face away for a moment; bring the working face
            // back as soon as the short mood expires.
            if (mood_changed && m == MOOD_IDLE) {
                mood_focus_enter(now);
                mood_changed = true;
                m = MOOD_FOCUS;
            }
        }

        if (focus_take_finished(now)) {
            focus_bar_clear();
            focus_bar_flush(0, SH1106_WIDTH);
            s_focus_px = -1;

            mood_celebrate(now);
            buzzer_play(&buzzer_startup);
            printf("focus: done\n");
            mood_changed = true;
            m = MOOD_HAPPY;
        }

        if (mood_changed) {
            eyes_config_t cfg = mood_params(m)->eyes;
            apply_eye_style(&cfg);
            eyes_set_config(&cfg);
            printf("mood: %s\n", mood_params(m)->name);

            if (prev_mood == MOOD_SLEEPY && m != MOOD_SLEEPY) {
                eyes_yawn();
                buzzer_play(&buzzer_yawn);
                printf("wake: yawn\n");
            } else if (m == MOOD_SLEEPY) {
                // A belch, then the goodnight phrase over the closing lids.
                buzzer_burp_then(now, &buzzer_sleep);
                printf("sound: burp + goodnight\n");
            }
            prev_mood = m;
        }

        buzzer_update(now);
        eyes_update(now);
        eyes_rect_t r = eyes_draw();

        // The pomodoro bar shares page 7 with the chin of the eyes, so it goes
        // into the framebuffer before the eye flush and rides along with it.
        if (focus_running()) {
            int perm = focus_progress_permille(now);
            focus_bar_paint(perm);

            int px = (SH1106_WIDTH * perm) / 1000;
            if (px != s_focus_px) {
                if (s_focus_px < 0) focus_bar_flush(0, SH1106_WIDTH);
                else                focus_bar_flush(s_focus_px, px + 1);
                s_focus_px = px;
            }
        } else if (s_focus_px >= 0) {
            focus_bar_clear();
            focus_bar_flush(0, SH1106_WIDTH);
            s_focus_px = -1;
        }

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
        bool warn = ts.pressed && ts.held_ms >= WARN_AFTER_MS && !s_focus_gesture;
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
