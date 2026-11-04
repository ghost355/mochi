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

// Round style: two big rings with a star pupil that travels inside them.
#define STAR_EYE_D   44
#define ROUND_EYE_CY 36
#define EYES_TOP_ROW 7     // rows 0..6 are the text banner's
#define STAR_R       5
#define ASTERISK_R   4
#define SCRIBBLE_R       6
#define DOT_R            3
#define BLOB_CORE_R      3
#define STAR_GAP     8
#define STAR_RING    2
#define STAR_PUPIL_GAIN 1.3f

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
static int         s_last_px, s_last_py;
static int         s_last_open_l, s_last_open_r;

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

// Filled ellipse, optionally turning pixels off instead of on. Used by the
// round eye style: a ring is an outer ellipse with the inner one cleared.
static void fill_ellipse_ex(int cx, int cy, int rx, int ry, bool on)
{
    if (rx < 0 || ry < 0) return;
    for (int dy = -ry; dy <= ry; dy++) {
        int dx = ellipse_halfwidth(rx, ry, dy);
        if (dx < 0) continue;
        for (int x = cx - dx; x <= cx + dx; x++)
            sh1106_set_pixel(x, cy + dy, on);
    }
}

// Four-point sparkle: the astroid |x|^(2/3) + |y|^(2/3) = r^(2/3). At 6 px
// across it reads as a star, and it stays clean without anti-aliasing because
// the shape is concave everywhere.
static bool star_hit(int dx, int dy, int r)
{
    if (dx == 0 && dy == 0) return true;
    if (r <= 0) return false;

    float ax = cbrtf(fabsf((float)dx) / (float)r);
    float ay = cbrtf(fabsf((float)dy) / (float)r);
    return ax * ax + ay * ay <= 1.0f;
}

static void draw_star(int cx, int cy, int r, bool on)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (star_hit(dx, dy, r))
                sh1106_set_pixel(cx + dx, cy + dy, on);
}

// Eight-spoke asterisk: one pixel thick along the axes and the diagonals.
// At r = 5 that is 21 lit pixels — small enough to read as a pupil, spiky
// enough to read as an asterisk.
static void draw_asterisk(int cx, int cy, int r, bool on)
{
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int ax = dx < 0 ? -dx : dx;
            int ay = dy < 0 ? -dy : dy;

            if ((ay == 0 && ax <= r) || (ax == 0 && ay <= r) ||
                (ax == ay && ax <= r))
                sh1106_set_pixel(cx + dx, cy + dy, on);
        }
    }
}

// A quick pen asterisk: strokes fan out from a jittered centre, each with its
// own length and angle. `pct` scales it down as the eyelid closes.
// Two hand-drawn pupils, authored as pixel masks rather than generated from
// random angles: at 9 px a generator produces noise, a human produces a mark
// that reads as an asterisk. The two eyes use different masks on purpose.
static const char *const k_scrib[2][9] = {
    {   // left: full six arms, top arm a touch short
        "....#....",
        "....#....",
        ".#..#..#.",
        "..#.#.#..",
        "....#....",
        "...#.#...",
        "..#.#.#..",
        ".#..#..#.",
        "....#....",
    },
    {   // right: thicker core, arms pushed off the axes
        "....#....",
        "....#...#",
        ".#...#..#",
        "..#..#...",
        "....##...",
        "..##.#...",
        ".#..#.#..",
        "..#...#..",
        ".....#...",
    },
};

static void draw_scribble(int cx, int cy, int eye, int pct, bool on)
{
    if (pct < 55) {                      // eyelid nearly shut: a speck is enough
        sh1106_set_pixel(cx, cy, on);
        sh1106_set_pixel(cx + 1, cy, on);
        return;
    }

    for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
            if (k_scrib[eye][r][c] != '#') continue;
            int dx = ((c - 4) * pct) / 100;
            int dy = ((r - 4) * pct) / 100;
            sh1106_set_pixel(cx + dx, cy + dy, on);
        }
    }
}

static void draw_dot(int cx, int cy, int r, bool on);

// A fat disc with short spikes: the pupil reads as a blob of ink with a few
// bristles, not as a star. Cardinals get a longer tail than diagonals, and the
// two eyes are offset by one step so they never look stamped from a symbol.
static void draw_blob(int cx, int cy, int eye, int pct, bool on)
{
    int core = (BLOB_CORE_R * pct) / 100;
    if (core < 1) core = 1;

    draw_dot(cx, cy, core, on);

    static const int8_t dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int8_t dy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };

    for (int i = 0; i < 8; i++) {
        int k = (i + eye) & 7;
        bool cardinal = (dx[k] == 0 || dy[k] == 0);

        if (pct < 70) continue;        // the eyelid takes the tails first

        if (cardinal) {
            for (int r = core + 1; r <= core + 2; r++)
                sh1106_set_pixel(cx + dx[k] * r, cy + dy[k] * r, on);
        } else {
            // A one-pixel stub on the diagonal only meets the disc at a corner,
            // and a corner touch reads as a separate dot at this scale. Fill the
            // notch instead: two pixels sharing an edge with the disc.
            sh1106_set_pixel(cx + dx[k] * core, cy + dy[k] * (core - 1), on);
            sh1106_set_pixel(cx + dx[k] * (core - 1), cy + dy[k] * core, on);
        }
    }
}

static void draw_dot(int cx, int cy, int r, bool on)
{
    if (r < 1) r = 1;
    for (int dy = -r; dy <= r; dy++) {
        int dx = ellipse_halfwidth(r, r, dy);
        if (dx < 0) continue;
        for (int x = cx - dx; x <= cx + dx; x++)
            sh1106_set_pixel(x, cy + dy, on);
    }
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

// Per-style face geometry: the capsule face is a pair of tall bars, the round
// face is a pair of big circles, so they need different sizes and gaps.
typedef struct {
    int w, h, gap;
} eye_dims_t;

static bool star_style(void)
{
    return s_cfg.shape == EYE_SHAPE_STAR_RING || s_cfg.shape == EYE_SHAPE_STAR_SOLID;
}

static eye_dims_t dims_for(eye_shape_t shape)
{
    eye_dims_t d = { EYE_W, EYE_H, EYE_GAP };
    if (shape == EYE_SHAPE_STAR_RING || shape == EYE_SHAPE_STAR_SOLID) {
        d.w = STAR_EYE_D;
        d.h = STAR_EYE_D;
        d.gap = STAR_GAP;
    }
    return d;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int eye_h_for(int which, int base_h)
{
    int h = (int)((float)base_h * eye_open_for(which) + 0.5f);
    if (h < BLINK_MIN_H) h = BLINK_MIN_H;
    return h;
}

// Straight lid across a round eye. Everything on the "above" side of the line
// is erased, so the eye keeps its circular outline and closes like an eyelid
// instead of squashing into an ellipse.
static void apply_lid(int cx, int cy, int r, float t, int angle_deg)
{
    if (t <= 0.0f) return;

    float a  = (float)angle_deg * (PI_F / 180.0f);
    float sa = sinf(a);
    float ca = cosf(a);
    // `t` is the fraction of the eye the lid covers, so the line sits at
    // 2t-1 radii down from the centre: t=0 is the top edge, t=1 the bottom.
    float s = (2.0f * t - 1.0f) * (float)r;

    // Erase the whole bounding square above the lid, not just the inside of
    // the circle: the outline is rasterised with rounding that pushes a few
    // pixels outside the mathematical circle, and those survive a clipped
    // erase as stray slivers along the top of the eye.
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (-sa * (float)x + ca * (float)y < s)
                sh1106_set_pixel(cx + x, cy + y, false);
        }
    }
}

static void apply_lid_bottom(int cx, int cy, int r, float t)
{
    if (t <= 0.0f) return;

    float s = (1.0f - 2.0f * t) * (float)r;   // line this far above the bottom

    for (int y = -r; y <= r; y++) {
        if ((float)y <= s) continue;
        for (int x = -r; x <= r; x++)
            sh1106_set_pixel(cx + x, cy + y, false);
    }
}

// The chord where the lid meets the eyeball. Only the outlined eye needs it:
// on a filled eye the white/black boundary already draws the lid.
static void lid_chord(int cx, int cy, int r, float t, int angle_deg)
{
    float a  = (float)angle_deg * (PI_F / 180.0f);
    float sa = sinf(a);
    float ca = cosf(a);
    float s  = (2.0f * t - 1.0f) * (float)r;

    for (int x = -r; x <= r; x++) {
        // solve -sa*x + ca*y = s for y
        if (fabsf(ca) < 1e-4f) continue;
        float y = (s + sa * (float)x) / ca;
        if (y < -(float)r || y > (float)r) continue;
        if ((float)x * (float)x + y * y > (float)(r * r)) continue;
        sh1106_set_pixel(cx + x, cy + (int)(y + (y < 0 ? -0.5f : 0.5f)), true);
    }
}

// Round face: a fixed circle, a pupil that travels, and a lid that closes.
static void draw_eye_round(int cx, int cy, int eye, float open)
{
    int r = STAR_EYE_D / 2;
    int thick = STAR_RING;
    bool solid = (s_cfg.shape == EYE_SHAPE_STAR_SOLID);

    if (solid) {
        fill_ellipse_ex(cx, cy, r, r, true);
    } else {
        // Row-scan ring: both edges rounded the same way, so the outline is
        // continuous instead of notched where the two approximations disagree.
        int ri = r - thick;
        for (int dy = -r; dy <= r; dy++) {
            float ty = (float)dy / (float)r;
            float out = 1.0f - ty * ty;
            if (out < 0.0f) continue;
            int xo = (int)((float)r * sqrtf(out) + 0.5f);

            float tyi = ri > 0 ? (float)dy / (float)ri : 2.0f;
            float in = 1.0f - tyi * tyi;
            int xi = (ri > 0 && in > 0.0f) ? (int)((float)ri * sqrtf(in) + 0.5f) : -1;

            for (int x = xi + 1; x <= xo; x++) {
                sh1106_set_pixel(cx + x, cy + dy, true);
                if (x != 0) sh1106_set_pixel(cx - x, cy + dy, true);
            }
        }
    }

    int base_r = (s_cfg.pupil == PUPIL_ASTERISK) ? ASTERISK_R
               : (s_cfg.pupil == PUPIL_SCRIBBLE) ? SCRIBBLE_R
               : (s_cfg.pupil == PUPIL_DOT)      ? DOT_R
               : (s_cfg.pupil == PUPIL_BLOB)     ? BLOB_CORE_R
               : STAR_R;

    // How far the lids close this frame: a blink and the mood's resting lid
    // combine, whichever is lower wins.
    float t = 1.0f - open;
    float mood_t = (float)s_cfg.lid_pct / 100.0f;
    if (mood_t > t) t = mood_t;

    float b = (float)s_cfg.lid_bottom_pct / 100.0f;
    if (b > 0.45f) b = 0.45f;

    int max_x = clamp_int(r - base_r - thick - 1, 0, r);
    int max_y = clamp_int(r - base_r - thick - 1, 0, r);
    int px = clamp_int((int)(s_look_x * s_cfg.gaze_amp * STAR_PUPIL_GAIN), -max_x, max_x);
    int py = clamp_int((int)(s_look_y * s_cfg.gaze_amp * STAR_PUPIL_GAIN), -max_y, max_y);

    // A closing lid carries the pupil with it: the pupil stays inside the
    // sliver the lids leave open, otherwise it gets sliced into a black bite.
    {
        int y_top = (int)((2.0f * t - 1.0f) * (float)r);      // relative to cy
        int y_bot = (int)((1.0f - 2.0f * b) * (float)r);

        int lowest  = y_top + base_r + 1;
        int highest = y_bot - base_r - 1;
        if (lowest > highest) lowest = highest;

        if (py < lowest)  py = lowest;
        if (py > highest) py = highest;
        if (py > max_y)   py = max_y;
        if (py < -max_y)  py = -max_y;
    }

    // A closing eye does not keep a pupil: without this the black disc sits
    // inside the last sliver of sclera and the blink ends in two white wings.
    bool pupil_visible = (t - mood_t) < 0.55f;

    switch (s_cfg.pupil) {
    case PUPIL_ASTERISK:
        if (pupil_visible) draw_asterisk(cx + px, cy + py, base_r, !solid);
        break;
    case PUPIL_SCRIBBLE:
        if (pupil_visible) draw_scribble(cx + px, cy + py, eye, 100, !solid);
        break;
    case PUPIL_DOT:
        if (pupil_visible) draw_dot(cx + px, cy + py, base_r, !solid);
        break;
    case PUPIL_BLOB:
        if (pupil_visible) draw_blob(cx + px, cy + py, eye, 100, !solid);
        break;
    default:
        if (pupil_visible) draw_star(cx + px, cy + py, base_r, !solid);
        break;
    }

    // Mirror the tilt on the right eye, so an annoyed face is symmetric.
    int angle = (eye == 0) ? s_cfg.lid_angle : -s_cfg.lid_angle;
    apply_lid(cx, cy, r, t, angle);
    apply_lid_bottom(cx, cy, r, b);

    if (!solid) {
        if (t > 0.0f) lid_chord(cx, cy, r, t, angle);
        if (b > 0.0f) lid_chord(cx, cy, r, 1.0f - b, 0);
    }
}

// One brow: a shallow arc, two pixels thick. The thickness is drawn as a
// vertical span per column rather than two offset lines, otherwise the arc
// steps between rows and the brow comes out dashed.
static void draw_brow(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0;
    int n  = dx < 0 ? -dx : dx;
    if (n == 0) {
        sh1106_set_pixel(x0, y0, true);
        sh1106_set_pixel(x0, y0 - 1, true);
        return;
    }

    for (int i = 0; i <= n; i++) {
        // Straight, not arched: a 1 px bow is invisible at this resolution but
        // it does eat into the thickness and makes the brow read as dashed.
        float t = (float)i / (float)n;
        int x = x0 + (dx * i) / n;
        int y = (int)lrintf((float)y0 + (float)(y1 - y0) * t);

        // Exactly two rows per column. Bridging the step as well would leave a
        // one-pixel bump at every stair, which is what makes a two-pixel brow
        // look like a thick bar with a thin line under it.
        sh1106_set_pixel(x, y, true);
        sh1106_set_pixel(x, y + 1, true);
    }
}

static void draw_brows(int cx_l, int cx_r, int cy, int r)
{
    if (s_cfg.brow_len <= 0) return;

    int half = (r * s_cfg.brow_len) / 100;
    int top  = cy - r - s_cfg.brow_lift;

    if (s_cfg.brow_mono) {
        // One line across both eyes, dipping at the bridge: the outer ends sit
        // high, the middle drops by `brow_tilt`. That V is the angry face —
        // a flat monobrow reads as puzzled, not as furious.
        int x0 = cx_l - half, x1 = cx_r + half;
        int mid = (cx_l + cx_r) / 2;
        draw_brow(x0, top, mid, top + s_cfg.brow_tilt);
        draw_brow(mid, top + s_cfg.brow_tilt, x1, top);
        return;
    }

    // Left brow: outer end on the left, inner end (towards the nose) on the
    // right. The right brow mirrors it.
    draw_brow(cx_l - half, top, cx_l + half, top + s_cfg.brow_tilt);
    draw_brow(cx_r + half, top, cx_r - half, top + s_cfg.brow_tilt);
}

// One eye, at its own height, in whichever silhouette the mood selected.
static void draw_eye(int cx, int cy, int w, int h, int eye, float open)
{
    if (star_style()) {
        draw_eye_round(cx, cy, eye, open);
        return;
    }

    (void)eye;
    (void)open;

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
    s_last_px = s_last_py = -999;
    s_last_open_l = s_last_open_r = -1;
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

void eyes_glance(float x, float y)
{
    s_look_x_t = x;
    s_look_y_t = y;
    s_next_saccade = s_now + 3000;
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
    eye_dims_t d = dims_for(s_cfg.shape);
    bool round = star_style();

    float open_l = eye_open_for(0);
    float open_r = eye_open_for(1);

    // The capsule face changes its height to blink; the round face keeps a
    // fixed circle and closes a lid over it instead, so its bounds never move.
    int h_l = round ? d.h : eye_h_for(0, d.h);
    int h_r = round ? d.h : eye_h_for(1, d.h);
    int h   = h_l > h_r ? h_l : h_r;
    int w = d.w;

    // Slow vertical bob: ~1.5 px of travel with a 4 s period, so the face
    // never looks frozen even when the gaze is still.
    int drift = (int)(1.5f * sinf((float)s_now * 0.0016f));

    // On the round face the rings stay put and only the pupils travel, so the
    // gaze must not move the eye centres.
    int gx = round ? 0 : (int)(s_look_x * s_cfg.gaze_amp);
    int gy = round ? 0 : (int)(s_look_y * s_cfg.gaze_amp);

    int cx_l = (SH1106_WIDTH / 2) - (d.gap / 2) - (w / 2) + gx;
    int cx_r = (SH1106_WIDTH / 2) + (d.gap / 2) + (w / 2) + gx;
    int cy   = (round ? ROUND_EYE_CY : EYE_CY) + gy + drift;

    int pupil_x = 0, pupil_y = 0;
    if (round) {
        pupil_x = (int)(s_look_x * s_cfg.gaze_amp * STAR_PUPIL_GAIN);
        pupil_y = (int)(s_look_y * s_cfg.gaze_amp * STAR_PUPIL_GAIN);
    }

    // Quantised so a sub-pixel drift does not count as a change.
    int open_l_q = (int)(open_l * 100.0f);
    int open_r_q = (int)(open_r * 100.0f);

    // Nothing moved since the last frame: the framebuffer already holds this
    // exact image, so skip both the redraw and the I2C transfer. Most frames
    // between blinks and saccades land here, which keeps the bus quiet.
    if (s_prev.w > 0 && h_l == s_last_h_l && h_r == s_last_h_r &&
        cx_l == s_last_cx && cy == s_last_cy &&
        pupil_x == s_last_px && pupil_y == s_last_py &&
        open_l_q == s_last_open_l && open_r_q == s_last_open_r &&
        (int)s_cfg.shape == s_last_shape) {
        eyes_rect_t none = { 0, 0, 0, 0 };
        return none;
    }

    // Brows live above the eyeballs, so the dirty rect has to reach up to
    // them. Missing that band is not a cosmetic bug: the brow is painted into
    // the framebuffer but never flushed, and when it is finally sent the old
    // brow from the previous mood is still on the panel, underneath it.
    int brow_h = 0;
    if (round && s_cfg.brow_len > 0)
        brow_h = (s_cfg.brow_lift > 0 ? s_cfg.brow_lift : 0) + 1;

    int x0 = cx_l - w / 2;
    int x1 = cx_r + w / 2;
    int y0 = cy - h / 2 - brow_h;
    int y1 = cy + h / 2;

    // Rows 0..6 belong to the text banner. The eye rect drifted up to row 5 on
    // the surprised face (brows 6 px up) and wiped the bottom of the letters.
    if (y0 < EYES_TOP_ROW) y0 = EYES_TOP_ROW;

    // Clear the previous frame's area first: the eyes move and shrink, so the
    // old pixels must go.
    if (s_prev.w > 0 && s_prev.h > 0) {
        for (int y = s_prev.y; y < s_prev.y + s_prev.h; y++)
            for (int x = s_prev.x; x < s_prev.x + s_prev.w; x++)
                sh1106_set_pixel(x, y, false);
    }

    draw_eye(cx_l, cy, w, h_l, 0, open_l);
    draw_eye(cx_r, cy, w, h_r, 1, open_r);

    if (round) draw_brows(cx_l, cx_r, cy, w / 2);

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
    s_last_px = pupil_x;
    s_last_py = pupil_y;
    s_last_open_l = open_l_q;
    s_last_open_r = open_r_q;
    s_last_shape = (int)s_cfg.shape;
    return r;
}
