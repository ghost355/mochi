/*
 * mochi — pomodoro timer.
 *
 * Three taps put the robot to work: it narrows its eyes, a progress bar crawls
 * along the bottom of the screen, and when the time is up it celebrates. The
 * timer itself is four numbers; the mood machine and the renderer do the rest.
 */

#ifndef MOCHI_FOCUS_H
#define MOCHI_FOCUS_H

#include <stdbool.h>
#include <stdint.h>

// Twenty-five minutes, the usual pomodoro. Short enough to sit through, long
// enough that the bar visibly moves.
#define FOCUS_DEFAULT_MS (25u * 60u * 1000u)

void focus_start(uint32_t now_ms);
void focus_start_for(uint32_t now_ms, uint32_t duration_ms);
void focus_stop(void);

bool focus_running(void);

// 0..1000, so the caller can draw a bar without touching floats.
int focus_progress_permille(uint32_t now_ms);

// True exactly once, on the first update after the timer ran out.
bool focus_take_finished(uint32_t now_ms);

#endif // MOCHI_FOCUS_H
