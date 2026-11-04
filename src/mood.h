/*
 * mochi — mood state machine.
 *
 * The robot has a small set of behaviours rather than a fixed idle animation.
 * Mood is driven purely by touch:
 *
 *   IDLE      default
 *   CURIOUS   one tap, lasts 2.5 s
 *   HAPPY     two taps within 1.2 s, lasts 4 s
 *   ANNOYED   four taps within 3 s, lasts 3 s
 *   ANGRY     holding the pad for 1.5 s, lasts 3.5 s
 *   SLEEPY    45 s without a touch, until the next tap
 *
 * Each mood carries the parameters the eye engine needs (blink rate, saccade
 * rate and amplitude, resting openness, eye shape), so the animation itself
 * stays a dumb renderer.
 */

#ifndef MOCHI_MOOD_H
#define MOCHI_MOOD_H

#include <stdbool.h>
#include <stdint.h>

#include "eyes.h"

typedef enum {
    MOOD_IDLE = 0,
    MOOD_CURIOUS,
    MOOD_HAPPY,
    MOOD_ANNOYED,
    MOOD_ANGRY,
    MOOD_SLEEPY,
    MOOD_COUNT,
} mood_t;

typedef struct {
    const char   *name;
    eyes_config_t eyes;
    uint16_t      duration_ms;    // 0 = stays until something else happens
} mood_params_t;

void mood_init(uint32_t now_ms);

void mood_tap(uint32_t now_ms);
void mood_hold(uint32_t now_ms);

// The touch has been held long enough to be a grab rather than a tap.
void mood_hold_long(uint32_t now_ms);
void mood_release(uint32_t now_ms);

// Advance timers. Returns the new mood if it changed since the last call.
bool mood_update(uint32_t now_ms, mood_t *out);

mood_t mood_current(void);
const mood_params_t *mood_params(mood_t m);

#define SLEEP_AFTER_MS 45000u

#endif // MOCHI_MOOD_H
