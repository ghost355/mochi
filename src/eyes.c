/*
 * mochi — procedural eye engine (see eyes.h).
 *
 * Geometry: two capsules. A capsule is a rounded rectangle whose corner
 * radius is half its height, which is why the vertical size can collapse to a
 * 3 px slit on blink and still look like an eyelid rather than a shrinking box.
 *
 * Everything is integer pixel work except the blink curve and the drift, which
 * use floats — the RP2350 has a single-precision FPU, and this is a handful of
 * operations per frame.
 */

#include "eyes.h"

#include <math.h>
#include <string.h>

#include "display/sh1106.h"

#define PI_F 3.14159265f

// --- Face geometry --------------------------------------------------------
#define EYE_W        34     // full-open width
#define EYE_H        42     // full-open height
#define EYE_GAP      18     // space between the two eyes
#define EYE_CY       32     // vertical centre of the face
#define BLINK_MIN_H  3      // thinnest eyelid

#define BLINK_MS     130
#define DOUBLE_GAP_MS 80
#define REACT_MS     250
#define REACT_WIDEN  0.22f   // extra height, as a fraction of EYE_H
#define SQUINT_OPEN  0.55f   // open fraction while a long press is held

#define WINK_MS        260
#define YAWN_SQUEEZE_MS 350
#define YAWN_WIDE_MS    900
#define REST_SLEW       0.6f   // openness units per second

#define NUM_GAZE     8

static const struct {
    int x, y;
} k_gaze[NUM_GAZE] = {
    {-5, -4}, {-3, 0}, {-1, 3}, {0, -2},
    {2, 4},  {4, 1},  {5, -3}, {1, 2},
};

// --- State ----------------------------------------------------------------
static uint32_t s_rng;

static float   s_open;            // 0..1 eyelid opening
static bool    s_blinking;
static uint32_t s_blink_start;
static bool    s_double_pending;
static uint32_t s_next_blink;
static bool    s_blink_request;

static float   s_look_x, s_look_y;      // current gaze offset
static float   s_look_x_t, s_look_y_t;  // target gaze offset
static uint32_t s_next_saccade;

static eyes_rect_t s_prev;
static uint32_t    s_now;
static int         s_last_h_l, s_last_h_r, s_last_cx, s_last_cy;
static int         s_last_shape;

static eyes_config_t s_cfg = {
    .open_rest = 1.0f,
    .gaze_amp  = 1.0f,
    .shape     = EYE_SHAPE_CAPSULE,
    .blink_min_ms = 2200,
    .blink_max_ms = 6500,
};

static bool        s_squint;
static float       s_extra;        // extra height from a tap reaction
static uint32_t    s_react_at;

static float       s_rest = 1.0f;  // smoothed resting openness
static uint32_t    s_last_ms;
static int         s_wink = -1;
static uint32_t    s_wink_at;
static bool        s_yawn_active;
static uint32_t    s_yawn_at;
static uint32_t    s_next_tic;

// --- Small helpers --------------------------------------------------------

static uint32_t rng_next(void)
{
    // xorshift32: cheap, no libc dependency, plenty for a toy face.
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static int rng_range(int lo, int hi)   // inclusive
{
    if (hi <= lo) return lo;
    return lo + (int)(rng_next() % (uint32_t)(hi - lo + 1));
}

static void fill_rect(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            sh1106_set_pixel(x, y, true);
}

// Horizontal inset of a rounded corner, `dist` rows in from the outer edge.
static int corner_inset(int dist, int r)
{
    float d = (float)(r - dist) - 0.5f;   // distance from the corner centre
    if (d <= 0.0f) return 0;

    float s = sqrtf((float)r * (float)r - d * d);
    int inset = r - (int)(s + 0.5f);
    return inset < 0 ? 0 : inset;
}

// Filled capsule: rounded rect with radius = min(w, h) / 2.
static void fill_capsule(int cx, int cy, int w, int h)
{
    if (w <= 0 || h <= 0) return;

    int x0 = cx - w / 2;
    int y0 = cy - h / 2;
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;

    int r = h / 2;
    if (r > w / 2) r = w / 2;

    for (int y = y0; y <= y1; y++) {
        int inset = 0;
        if (y < y0 + r)      inset = corner_inset(y - y0, r);
        else if (y > y1 - r) inset = corner_inset(y1 - y, r);

        int lx = x0 + inset;
        int rx = x1 - inset;
        if (rx >= lx) fill_rect(lx, y, rx, y);
    }
}

// Half-width of a filled ellipse at row `dy` (relative to the centre).
static int ellipse_halfwidth(int rx, int ry, int dy)
{
    if (ry <= 0) return -1;
    if (dy < -ry || dy > ry) return -1;

    float t = (float)dy / (float)ry;
    float s = 1.0f - t * t;
    if (s <= 0.0f) return 0;

    return (int)((float)rx * sqrtf(s) + 0.5f);
}

// Crescent: an ellipse minus a slightly smaller ellipse shifted by `cut_dy`.
// A cut ellipse above the eye leaves the lower arc (a smile); below leaves a
// frown. Scaling with `ry` means a happy face still blinks properly.
static void fill_crescent(int cx, int cy, int rx, int ry, int cut_dy)
{
    int cut_rx = rx - 3;
    int cut_ry = ry - 3;
    if (cut_rx < 1) cut_rx = 1;
    if (cut_ry < 1) cut_ry = 1;

    for (int dy = -ry; dy <= ry; dy++) {
        int a = ellipse_halfwidth(rx, ry, dy);
        if (a < 0) continue;

        int b = ellipse_halfwidth(cut_rx, cut_ry, dy - cut_dy);
        if (b < 0) {
            fill_rect(cx - a, cy + dy, cx + a, cy + dy);
            continue;
        }

        if (cx - a <= cx - b - 1) fill_rect(cx - a, cy + dy, cx - b - 1, cy + dy);
        if (cx + b + 1 <= cx + a) fill_rect(cx + b + 1, cy + dy, cx + a, cy + dy);
    }
}

static float eye_open_for(int which)
{
    float open = s_open * s_rest;

    if (s_squint && !s_blinking) open *= SQUINT_OPEN;
    open += s_extra;

    // Yawn: clamp shut, throw wide, then hand control back to the mood.
    if (s_yawn_active) {
        uint32_t t = s_now - s_yawn_at;
        if (t < YAWN_SQUEEZE_MS)    open = 0.10f;
        else if (t < YAWN_WIDE_MS)  open = 1.30f;
        else                        s_yawn_active = false;
    }

    // Wink: only the chosen eye closes.
    if (which == s_wink) {
        uint32_t t = s_now - s_wink_at;
        if (t < WINK_MS) {
            open *= 1.0f - sinf(((float)t / (float)WINK_MS) * PI_F);
        } else {
            s_wink = -1;
        }
    }

    if (open > 1.35f) open = 1.35f;
    if (open < 0.06f) open = 0.06f;
    return open;
}

static int eye_h_for(int which)
{
    int h = (int)((float)EYE_H * eye_open_for(which) + 0.5f);
    if (h < BLINK_MIN_H) h = BLINK_MIN_H;
    return h;
}

// One eye, at its own height, in whichever silhouette the mood selected.
static void draw_eye(int cx, int cy, int w, int h)
{
    if (s_cfg.shape == EYE_SHAPE_CAPSULE || h < 10) {
        fill_capsule(cx, cy, w, h);
    } else if (s_cfg.shape == EYE_SHAPE_HAPPY) {
        // Smile: lower arc, opening upward.
        int cut = (h / 2) * 55 / 100;
        fill_crescent(cx, cy, w / 2, h / 2, -cut);
    } else {
        // Frown: upper arc, opening downward.
        int cut = -(h / 2) * 55 / 100;
        fill_crescent(cx, cy, w / 2, h / 2, -cut);
    }
}

// --- Public API -----------------------------------------------------------

void eyes_init(uint32_t seed)
{
    s_rng = seed ? seed : 0x9E3779B9u;

    s_open = 1.0f;
    s_blinking = false;
    s_double_pending = false;
    s_blink_request = false;

    s_squint = false;
    s_extra = 0.0f;
    s_react_at = 0;

    s_rest = s_cfg.open_rest;
    s_last_ms = 0;
    s_wink = -1;
    s_yawn_active = false;
    s_next_tic = rng_range(6000, 12000);

    s_look_x = s_look_y = 0.0f;
    s_look_x_t = s_look_y_t = 0.0f;

    uint32_t t = (uint32_t)(seed & 0xFFFF);
    s_next_blink = t + rng_range(1500, 4000);
    s_next_saccade = t + rng_range(600, 1800);

    memset(&s_prev, 0, sizeof(s_prev));
    s_last_h_l = s_last_h_r = s_last_cx = s_last_cy = -1;
    s_last_shape = -1;
}

void eyes_blink_now(void)
{
    s_blink_request = true;
}

void eyes_react_tap(void)
{
    s_react_at = s_now;
    s_extra = REACT_WIDEN;
}

void eyes_yawn(void)
{
    s_yawn_active = true;
    s_yawn_at = s_now;
}

void eyes_wink(int eye)
{
    s_wink = (eye != 0);
    s_wink_at = s_now;
}

void eyes_set_squint(bool on)
{
    s_squint = on;
}

void eyes_set_config(const eyes_config_t *cfg)
{
    if (!cfg) return;
    s_cfg = *cfg;
    if (s_cfg.open_rest <= 0.0f) s_cfg.open_rest = 0.05f;
    if (s_cfg.blink_min_ms < 400) s_cfg.blink_min_ms = 400;
    if (s_cfg.blink_max_ms < s_cfg.blink_min_ms)
        s_cfg.blink_max_ms = s_cfg.blink_min_ms;
}

void eyes_update(uint32_t now_ms)
{
    uint32_t elapsed = now_ms - s_last_ms;
    float dt = (float)elapsed * 0.001f;
    if (dt > 0.2f) dt = 0.2f;      // first frame, or the loop stalled
    s_last_ms = now_ms;
    s_now = now_ms;

    // Resting openness glides toward whatever the mood asked for, so falling
    // asleep and waking up are animations instead of jumps.
    {
        float delta = s_cfg.open_rest - s_rest;
        float step  = REST_SLEW * dt;
        if (delta > step)       s_rest += step;
        else if (delta < -step) s_rest -= step;
        else                    s_rest = s_cfg.open_rest;
    }

    // --- Blink ---
    if (s_blink_request) {
        s_blink_request = false;
        if (!s_blinking) {
            s_blinking = true;
            s_blink_start = now_ms;
        }
    }

    if (!s_blinking && now_ms >= s_next_blink) {
        s_blinking = true;
        s_blink_start = now_ms;
        s_next_blink = now_ms + (uint32_t)rng_range(s_cfg.blink_min_ms, s_cfg.blink_max_ms);
    }

    if (s_blinking) {
        uint32_t t = now_ms - s_blink_start;
        if (t < BLINK_MS) {
            float p = (float)t / (float)BLINK_MS;
            s_open = 1.0f - sinf(p * PI_F);
        } else {
            s_open = 1.0f;
            s_blinking = false;

            // One blink in three comes back for a second, rapid one.
            if (s_double_pending) {
                s_double_pending = false;
                s_blink_request = true;
                s_next_blink = now_ms + DOUBLE_GAP_MS;
            } else if (rng_range(0, 2) == 0) {
                s_double_pending = true;
            }
        }
    }

    // --- Reaction (tap): the widen decays linearly back to zero ---
    if (s_extra > 0.0f) {
        uint32_t dt = now_ms - s_react_at;
        if (dt >= REACT_MS) {
            s_extra = 0.0f;
        } else {
            s_extra = REACT_WIDEN * (1.0f - (float)dt / (float)REACT_MS);
        }
    }

    // --- Random tics: small involuntary gestures that keep the face alive ---
    if (!s_blinking && !s_yawn_active && s_rest > 0.80f &&
        (int32_t)(now_ms - s_next_tic) >= 0) {
        if (rng_range(0, 2) == 0) {
            s_wink = rng_range(0, 1);
            s_wink_at = now_ms;
        } else {
            s_next_saccade = now_ms;   // glance somewhere right now
        }
        s_next_tic = now_ms + (uint32_t)rng_range(7000, 16000);
    }

    // --- Saccade ---
    if (now_ms >= s_next_saccade) {
        int i = rng_range(0, NUM_GAZE - 1);
        s_look_x_t = (float)k_gaze[i].x;
        s_look_y_t = (float)k_gaze[i].y;
        s_next_saccade = now_ms + (uint32_t)rng_range(900, 3200);
    }

    // Eyes snap to a new target quickly; a 0.35 lerp per 30 fps frame lands
    // the gaze in roughly 100 ms, which reads as a saccade and not a slide.
    s_look_x += (s_look_x_t - s_look_x) * 0.35f;
    s_look_y += (s_look_y_t - s_look_y) * 0.35f;
}

eyes_rect_t eyes_draw(void)
{
    int h_l = eye_h_for(0);
    int h_r = eye_h_for(1);
    int h   = h_l > h_r ? h_l : h_r;   // for the dirty rect
    int w = EYE_W;

    // Slow vertical bob: ~1.5 px of travel with a 4 s period, so the face
    // never looks frozen even when the gaze is still.
    int drift = (int)(1.5f * sinf((float)s_now * 0.0016f));

    int cx_l = (SH1106_WIDTH / 2) - (EYE_GAP / 2) - (w / 2) + (int)(s_look_x * s_cfg.gaze_amp);
    int cx_r = (SH1106_WIDTH / 2) + (EYE_GAP / 2) + (w / 2) + (int)(s_look_x * s_cfg.gaze_amp);
    int cy   = EYE_CY + (int)(s_look_y * s_cfg.gaze_amp) + drift;

    // Nothing moved since the last frame: the framebuffer already holds this
    // exact image, so skip both the redraw and the I2C transfer. Most frames
    // between blinks and saccades land here, which keeps the bus quiet.
    if (s_prev.w > 0 && h_l == s_last_h_l && h_r == s_last_h_r &&
        cx_l == s_last_cx && cy == s_last_cy &&
        (int)s_cfg.shape == s_last_shape) {
        eyes_rect_t none = { 0, 0, 0, 0 };
        return none;
    }

    int x0 = cx_l - w / 2;
    int x1 = cx_r + w / 2;
    int y0 = cy - h / 2;
    int y1 = cy + h / 2;

    // Clear the previous frame's area first: the eyes move and shrink, so the
    // old pixels must go.
    if (s_prev.w > 0 && s_prev.h > 0) {
        for (int y = s_prev.y; y < s_prev.y + s_prev.h; y++)
            for (int x = s_prev.x; x < s_prev.x + s_prev.w; x++)
                sh1106_set_pixel(x, y, false);
    }

    draw_eye(cx_l, cy, w, h_l);
    draw_eye(cx_r, cy, w, h_r);

    // Dirty rect: union of what was cleared and what was just drawn.
    eyes_rect_t r;
    r.x = x0 < s_prev.x ? x0 : s_prev.x;
    r.y = y0 < s_prev.y ? y0 : s_prev.y;
    int rx = x1 > (s_prev.x + s_prev.w - 1) ? x1 : (s_prev.x + s_prev.w - 1);
    int ry = y1 > (s_prev.y + s_prev.h - 1) ? y1 : (s_prev.y + s_prev.h - 1);

    if (s_prev.w <= 0 || s_prev.h <= 0) {
        r.x = x0;
        r.y = y0;
        rx = x1;
        ry = y1;
    }

    // Clamp to the panel.
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (rx > SH1106_WIDTH - 1)  rx = SH1106_WIDTH - 1;
    if (ry > SH1106_HEIGHT - 1) ry = SH1106_HEIGHT - 1;

    r.w = rx - r.x + 1;
    r.h = ry - r.y + 1;

    s_prev = r;
    s_last_h_l = h_l;
    s_last_h_r = h_r;
    s_last_cx = cx_l;
    s_last_cy = cy;
    s_last_shape = (int)s_cfg.shape;
    return r;
}
