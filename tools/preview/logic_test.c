/*
 * logic_test — the mood ladder and the pomodoro, on the host.
 *
 * These two modules are pure bookkeeping: no pixels, no hardware. Testing them
 * by flashing a board and counting taps by hand is slow and unreliable, so the
 * gesture table and the timer are exercised here instead.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "buzzer.h"
#include "focus.h"
#include "mood.h"

// --- buzzer stubs ---------------------------------------------------------
// mood.c reacts to gestures with sounds; the sounds are not under test.

void buzzer_play(const buzzer_melody_t *m) { (void)m; }
void buzzer_tone(uint16_t hz, uint32_t ms) { (void)hz; (void)ms; }
void buzzer_stop(void) {}
bool buzzer_busy(void) { return false; }
void buzzer_chirp(uint32_t now_ms) { (void)now_ms; }
void buzzer_burp(uint32_t now_ms) { (void)now_ms; }

// mood.c plays two melodies by name, so the tables have to exist to link.
const buzzer_melody_t buzzer_sad = { NULL, 0 };

// --- helpers --------------------------------------------------------------

static int g_fail;

static void expect(const char *what, mood_t got, mood_t want)
{
    if (got == want) return;
    printf("  FAIL %s: got %s, want %s\n", what,
           mood_params(got)->name, mood_params(want)->name);
    g_fail++;
}

static mood_t settle(uint32_t now)
{
    mood_t m = MOOD_COUNT;
    mood_update(now, &m);
    return m;
}

// Tap `n` times 100 ms apart, starting at `t`.
static mood_t taps(uint32_t t, int n)
{
    for (int i = 0; i < n; i++) mood_tap(t + (uint32_t)i * 100u);
    return settle(t + (uint32_t)n * 100u);
}

int main(void)
{
    mood_init(0);
    memset(&(mood_t){0}, 0, 0);   // keep -Wunused happy on old compilers

    printf("gestures\n");
    expect("one tap",     taps(1000, 1),  MOOD_CURIOUS);
    expect("two taps",    taps(9000, 2),  MOOD_HAPPY);
    expect("three taps",  taps(20000, 3), MOOD_FOCUS);
    expect("four taps",   taps(40000, 4), MOOD_ANNOYED);
    expect("five taps",   taps(60000, 5), MOOD_PLEADING);
    expect("six taps",    taps(80000, 6), MOOD_PANIC);

    printf("timed moods fall back to idle\n");
    mood_init(100000);
    taps(100000, 1);
    expect("curious expires", settle(103000), MOOD_IDLE);
    taps(110000, 2);
    expect("happy expires",   settle(115000), MOOD_IDLE);
    taps(120000, 6);
    expect("panic expires",   settle(124000), MOOD_IDLE);

    printf("sleep\n");
    mood_init(200000);
    settle(200000);
    expect("still awake", settle(200000 + SLEEP_AFTER_MS - 1000), MOOD_IDLE);
    expect("asleep",      settle(200000 + SLEEP_AFTER_MS + 1000), MOOD_SLEEPY);

    printf("focus\n");
    mood_init(300000);
    mood_focus_enter(300000);
    expect("focus face", settle(300000), MOOD_FOCUS);

    focus_start_for(300000, 10000);
    if (!focus_running()) { printf("  FAIL focus did not start\n"); g_fail++; }
    if (focus_progress_permille(300000) != 0)   { printf("  FAIL progress at start\n"); g_fail++; }
    if (focus_progress_permille(305000) != 500) { printf("  FAIL progress at half\n");  g_fail++; }
    if (focus_take_finished(305000))            { printf("  FAIL finished early\n");    g_fail++; }
    if (!focus_take_finished(310000))           { printf("  FAIL finished late\n");     g_fail++; }
    if (focus_take_finished(311000))            { printf("  FAIL finished twice\n");    g_fail++; }
    if (focus_running())                        { printf("  FAIL still running\n");     g_fail++; }

    mood_focus_leave(311000);
    expect("focus leaves", settle(311000), MOOD_IDLE);

    // A running timer must not let the robot fall asleep: focus mode is work.
    mood_focus_enter(400000);
    focus_start_for(400000, 3600000u);
    for (uint32_t t = 400000; t < 400000 + SLEEP_AFTER_MS * 2u; t += 1000) {
        mood_keep_awake(t);
        mood_t m = settle(t);
        if (m == MOOD_SLEEPY) { printf("  FAIL fell asleep during focus\n"); g_fail++; break; }
    }
    focus_stop();

    if (g_fail == 0) printf("all gesture and timer checks passed\n");
    return g_fail ? 1 : 0;
}
