#pragma once
#include <stdint.h>
#include <stdbool.h>

// QMI8658C driver — accelerometer features only (gyro disabled for power savings).

// ── Init ──────────────────────────────────────────────────────────────────────
// Full init: configure accel ODR/scale, tap detection, WoM interrupt.
// Returns false if WHO_AM_I doesn't match — sensor absent or bus fault.
bool imu_init();

// Configure which features are active. Call after imu_init() and after
// loading ImuSettings from NVS. Also re-applies on every BLE PROVISION_DONE.
struct ImuSettings {
    bool tap_sleep;        // double-tap the capacitive SCREEN → sleep (not an IMU feature)
    bool adaptive_timeout; // motion resets the display idle timer
    bool orient_flip;      // upside-down → LVGL 180° rotation
};
void imu_configure(const ImuSettings& cfg);

// Put sensor into low-power mode before deep sleep (accel low-power ODR).
void imu_sleep_mode();

// ── Runtime polling (call from main loop while display is active) ─────────────
// Read raw accel values (LSB; 1 g ≈ 4096 LSB at ±8 g full scale).
// Returns false on I²C error.
bool imu_read_accel(int16_t* ax, int16_t* ay, int16_t* az);

// Returns true if the magnitude delta since the last call exceeds the
// motion threshold — use to reset the display idle timer.
bool imu_is_moving();

// Returns true if the device appears upside-down (X-axis > 500 LSB — gravity on +X when held portrait).
// Pass the current flip state so it can be held when the device is tilted sideways (|ax| ≤ 2000),
// preventing spurious un-flips while the screen is upside-down and the user tilts left/right.
bool imu_is_flipped(bool current_state);
