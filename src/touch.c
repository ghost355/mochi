/*
 * mochi — TTP223 touch input (see touch.h).
 */

#include "touch.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "board.h"

static touch_ctx_t s_ctx;

void touch_init(void)
{
    gpio_init(PIN_TOUCH);
    gpio_set_dir(PIN_TOUCH, GPIO_IN);
    // The common 3-pin TTP223 breakout drives OUT push-pull, so the pull-down
    // is only a safety net for open-drain variants and for the moment before
    // the module finishes its own power-on calibration.
    gpio_pull_down(PIN_TOUCH);
}

touch_state_t touch_sample(touch_ctx_t *ctx, bool raw, uint32_t now_ms)
{
    touch_state_t out = { 0 };

    if (raw != ctx->candidate) {
        // Level changed: start waiting. Any earlier candidate is dropped, so a
        // burst of chatter restarts the window instead of passing through.
        ctx->candidate = raw;
        ctx->candidate_since = now_ms;
    } else if (ctx->candidate != ctx->stable &&
               (uint32_t)(now_ms - ctx->candidate_since) >= TOUCH_DEBOUNCE_MS) {
        ctx->stable = ctx->candidate;

        if (ctx->stable) {
            out.just_pressed = true;
            ctx->pressed_since = now_ms;
            ctx->have_press_time = true;
        } else {
            out.just_released = true;
            ctx->have_press_time = false;
        }
    }

    out.pressed = ctx->stable;
    out.held_ms = (ctx->stable && ctx->have_press_time)
                      ? (uint32_t)(now_ms - ctx->pressed_since)
                      : 0;
    return out;
}

touch_state_t touch_update(uint32_t now_ms)
{
    return touch_sample(&s_ctx, gpio_get(PIN_TOUCH), now_ms);
}
