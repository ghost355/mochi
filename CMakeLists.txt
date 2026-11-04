cmake_minimum_required(VERSION 3.13)

# Custom board: must be set before the SDK import so the SDK picks it up.
set(PICO_BOARD waveshare_rp2350_usb_a)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include(pico_sdk_import.cmake)

project(mochi C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

pico_sdk_init()

add_executable(mochi
    src/main.c
    src/eyes.c
    src/mood.c
    src/focus.c
    src/touch.c
    src/buzzer.c
    src/display/sh1106.c
    src/display/font5x7.c
)

target_include_directories(mochi PRIVATE src)

target_link_libraries(mochi
    pico_stdlib
    hardware_i2c
    hardware_gpio
    hardware_pwm
    hardware_clocks
    m
)

# USB CDC + UART0 debug console (GP0/GP1).
pico_enable_stdio_usb(mochi 1)
pico_enable_stdio_uart(mochi 1)

pico_add_extra_outputs(mochi)

# Release-friendly warnings; no -Werror so SDK headers can't break the build.
target_compile_options(mochi PRIVATE -O2 -Wall -Wextra)
