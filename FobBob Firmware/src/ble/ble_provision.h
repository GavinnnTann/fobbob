#pragma once
#include <stdint.h>
#include <stddef.h>   // size_t — used by the reuse hooks below
#include "imu/imu_manager.h"

enum class BleProvEvent : uint8_t {
    NONE,
    WIFI_CREDS_RECEIVED,
    TOTP_SECRETS_RECEIVED,
    NTP_TIME_RECEIVED,
    FP_COMMAND_RECEIVED,
    FP_NAMES_RECEIVED,
    DEVICE_NAME_RECEIVED,
    DISPLAY_TIMEOUT_RECEIVED,
    IMU_SETTINGS_RECEIVED,
    FACTORY_RESET_REQUESTED,
    PROVISION_DONE,
    CLIENT_CONNECTED,
    CLIENT_DISCONNECTED,
};

// Start BLE GATT server. Generates and logs the 6-digit PIN.
// Returns the PIN so the caller can display it.
uint32_t ble_provision_start();

// Stop BLE GATT server and release resources.
void ble_provision_stop();

// Call every loop iteration. Processes the internal event queue.
// Returns the next pending event or NONE.
BleProvEvent ble_provision_tick();

// Send a fingerprint status notification to connected client.
// status: one of "PLACE_FINGER", "LIFT_FINGER", "PLACE_AGAIN",
//         "ENROLLED_OK", "ENROLL_FAILED", "MATCH_OK", "MATCH_FAILED"
void ble_provision_notify_fp_status(const char* status, uint8_t fp_id = 0);

// Returns true if a client is currently connected.
bool ble_provision_connected();

// Returns the generated PIN (only valid after ble_provision_start()).
uint32_t ble_provision_get_pin();

// Returns the raw JSON payload of the last FP_COMMAND_RECEIVED event.
// Valid until the next FP_COMMAND_RECEIVED.
const char* ble_provision_get_fp_cmd();

// Returns the raw payload of the last NTP_TIME_RECEIVED event (unix ms as decimal string).
// Valid until the next NTP_TIME_RECEIVED.
const char* ble_provision_get_ntp_payload();

// Returns the display timeout (seconds) from the last DISPLAY_TIMEOUT_RECEIVED event.
uint16_t ble_provision_get_display_sec();

// Returns the IMU settings from the last IMU_SETTINGS_RECEIVED event.
ImuSettings ble_provision_get_imu_settings();

// Returns the sensor slot IDs from the last FP_NAMES_RECEIVED event.
// out[] is filled with up to FP_MAX_TEMPLATES values; returns the count.
// Used by main.cpp to reconcile the sensor on PROVISION_DONE.
#include "config.h"
uint8_t ble_provision_get_fp_slots(uint8_t out[FP_MAX_TEMPLATES]);

// Delete all stored BLE bonds (pairing keys) on the device. Requires the BLE
// stack to be active (provisioning or timesync mode); returns false otherwise.
// Use when a peer OS holds a stale bond and pairing fails repeatedly.
bool ble_provision_clear_bonds();

// Start a minimal BLE window for time sync only — no PIN required.
// Exposes only NTP_TIME. Call ble_provision_tick() to process events,
// ble_provision_stop() to close early. Use with TIMESYNC_WINDOW_MS.
void ble_timesync_start();
