/*
 * mochi — TTP223 capacitive touch input.
 *
 * The sensor is polled, not interrupt-driven: the frame loop already runs at
 * 50 fps, so polling adds at most 20 ms of latency, while an IRQ handler would
 * have to stay away from the eye engine anyway.
 *
 * The TTP223 does its own filtering but still needs a software debounce: after
 * a period of inactivity the first touch can arrive up to ~220 ms late, and the
 * output can chatter for a few milliseconds on release.
 *
 * The state machine is split from the GPIO read so it can be tested on a host
 * by feeding synthetic levels into touch_sample().
 */

#ifndef MOCHI_TOUCH_H
#define MOCHI_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#define TOUCH_DEBOUNCE_MS 30

typedef struct {
    bool     pressed;        // debounced state
    bool     just_pressed;   // one-shot: accepted a press this sample
    bool     just_released;  // one-shot: accepted a release this sample
    uint32_t held_ms;        // how long the press has lasted (0 when idle)
} touch_state_t;

typedef struct {
    bool     stable;         // current debounced level
    bool     candidate;      // level we are waiting to confirm
    uint32_t candidate_since;
    uint32_t pressed_since;
    bool     have_press_time;
} touch_ctx_t;

void touch_init(void);

// Read the pin and advance the state machine.
touch_state_t touch_update(uint32_t now_ms);

// Same, but from an explicit level — used by host tests.
touch_state_t touch_sample(touch_ctx_t *ctx, bool raw, uint32_t now_ms);

#endif // MOCHI_TOUCH_H
