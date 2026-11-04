/*
 * mochi — non-blocking tone player for a passive piezo buzzer.
 *
 * The buzzer is driven straight from a PWM slice: a piezo disc is a capacitor,
 * so the pin only has to charge and discharge it and the current stays in the
 * low milliamps. No transistor, no flyback diode — that would only be needed
 * for a magnetic (coil) buzzer.
 *
 * Nothing here blocks: buzzer_update() is called once per frame and advances
 * the melody, so the eye animation never stutters while a sound plays.
 */

#ifndef MOCHI_BUZZER_H
#define MOCHI_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t freq;   // Hz, 0 = rest
    uint16_t ms;     // duration
} buzzer_note_t;

typedef struct {
    const buzzer_note_t *notes;
    uint8_t              count;
} buzzer_melody_t;

void buzzer_init(void);

// Advance the currently playing melody. Call once per frame.
void buzzer_update(uint32_t now_ms);

// Start a melody. Replaces whatever is playing.
void buzzer_play(const buzzer_melody_t *melody);

// Hold a single tone for `ms` (used for the long press drone).
void buzzer_tone(uint16_t freq, uint32_t ms);

// R2-D2 style chatter: 5-7 off-scale blips, one of four variants picked at
// random. Interrupts whatever is playing.
void buzzer_chirp(uint32_t now_ms);

// Rick-style belch: a burst of low pulses whose duty cycle swings 20-60 %
// on every pulse. Interrupts whatever is playing.
void buzzer_burp(uint32_t now_ms);

// A belch that hands over to a melody when it ends — the goodnight phrase
// follows the burp instead of cutting it off.
void buzzer_burp_then(uint32_t now_ms, const buzzer_melody_t *then);

void buzzer_stop(void);
bool buzzer_busy(void);

extern const buzzer_melody_t buzzer_startup;
extern const buzzer_melody_t buzzer_tap;
extern const buzzer_melody_t buzzer_release;
extern const buzzer_melody_t buzzer_sad;
extern const buzzer_melody_t buzzer_yawn;
extern const buzzer_melody_t buzzer_sleep;

#endif // MOCHI_BUZZER_H
