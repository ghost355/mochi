/*
 * mochi — mood state machine (see mood.h).
 */

#include "mood.h"

#include "buzzer.h"

// --- Parameters -----------------------------------------------------------

const mood_params_t k_moods[MOOD_COUNT] = {
    [MOOD_IDLE] = {
        .name = "idle",
        .eyes = { .open_rest = 1.00f, .gaze_amp = 1.00f,
                  .shape = EYE_SHAPE_CAPSULE, .blink_min_ms = 2200, .blink_max_ms = 6500,
                  .brow_len = 62, .brow_lift = 3, .brow_tilt = 0, .brow_thick = 3 },
        .duration_ms = 0,
    },
    [MOOD_CURIOUS] = {
        .name = "curious",
        .eyes = { .open_rest = 1.12f, .gaze_amp = 1.60f,
                  .shape = EYE_SHAPE_CAPSULE, .blink_min_ms = 1500, .blink_max_ms = 3000,
                  .brow_len = 52, .brow_lift = 6, .brow_tilt = 0, .brow_thick = 3 },
        .duration_ms = 2500,
    },
    [MOOD_HAPPY] = {
        .name = "happy",
        .eyes = { .open_rest = 1.00f, .gaze_amp = 1.20f,
                  .shape = EYE_SHAPE_HAPPY, .blink_min_ms = 1200, .blink_max_ms = 2600,
                  .brow_len = 60, .brow_lift = 4, .brow_tilt = 0, .brow_thick = 3 },
        .duration_ms = 4000,
    },
    [MOOD_PLEADING] = {
        .name = "pleading",
        // Brows raised and pinched at the bridge: the inner ends sit five
        // pixels above the outer ones, which is the whole expression.
        .eyes = { .open_rest = 1.10f, .gaze_amp = 0.80f,
                  .shape = EYE_SHAPE_CAPSULE, .blink_min_ms = 1600, .blink_max_ms = 3200,
                  .brow_len = 60, .brow_lift = 2, .brow_tilt = -4, .brow_thick = 3 },
        .duration_ms = 4000,
    },
    [MOOD_ANNOYED] = {
        .name = "annoyed",
        .eyes = { .open_rest = 1.00f, .gaze_amp = 0.40f,
                  .shape = EYE_SHAPE_CAPSULE, .blink_min_ms = 900, .blink_max_ms = 1800,
                  .lid_pct = 32, .lid_angle = 22,
                  .brow_len = 64, .brow_lift = 3, .brow_tilt = 6, .brow_thick = 3 },
        .duration_ms = 3000,
    },
    [MOOD_ANGRY] = {
        .name = "angry",
        .eyes = { .open_rest = 1.00f, .gaze_amp = 0.30f,
                  .shape = EYE_SHAPE_CAPSULE, .blink_min_ms = 1100, .blink_max_ms = 2400,
                  .lid_pct = 34, .lid_angle = 28,
                  .brow_len = 72, .brow_lift = 4, .brow_tilt = 10, .brow_thick = 3, .brow_mono = 1 },
        .duration_ms = 3500,
    },
    [MOOD_SLEEPY] = {
        .name = "sleepy",
        .eyes = { .open_rest = 1.00f, .gaze_amp = 0.50f,
                  .shape = EYE_SHAPE_CAPSULE, .blink_min_ms = 4000, .blink_max_ms = 9000,
                  .lid_pct = 46, .lid_bottom_pct = 18,
                  .brow_len = 58, .brow_lift = 1, .brow_tilt = 0, .brow_thick = 3 },
        .duration_ms = 0,
    },
};

// --- State ----------------------------------------------------------------

static mood_t   s_mood;
static uint32_t s_mood_since;
static uint32_t s_last_touch;
static uint32_t s_displayed;      // mood reported by the previous update

#define TAP_RING 8
static uint32_t s_taps[TAP_RING];
static uint8_t  s_tap_head;
static uint8_t  s_tap_count;

static void enter(mood_t m, uint32_t now)
{
    s_mood = m;
    s_mood_since = now;
    s_last_touch = now;
}

void mood_init(uint32_t now_ms)
{
    s_mood = MOOD_IDLE;
    s_mood_since = now_ms;
    s_last_touch = now_ms;
    s_displayed = MOOD_IDLE;
    s_tap_head = 0;
    s_tap_count = 0;
}

static int taps_in_window(uint32_t now, uint32_t window)
{
    int n = 0;
    for (int i = 0; i < s_tap_count; i++) {
        uint32_t t = s_taps[(s_tap_head + TAP_RING - 1 - i) % TAP_RING];
        if ((uint32_t)(now - t) <= window) n++;
    }
    return n;
}

void mood_tap(uint32_t now_ms)
{
    s_taps[s_tap_head] = now_ms;
    s_tap_head = (uint8_t)((s_tap_head + 1) % TAP_RING);
    if (s_tap_count < TAP_RING) s_tap_count++;

    s_last_touch = now_ms;

    if (taps_in_window(now_ms, 3000) >= 4) {
        enter(MOOD_ANNOYED, now_ms);
        buzzer_play(&buzzer_sad);
    } else if (taps_in_window(now_ms, 2000) >= 3) {
        enter(MOOD_PLEADING, now_ms);
    } else if (taps_in_window(now_ms, 1200) >= 2) {
        enter(MOOD_HAPPY, now_ms);
    } else {
        enter(MOOD_CURIOUS, now_ms);
    }
}

void mood_hold(uint32_t now_ms)
{
    s_last_touch = now_ms;
}

void mood_hold_long(uint32_t now_ms)
{
    s_last_touch = now_ms;
    enter(MOOD_ANGRY, now_ms);
    buzzer_play(&buzzer_sad);
}

void mood_release(uint32_t now_ms)
{
    s_last_touch = now_ms;
}

bool mood_update(uint32_t now_ms, mood_t *out)
{
    const mood_params_t *p = &k_moods[s_mood];

    if (p->duration_ms != 0 &&
        (uint32_t)(now_ms - s_mood_since) >= p->duration_ms) {
        enter(MOOD_IDLE, now_ms);
        // Falling back to idle is not "a touch", so do not reset the sleep timer.
        s_last_touch = now_ms - (p->duration_ms);
    }

    if ((s_mood == MOOD_IDLE || s_mood == MOOD_CURIOUS || s_mood == MOOD_HAPPY) &&
        (uint32_t)(now_ms - s_last_touch) >= SLEEP_AFTER_MS) {
        enter(MOOD_SLEEPY, now_ms);
    }

    bool changed = (s_mood != s_displayed);
    s_displayed = s_mood;
    if (out) *out = s_mood;
    return changed;
}

mood_t mood_current(void)
{
    return s_mood;
}

const mood_params_t *mood_params(mood_t m)
{
    if (m >= MOOD_COUNT) m = MOOD_IDLE;
    return &k_moods[m];
}
