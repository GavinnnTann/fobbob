#pragma once
#include <stdint.h>
#include <stdbool.h>

// CST816S capacitive touch driver.
// I²C address 0x15, shares SDA/SCL with RTC + TCA6408.
// RST = GPIO0, INT = TCA6408 P0 (unused — see note in .cpp).
//
// This driver exposes a single continuous contact point rather than the chip's
// canned gesture codes. All higher-level gestures (tap / swipe / long-press) and
// drag-to-scroll are derived in main.cpp from this one transformed point, so they
// can never disagree about orientation.

// Initialise the CST816S: hardware reset, verify chip ID, enable continuous
// coordinate reporting. Returns false if the chip does not ACK on I²C.
bool touch_init();

// Read the current contact. Returns true if a finger is down; *x and *y are set
// to the contact point already transformed into the on-screen frame (0..239).
// Returns false (and leaves *x/*y untouched) when no finger is present.
bool touch_read_point(int* x, int* y);

// Inform the driver of the current display flip so it mirrors coordinates to
// match what the user sees. The panel is mounted 180° (MADCTL 0xC8) in the
// normal orientation, so the un-flipped state mirrors; the flipped state does not.
void touch_set_flipped(bool flipped);
