/*
 * mochi — procedural eye engine.
 *
 * Clean-room implementation: two capsule-shaped eyes drawn from parameters,
 * no bitmap frames, no third-party code.
 *
 * What it does:
 *   - blink, with a smooth close/open curve and occasional double blinks
 *   - saccades: the gaze jumps to a new offset every 1-3 s and eases there
 *   - idle drift: a slow vertical bob so the face never looks frozen
 *
 * The engine owns its own dirty-rect bookkeeping. eyes_draw() reports the
 * union of the previous and the current eye bounds, which is exactly the
 * region the caller has to push to the display — everything else on screen is
 * untouched.
 */

#ifndef MOCHI_EYES_H
#define MOCHI_EYES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x, y, w, h;   // dirty rect in pixels, already clamped to the screen
} eyes_rect_t;

// Eye silhouette. CAPSULE is the neutral shape; HAPPY and SAD are crescents
// that read as a smile or a frown.
typedef enum {
    EYE_SHAPE_CAPSULE = 0,
    EYE_SHAPE_HAPPY,
    EYE_SHAPE_SAD,
} eye_shape_t;

// Everything the mood system is allowed to change.
typedef struct {
    float       open_rest;      // 1.0 = fully open, 0.45 = sleepy
    float       gaze_amp;       // saccade amplitude multiplier
    eye_shape_t shape;
    uint16_t    blink_min_ms;
    uint16_t    blink_max_ms;
} eyes_config_t;

void eyes_init(uint32_t seed);

// Apply a mood. Takes effect on the next eyes_draw().
void eyes_set_config(const eyes_config_t *cfg);

// Advance the animation to `now_ms` (monotonic milliseconds).
void eyes_update(uint32_t now_ms);

// Draw the current frame into the display framebuffer and return the region
// that changed since the previous call.
eyes_rect_t eyes_draw(void);

// Force a blink on the next update (for touch reaction in M2).
void eyes_blink_now(void);

// Reaction to a tap: the eyes pop wide for ~250 ms, then settle.
void eyes_react_tap(void);

// Held touch: squint while `on` is true.
void eyes_set_squint(bool on);

// Wake-up yawn: squeeze shut, open wide, settle.
void eyes_yawn(void);

// One-off wink. `eye` 0 = left, 1 = right.
void eyes_wink(int eye);

#endif // MOCHI_EYES_H
