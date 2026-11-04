/*
 * mochi — PWM tone player (see buzzer.h).
 *
 * Frequency: the slice clock is sys_clk / BUZZER_CLKDIV. At 150 MHz and a
 * divide of 64 that is 2.34375 MHz, so a 440 Hz tone gets ~5300 counter steps —
 * plenty of resolution, and the lowest reachable note is ~36 Hz.
 */

#include "buzzer.h"

#include <stddef.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pwm.h"

#include "board.h"

#define BUZZER_CLKDIV 64.0f

static uint     s_slice;
static uint     s_chan;
static uint16_t s_wrap;

static const buzzer_note_t *s_notes;
static uint8_t               s_count;
static uint8_t               s_index;
static uint32_t              s_next_at;

static uint32_t s_tone_until;

// --- Note frequencies (equal temperament, A4 = 440 Hz) --------------------

#define C4  262
#define D4  294
#define E4  330
#define G4  392
#define A4  440
#define C5  523
#define D5  587
#define E5  659
#define G5  784
#define A5  880
#define C6 1047
#define E6 1319

// --- Melodies -------------------------------------------------------------

static const buzzer_note_t k_startup[] = {
    { C5, 90 }, { E5, 90 }, { G5, 90 }, { C6, 160 }, { 0, 40 },
};

static const buzzer_note_t k_tap[] = {
    { E6, 30 }, { 0, 20 }, { A5, 55 },
};

static const buzzer_note_t k_release[] = {
    { G5, 55 }, { E5, 55 }, { C5, 90 },
};

static const buzzer_note_t k_sad[] = {
    { A4, 110 }, { G4, 110 }, { E4, 200 }, { 0, 40 },
};

// A yawn: low, slow, falling.
static const buzzer_note_t k_yawn[] = {
    { G4, 220 }, { E4, 260 }, { D4, 320 }, { 0, 60 },
};

// Goodnight: soft and short.
static const buzzer_note_t k_sleep[] = {
    { E5, 120 }, { C5, 200 }, { 0, 40 },
};

const buzzer_melody_t buzzer_startup = { k_startup, sizeof(k_startup) / sizeof(k_startup[0]) };
const buzzer_melody_t buzzer_tap     = { k_tap,     sizeof(k_tap)     / sizeof(k_tap[0]) };
const buzzer_melody_t buzzer_release = { k_release, sizeof(k_release) / sizeof(k_release[0]) };
const buzzer_melody_t buzzer_sad     = { k_sad,     sizeof(k_sad)     / sizeof(k_sad[0]) };
const buzzer_melody_t buzzer_yawn    = { k_yawn,    sizeof(k_yawn)    / sizeof(k_yawn[0]) };
const buzzer_melody_t buzzer_sleep   = { k_sleep,   sizeof(k_sleep)   / sizeof(k_sleep[0]) };

// --- PWM plumbing ---------------------------------------------------------

static void set_freq(uint16_t hz)
{
    if (hz == 0) {
        pwm_set_chan_level(s_slice, s_chan, 0);
        return;
    }

    float clk = (float)clock_get_hz(clk_sys) / BUZZER_CLKDIV;
    uint32_t wrap = (uint32_t)(clk / (float)hz + 0.5f);

    if (wrap < 2)      wrap = 2;
    if (wrap > 65535u) wrap = 65535u;

    s_wrap = (uint16_t)(wrap - 1);
    pwm_set_wrap(s_slice, s_wrap);
    // 50 % duty: a square wave is the loudest thing a piezo will take without
    // extra drive circuitry, and duty mostly shapes timbre here, not volume.
    pwm_set_chan_level(s_slice, s_chan, (uint16_t)((s_wrap + 1u) / 2u));
}

void buzzer_init(void)
{
    gpio_set_function(PIN_BUZZER, GPIO_FUNC_PWM);

    s_slice = pwm_gpio_to_slice_num(PIN_BUZZER);
    s_chan  = pwm_gpio_to_channel(PIN_BUZZER);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, BUZZER_CLKDIV);
    pwm_config_set_wrap(&cfg, 65535);
    pwm_init(s_slice, &cfg, true);

    pwm_set_chan_level(s_slice, s_chan, 0);

    s_notes = NULL;
    s_tone_until = 0;
}

void buzzer_play(const buzzer_melody_t *melody)
{
    if (!melody || melody->count == 0) return;

    s_notes   = melody->notes;
    s_count   = melody->count;
    s_index   = 0;
    s_next_at = 0;       // fire on the next update
    s_tone_until = 0;
}

void buzzer_tone(uint16_t freq, uint32_t ms)
{
    s_notes = NULL;
    set_freq(freq);
    s_tone_until = (uint32_t)to_ms_since_boot(get_absolute_time()) + ms;
}

void buzzer_stop(void)
{
    s_notes = NULL;
    s_tone_until = 0;
    set_freq(0);
}

bool buzzer_busy(void)
{
    return s_notes != NULL || s_tone_until != 0;
}

void buzzer_update(uint32_t now_ms)
{
    if (s_tone_until != 0 && (int32_t)(now_ms - s_tone_until) >= 0) {
        s_tone_until = 0;
        set_freq(0);
    }

    if (s_notes == NULL) return;
    if ((int32_t)(now_ms - s_next_at) < 0) return;

    if (s_index >= s_count) {
        s_notes = NULL;
        set_freq(0);
        return;
    }

    const buzzer_note_t *n = &s_notes[s_index++];
    set_freq(n->freq);
    s_next_at = now_ms + n->ms;
}
