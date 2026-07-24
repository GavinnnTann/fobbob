#pragma once
#include <stdint.h>

enum class ButtonEvent : uint8_t {
    NONE,
    BTN1_PRESS,      // SW_Up (P3): one step on press, then auto-repeats while held
    BTN2_PRESS,      // SW_Down (P5): one step on press, then auto-repeats while held
    BTN_PWR_PRESS,   // SW_Power (P4) single tap
    CHORD_HELD,      // both BTN1+BTN2 held >= BTN_CHORD_MS
};

// Call once in setup() after hardware_init().
void buttons_init();

// Call every loop iteration. Returns pending event or NONE.
ButtonEvent buttons_tick();

// Returns true if both buttons are currently held (raw, no debounce).
bool buttons_chord_active();

// Read the raw TCA6408A input port byte. Used by boot_determine_mode() to
// identify which signal (Touch_INT, IMU_INT1, etc.) caused a GPIO45 wakeup.
// Returns 0xFF on I²C error.
uint8_t buttons_read_port();
