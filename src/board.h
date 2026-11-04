/*
 * mochi — central pin map for the Waveshare RP2350-USB-A build.
 *
 * Board: Waveshare RP2350-USB-A (RP2350A, dual Cortex-M33 @150MHz,
 *        520KB SRAM, 2MB QSPI flash, 15 exposed GPIO: GP0-GP10, GP26-GP29).
 *
 * Wiring:
 *   - OLED 1.3" 128x64 SH1106 (I2C, addr 0x3C): SDA=GP4, SCL=GP5, VCC=3V3, GND
 *   - TTP223 touch OUT = GP2 (reserved, not used in M0)
 *   - passive buzzer +  = GP3 (PWM slice 1B, reserved, not used in M0)
 *
 * GP0 (UART0 TX) / GP1 (UART0 RX) are the debug UART — left at the SDK
 * board defaults (PICO_DEFAULT_UART 0, TX=0, RX=1) and used by
 * pico_enable_stdio_uart.
 */

#ifndef MOCHI_BOARD_H
#define MOCHI_BOARD_H

// --- I2C0: OLED ---
#define PIN_I2C_SDA   4
#define PIN_I2C_SCL   5
#define OLED_I2C_ADDR 0x3C    // 7-bit address of the SH1106
#define I2C_BAUD      400000  // 400 kHz

// --- Reserved pins (not used in M0) ---
#define PIN_TOUCH     2       // TTP223 touch sensor output
#define PIN_BUZZER    3       // passive buzzer + (PWM slice 1B)

// --- Eye style ------------------------------------------------------------
// Which face the mood engine draws. The moods still drive openness, blink
// rate and gaze; this only picks the silhouette.
//
//   0  capsule bars   — the original face, cheapest on I2C
//   1  round ring     — big circles drawn as outlines, star pupil inside
//   2  round filled   — big circles filled, pupil cut out as a hole
//
// In the round styles the rings stay put and only the pupils travel, which
// costs a lot less bus time than moving the whole eye.
#define EYES_STYLE       2

// Pupil shape for the round styles:
//   0 = four-point sparkle   1 = symmetric asterisk
//   2 = hand-drawn asterisk  3 = plain dot  4 = blob with spikes
#define EYES_PUPIL       4

#endif // MOCHI_BOARD_H
