#pragma once
#include <stdint.h>

// Enter deep sleep immediately. Configures gpio_wakeup then calls esp_deep_sleep_start().
void power_go_to_sleep();

// Call every loop iteration to check for display timeout.
// Enters deep sleep if no activity for DISPLAY_TIMEOUT_MS.
void power_tick();

// Reset the idle timer (call when user activity is detected).
void power_reset_idle_timer();

// Store the configured display timeout so power_go_to_sleep() and power_tick()
// use the NVS-backed value instead of the compile-time default.
// Call once in setup() after loading NVS settings.
void power_set_config(uint16_t display_sec);
