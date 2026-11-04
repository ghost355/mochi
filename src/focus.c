/*
 * mochi — pomodoro timer (see focus.h).
 */

#include "focus.h"

static bool     s_running;
static bool     s_finished;
static uint32_t s_start;
static uint32_t s_duration;

void focus_start_for(uint32_t now_ms, uint32_t duration_ms)
{
    if (duration_ms < 1000u) duration_ms = 1000u;

    s_running  = true;
    s_finished = false;
    s_start    = now_ms;
    s_duration = duration_ms;
}

void focus_start(uint32_t now_ms)
{
    focus_start_for(now_ms, FOCUS_DEFAULT_MS);
}

void focus_stop(void)
{
    s_running  = false;
    s_finished = false;
}

bool focus_running(void)
{
    return s_running;
}

int focus_progress_permille(uint32_t now_ms)
{
    if (!s_running) return 0;

    uint32_t elapsed = now_ms - s_start;
    if (elapsed >= s_duration) return 1000;

    // 64-bit on purpose: 25 minutes in milliseconds times a thousand overflows
    // 32 bits, and RP2350 has a hardware divider anyway.
    return (int)(((uint64_t)elapsed * 1000u) / s_duration);
}

bool focus_take_finished(uint32_t now_ms)
{
    if (!s_running || s_finished) return false;
    if ((uint32_t)(now_ms - s_start) < s_duration) return false;

    s_running  = false;
    s_finished = true;
    return true;
}
