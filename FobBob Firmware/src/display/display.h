#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Initialise I80 bus, GC9A01 panel, LVGL, and backlight PWM. Call once in setup().
bool display_init();

// Drive LVGL tick + timer handler. Call every loop() iteration.
void display_tick();

// Backlight control (ledc channel 0).
void display_on();                      // restore BACKLIGHT_DUTY
void display_off();                     // duty 0, LVGL keeps running
void display_set_brightness(uint8_t duty);  // 0–255

// Send GC9A01 SLPIN (0x10) then blank backlight. Call before deep sleep so the
// panel's internal regulators shut down and eliminate the faint glow on dark screens.
// Safe to call even if display_init() was never called (guards nullptr internally).
void display_sleep();

// Returns the LVGL display handle — use this to create screens.
lv_display_t* display_get();

// Rotate the LVGL display. degrees must be 0 or 180.
// Redraws the current screen; safe to call from the main task at any time.
void display_set_rotation(int degrees);
