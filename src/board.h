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

#endif // MOCHI_BOARD_H
