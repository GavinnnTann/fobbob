#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "imu/imu_manager.h"

struct TotpAccount {
    char name[64];
    char secret_b32[128];
};

struct FpFinger {
    char    name[32];
    uint8_t slot_id;
};

bool     nvs_init();
bool     nvs_is_provisioned();

// Live config reads (called on every boot after provisioning)
bool     nvs_get_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len);
uint8_t  nvs_get_totp_count();
bool     nvs_get_totp_account(uint8_t index, TotpAccount* out);
uint8_t  nvs_get_fp_count();
bool     nvs_get_fp_finger(uint8_t index, FpFinger* out);
bool     nvs_get_device_name(char* out, size_t len);
uint16_t nvs_get_display_sec();   // TOTP screen timeout in seconds; default TOTP_DISPLAY_SEC_DEFAULT
ImuSettings nvs_get_imu_settings(); // all fields default false
bool     nvs_get_haptic_enabled(); // vibration-motor feedback master switch; default true

// WiFi master switch — independent of whether wifi_ssid/wifi_pass exist. Toggling
// this off must NOT erase the stored credentials. Default false (off).
bool     nvs_get_wifi_enabled();
// True when both an SSID is stored AND WiFi is enabled — i.e. background sync may run.
bool     nvs_wifi_ready();

// Live writes — bypass staging for settings changed on-device (touch settings page).
// Write directly to the committed "config" namespace; take effect on next read.
void     nvs_save_imu_settings(const ImuSettings& cfg);
void     nvs_save_display_sec(uint16_t sec);  // clamps to [MIN, MAX]
void     nvs_save_haptic_enabled(bool en);
void     nvs_save_wifi_enabled(bool en);      // flips the toggle only; keeps credentials

// Staging writes (called during provisioning flow)
void     nvs_stage_wifi(const char* ssid, const char* pass);
void     nvs_stage_totp(const TotpAccount* accounts, uint8_t count);
void     nvs_stage_fp_count(uint8_t count);
void     nvs_stage_fp_finger(uint8_t index, uint8_t slot_id, const char* name);
void     nvs_stage_device_name(const char* name);
void     nvs_stage_display_sec(uint16_t sec);  // clamps to [MIN, MAX]
void     nvs_stage_imu_settings(const ImuSettings& cfg);
void     nvs_stage_mark_valid();

// Atomic commit — copies staging → config, clears staging, returns false if staging invalid
bool     nvs_commit_staging();

// Wipe everything (factory reset)
void     nvs_wipe_all();

// Persist NTP-synced unix time to NVS so it survives power cycles and crash resets.
// Falls back to this if RTC memory is cleared.
void     nvs_save_time(time_t t);
time_t   nvs_load_time();

// Timestamp (unix seconds) of the last successful WiFi/NTP sync. Stored in
// "rtdata" so it survives all resets. 0 = never synced.
void     nvs_set_last_ntp_sync(time_t t);
time_t   nvs_get_last_ntp_sync();

// Signal that the next boot should enter BLE_REPROVISION without wiping NVS.
// Call nvs_consume_reprovision_request() in boot_determine_mode() to read+clear.
void     nvs_request_reprovision();
bool     nvs_consume_reprovision_request();
