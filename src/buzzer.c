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

// A chirp or burp step: like buzzer_note_t, but it carries its own duty
// cycle — the burp's roughness comes from swinging duty on every pulse,
// which buzzer_note_t has no field for. Hence a second, smaller table engine.
typedef struct {
    uint16_t freq;  // Hz, 0 = gap
    uint16_t ms;    // how long this step lasts
    uint8_t  duty;  // percent of the PWM period
} burst_note_t;

typedef struct {
    const burst_note_t *steps;
    uint8_t             count;
} burst_t;

static const burst_note_t *s_burst;
static uint8_t             s_burst_count;
static uint8_t             s_burst_index;
static uint32_t            s_burst_next_at;

// A melody to start the moment the burst ends. A belch followed by a goodnight
// phrase is one gesture, not two: without this the second call would cut the
// first one off.
static const buzzer_melody_t *s_burst_then;

// Scratch table the burp is assembled into before playback.
static burst_note_t s_burp_steps[14 * 2 - 1];  // worst case: 14 pulses + 13 gaps

static uint32_t s_rng_state;
static uint32_t s_rng_nonce = 1;

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

// Greeting arpeggio: C5-E5-G5-C6-E6, top note held so it lands like a hello.
// 650 ms total, short enough that the eyes never wait on it.
static const buzzer_note_t k_startup[] = {
    { C5, 90 }, { E5, 90 }, { G5, 90 }, { C6, 160 }, { E6, 180 }, { 0, 40 },
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

// --- Chirp chatter --------------------------------------------------------
// Four variants of a droid talking: 5-7 blips of 25-45 ms separated by
// 12-25 ms gaps. The pitches deliberately leap around 700-2600 Hz instead of
// following a scale — chatter lives in odd intervals and uneven rhythms, and
// nothing here lines up with a note name.

// Six blips spanning the low and high ends of the chatter range.
static const burst_note_t k_chirp0[] = {
    { 1870, 32, 50 }, { 0, 15, 50 }, { 2310, 27, 50 }, { 0, 12, 50 },
    { 1440, 38, 50 }, { 0, 18, 50 }, { 2560, 25, 50 }, { 0, 22, 50 },
    { 980,  41, 50 }, { 0, 14, 50 }, { 2120, 30, 50 },
};

// Seven blips: the longest, most agitated variant.
static const burst_note_t k_chirp1[] = {
    { 2440, 25, 50 }, { 0, 13, 50 }, { 1180, 34, 50 }, { 0, 21, 50 },
    { 745,  29, 50 }, { 0, 16, 50 }, { 2050, 41, 50 }, { 0, 12, 50 },
    { 1620, 26, 50 }, { 0, 24, 50 }, { 2540, 38, 50 }, { 0, 15, 50 },
    { 890,  45, 50 },
};

// Five blips, slower and more spaced: a lazy comment.
static const burst_note_t k_chirp2[] = {
    { 730,  45, 50 }, { 0, 25, 50 }, { 1950, 30, 50 }, { 0, 14, 50 },
    { 2530, 25, 50 }, { 0, 19, 50 }, { 1330, 36, 50 }, { 0, 12, 50 },
    { 1710, 43, 50 },
};

// Six blips swinging hard between high and low: an urgent question.
static const burst_note_t k_chirp3[] = {
    { 2210, 28, 50 }, { 0, 17, 50 }, { 1030, 40, 50 }, { 0, 23, 50 },
    { 2470, 25, 50 }, { 0, 12, 50 }, { 840,  33, 50 }, { 0, 20, 50 },
    { 1540, 37, 50 }, { 0, 14, 50 }, { 2080, 31, 50 },
};

static const burst_t k_chirp[4] = {
    { k_chirp0, sizeof(k_chirp0) / sizeof(k_chirp0[0]) },
    { k_chirp1, sizeof(k_chirp1) / sizeof(k_chirp1[0]) },
    { k_chirp2, sizeof(k_chirp2) / sizeof(k_chirp2[0]) },
    { k_chirp3, sizeof(k_chirp3) / sizeof(k_chirp3[0]) },
};

// --- PWM plumbing ---------------------------------------------------------

static void set_freq_duty(uint16_t hz, uint8_t duty)
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

    // Duty shapes timbre, not volume: a piezo is a capacitor, so a narrow
    // pulse only partly discharges it and the tone comes out rough — exactly
    // what the burp wants. Melodies stay at 50 %, the loudest clean square
    // wave a piezo takes without extra drive circuitry.
    if (duty > 100) duty = 100;
    pwm_set_chan_level(s_slice, s_chan,
                       (uint16_t)(((uint32_t)(s_wrap + 1u) * duty) / 100u));
}

static void set_freq(uint16_t hz)
{
    set_freq_duty(hz, 50);
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
    s_burst = NULL;
    s_burst_then = NULL;
}

void buzzer_play(const buzzer_melody_t *melody)
{
    if (!melody || melody->count == 0) return;

    s_notes   = melody->notes;
    s_count   = melody->count;
    s_index   = 0;
    s_next_at = 0;       // fire on the next update
    s_tone_until = 0;
    s_burst = NULL;
    s_burst_then = NULL;
}

void buzzer_tone(uint16_t freq, uint32_t ms)
{
    s_notes = NULL;
    s_burst = NULL;
    set_freq(freq);
    s_tone_until = (uint32_t)to_ms_since_boot(get_absolute_time()) + ms;
}

void buzzer_stop(void)
{
    s_notes = NULL;
    s_tone_until = 0;
    s_burst = NULL;
    s_burst_then = NULL;
    set_freq(0);
}

bool buzzer_busy(void)
{
    return s_notes != NULL || s_tone_until != 0 || s_burst != NULL;
}

void buzzer_update(uint32_t now_ms)
{
    // Reactions run on their own slot and own the frame while they last;
    // the melody underneath was already dropped when they started.
    if (s_burst != NULL) {
        if ((int32_t)(now_ms - s_burst_next_at) >= 0) {
            if (s_burst_index >= s_burst_count) {
                s_burst = NULL;
                set_freq_duty(0, 50);

                const buzzer_melody_t *then = s_burst_then;
                s_burst_then = NULL;
                if (then) buzzer_play(then);
            } else {
                const burst_note_t *n = &s_burst[s_burst_index++];
                set_freq_duty(n->freq, n->duty);
                s_burst_next_at = now_ms + n->ms;
            }
        }
        return;
    }

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

// --- Reactive bursts ------------------------------------------------------

// xorshift32: reactions must sound different every time, and pulling libc
// rand() in for a buzzer is not worth it. Seeded from the frame clock; the
// nonce keeps two calls inside the same millisecond apart. Zero would be a
// fixed point, hence the |1. The golden-ratio multiplier scatters the
// clock's low bits, which only ever tick by the frame interval.
static void rng_seed(uint32_t now_ms)
{
    s_rng_state = now_ms * 2654435769u + s_rng_nonce++;
    s_rng_state |= 1u;
}

static uint32_t rnd32(void)
{
    uint32_t x = s_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng_state = x;
    return x;
}

static void burst_start(const burst_note_t *steps, uint8_t count)
{
    // Reactions win: whatever melody or held tone was playing is dropped
    // for good — nobody wants a yawn finishing over a chirp.
    s_notes         = NULL;
    s_tone_until    = 0;
    s_burst         = steps;
    s_burst_count   = count;
    s_burst_index   = 0;
    s_burst_next_at = 0;  // fire on the next update
}

// Lays out one belch: 10-14 pulses sliding from a low growl down to a
// rattle, each with its own jittered pitch, length, gap and duty. Regenerated
// per call so no two burps match.
static uint8_t build_burp(void)
{
    uint8_t  pulses = (uint8_t)(10 + rnd32() % 5);    // 10..14
    uint16_t hz_top = (uint16_t)(110 + rnd32() % 11); // starts around 110..120 Hz
    uint16_t hz_end = (uint16_t)(55 + rnd32() % 11);  // lands around 55..65 Hz

    uint8_t n = 0;
    for (uint8_t i = 0; i < pulses; i++) {
        // Straight slide between the two endpoints, plus jitter so it reads
        // as a rapid stumble downward rather than a musical glissando.
        int32_t hz = (int32_t)hz_top
                   - (int32_t)((hz_top - hz_end) * i / (pulses - 1))
                   + (int32_t)(rnd32() % 13) - 6;
        if (hz < 50) hz = 50;  // keep the tail audible even with jitter

        burst_note_t *step = &s_burp_steps[n++];
        step->freq = (uint16_t)hz;
        step->ms   = (uint16_t)(18 + rnd32() % 23);  // 18..40 ms
        step->duty = (uint8_t)(20 + rnd32() % 41);   // 20..60 %, the roughness

        if (i + 1 < pulses) {
            burst_note_t *gap = &s_burp_steps[n++];
            gap->freq = 0;
            gap->ms   = (uint16_t)(10 + rnd32() % 16);  // 10..25 ms
            gap->duty = 50;
        }
    }
    return n;
}

void buzzer_chirp(uint32_t now_ms)
{
    rng_seed(now_ms);

    // One of four precomputed chatter patterns, so repeated reactions don't
    // sound like a loop.
    const burst_t *v = &k_chirp[rnd32() & 3u];
    burst_start(v->steps, v->count);
}

void buzzer_burp(uint32_t now_ms)
{
    rng_seed(now_ms);
    burst_start(s_burp_steps, build_burp());
}

void buzzer_burp_then(uint32_t now_ms, const buzzer_melody_t *then)
{
    buzzer_burp(now_ms);
    s_burst_then = then;
}
