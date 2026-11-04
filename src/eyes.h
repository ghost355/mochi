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
// that read as a smile or a frown. STAR_RING and STAR_SOLID are the round
// cartoon-eyes style: two big circles with a star pupil that travels inside
// them — STAR_RING draws the sclera as an outline, STAR_SOLID fills it and
// cuts the pupil out as a hole.
typedef enum {
    EYE_SHAPE_CAPSULE = 0,
    EYE_SHAPE_HAPPY,
    EYE_SHAPE_SAD,
    EYE_SHAPE_STAR_RING,
    EYE_SHAPE_STAR_SOLID,
} eye_shape_t;

// What the pupil inside a round eye looks like.
typedef enum {
    PUPIL_ASTROID = 0,   // four-point sparkle, solid
    PUPIL_ASTERISK,      // thin eight-spoke asterisk, perfectly symmetric
    PUPIL_SCRIBBLE,      // hand-drawn asterisk: uneven spokes, no two alike
    PUPIL_DOT,           // plain filled circle
    PUPIL_BLOB,          // fat disc with short spikes poking out of it
} pupil_style_t;

// Everything the mood system is allowed to change.
typedef struct {
    float          open_rest;      // 1.0 = fully open, 0.45 = sleepy
    float          gaze_amp;       // saccade amplitude multiplier
    eye_shape_t    shape;
    uint16_t       blink_min_ms;
    uint16_t       blink_max_ms;
    pupil_style_t  pupil;
    // Round styles only: a straight lid across the eye. `lid_pct` is how much
    // of the eye it covers at rest (0 = wide open, 100 = shut), `lid_angle` is
    // its tilt in degrees, positive dropping the inner corner.
    int8_t         lid_pct;
    int8_t         lid_angle;
    // Lower lid, percent of the eye it covers from the bottom. Both lids close
    // on a sleepy face; the upper one alone reads as a squint, not as fatigue.
    int8_t         lid_bottom_pct;
    // Brows. `brow_len` is the share of the eye width each brow spans, 0 = no
    // brow at all. `brow_lift` moves them up from the top of the eye,
    // `brow_tilt` drops the inner end (positive = angry), and `brow_mono`
    // joins the two into one line across the bridge.
    int8_t         brow_len;    // percent, 0..100
    int8_t         brow_lift;   // pixels above the eyeball
    int8_t         brow_tilt;   // pixels, inner end lower
    int8_t         brow_mono;
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

// Look at a specific offset right now (x/y in pixels, roughly -6..6) and hold
// it for a few seconds. Lets input and sensors steer the gaze.
void eyes_glance(float x, float y);

#endif // MOCHI_EYES_H
