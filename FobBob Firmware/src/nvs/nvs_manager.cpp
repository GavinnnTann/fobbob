#include "nvs_manager.h"
#include "config.h"
#include <Preferences.h>

static Preferences s_live;
static Preferences s_staging;
static Preferences s_rtdata;  // runtime data — never cleared on provision, survives power cycles

bool nvs_init() {
    bool live_ok    = s_live.begin("config",   false);
    bool staging_ok = s_staging.begin("staging", false);
    bool rtdata_ok  = s_rtdata.begin("rtdata",   false);
    return live_ok && staging_ok && rtdata_ok;
}

bool nvs_is_provisioned() {
    return s_live.getBool("provisioned", false);
}

bool nvs_get_wifi(char* ssid, size_t ssid_len, char* pass, size_t pass_len) {
    if (!ssid || !pass) return false;
    s_live.getString("wifi_ssid", ssid, ssid_len);
    s_live.getString("wifi_pass", pass, pass_len);
    return ssid[0] != '\0';
}

uint8_t nvs_get_totp_count() {
    return s_live.getUChar("totp_count", 0);
}

bool nvs_get_totp_account(uint8_t index, TotpAccount* out) {
    if (!out || index >= TOTP_MAX_ACCOUNTS) return false;
    char key_name[16];
    char key_sec[16];
    snprintf(key_name, sizeof(key_name), "totp_%d_name", index);
    snprintf(key_sec,  sizeof(key_sec),  "totp_%d_sec",  index);
    s_live.getString(key_name, out->name,       sizeof(out->name));
    s_live.getString(key_sec,  out->secret_b32, sizeof(out->secret_b32));
    return out->name[0] != '\0';
}

uint8_t nvs_get_fp_count() {
    return s_live.getUChar("fp_count", 0);
}

bool nvs_get_fp_finger(uint8_t index, FpFinger* out) {
    if (!out || index >= FP_MAX_TEMPLATES) return false;
    char key[16];
    snprintf(key, sizeof(key), "fp_%d_name", index);
    s_live.getString(key, out->name, sizeof(out->name));
    out->slot_id = index;
    return out->name[0] != '\0';
}

void nvs_stage_wifi(const char* ssid, const char* pass) {
    s_staging.putString("wifi_ssid", ssid);
    s_staging.putString("wifi_pass", pass);
    s_staging.putBool("wifi_staged", true);
}

void nvs_stage_totp(const TotpAccount* accounts, uint8_t count) {
    if (count > TOTP_MAX_ACCOUNTS) count = TOTP_MAX_ACCOUNTS;
    s_staging.putUChar("totp_count", count);
    char key_name[16];
    char key_sec[16];
    for (uint8_t i = 0; i < count; i++) {
        snprintf(key_name, sizeof(key_name), "totp_%d_name", i);
        snprintf(key_sec,  sizeof(key_sec),  "totp_%d_sec",  i);
        s_staging.putString(key_name, accounts[i].name);
        s_staging.putString(key_sec,  accounts[i].secret_b32);
    }
    s_staging.putBool("totp_staged", true);
}

void nvs_stage_fp_count(uint8_t count) {
    s_staging.putUChar("fp_count", count);
    s_staging.putBool("fp_staged", true);
}

void nvs_stage_fp_finger(uint8_t index, uint8_t slot_id, const char* name) {
    if (index >= FP_MAX_TEMPLATES) return;
    char key[16];
    snprintf(key, sizeof(key), "fp_%d_name", index);
    s_staging.putString(key, name ? name : "");
    s_staging.putBool("fp_staged", true);
}

void nvs_stage_device_name(const char* name) {
    s_staging.putString("dev_name", name);
    s_staging.putBool("name_staged", true);
}

void nvs_stage_display_sec(uint16_t sec) {
    if (sec < TOTP_DISPLAY_SEC_MIN) sec = TOTP_DISPLAY_SEC_MIN;
    if (sec > TOTP_DISPLAY_SEC_MAX) sec = TOTP_DISPLAY_SEC_MAX;
    s_staging.putUShort("disp_sec", sec);
    s_staging.putBool("disp_staged", true);
}

bool nvs_get_device_name(char* out, size_t len) {
    if (!s_live.isKey("dev_name")) return false;
    s_live.getString("dev_name", out, len);
    return out[0] != '\0';
}

uint16_t nvs_get_display_sec() {
    return s_live.getUShort("disp_sec", TOTP_DISPLAY_SEC_DEFAULT);
}

ImuSettings nvs_get_imu_settings() {
    ImuSettings cfg = {};
    cfg.tap_sleep        = s_live.getBool("imu_tap",    false);
    cfg.adaptive_timeout = s_live.getBool("imu_adapt",  false);
    cfg.orient_flip      = s_live.getBool("imu_flip",   false);
    return cfg;
}

void nvs_stage_imu_settings(const ImuSettings& cfg) {
    s_staging.putBool("imu_tap",   cfg.tap_sleep);
    s_staging.putBool("imu_adapt", cfg.adaptive_timeout);
    s_staging.putBool("imu_flip",  cfg.orient_flip);
    s_staging.putBool("imu_staged", true);
}

void nvs_save_imu_settings(const ImuSettings& cfg) {
    s_live.putBool("imu_tap",   cfg.tap_sleep);
    s_live.putBool("imu_adapt", cfg.adaptive_timeout);
    s_live.putBool("imu_flip",  cfg.orient_flip);
}

void nvs_save_display_sec(uint16_t sec) {
    if (sec < TOTP_DISPLAY_SEC_MIN) sec = TOTP_DISPLAY_SEC_MIN;
    if (sec > TOTP_DISPLAY_SEC_MAX) sec = TOTP_DISPLAY_SEC_MAX;
    s_live.putUShort("disp_sec", sec);
}

bool nvs_get_haptic_enabled() {
    return s_live.getBool("haptic", true);   // default on
}

void nvs_save_haptic_enabled(bool en) {
    s_live.putBool("haptic", en);
}

bool nvs_get_wifi_enabled() {
    return s_live.getBool("wifi_enabled", false);   // default off
}

void nvs_save_wifi_enabled(bool en) {
    s_live.putBool("wifi_enabled", en);
}

bool nvs_wifi_ready() {
    if (!s_live.getBool("wifi_enabled", false)) return false;
    char ssid[64] = {};
    s_live.getString("wifi_ssid", ssid, sizeof(ssid));
    return ssid[0] != '\0';
}

void nvs_stage_mark_valid() {
    s_staging.putBool("staged_valid", true);
}

bool nvs_commit_staging() {
    if (!s_staging.getBool("staged_valid", false)) return false;

    char key_name[16], key_sec[16], val[128];

    // Only overwrite sections that were explicitly staged — leaves the rest intact.
    if (s_staging.getBool("wifi_staged", false)) {
        char ssid[64] = {}, pass[64] = {};
        s_staging.getString("wifi_ssid", ssid, sizeof(ssid));
        s_staging.getString("wifi_pass", pass, sizeof(pass));
        s_live.putString("wifi_ssid", ssid);
        s_live.putString("wifi_pass", pass);
        // Providing credentials during provisioning enables WiFi by default; an
        // empty SSID (user cleared it) disables it. The on-device toggle can flip
        // this later without touching the stored credentials.
        s_live.putBool("wifi_enabled", ssid[0] != '\0');
    }

    if (s_staging.getBool("totp_staged", false)) {
        uint8_t new_count = s_staging.getUChar("totp_count", 0);
        uint8_t old_count = s_live.getUChar("totp_count", 0);

        // Remove keys that no longer exist (account list shrank)
        for (uint8_t i = new_count; i < old_count; i++) {
            snprintf(key_name, sizeof(key_name), "totp_%d_name", i);
            snprintf(key_sec,  sizeof(key_sec),  "totp_%d_sec",  i);
            s_live.remove(key_name);
            s_live.remove(key_sec);
        }

        for (uint8_t i = 0; i < new_count; i++) {
            snprintf(key_name, sizeof(key_name), "totp_%d_name", i);
            snprintf(key_sec,  sizeof(key_sec),  "totp_%d_sec",  i);
            s_staging.getString(key_name, val, sizeof(val));
            s_live.putString(key_name, val);
            s_staging.getString(key_sec, val, sizeof(val));
            s_live.putString(key_sec, val);
        }
        s_live.putUChar("totp_count", new_count);
    }

    if (s_staging.getBool("fp_staged", false)) {
        uint8_t new_fp_count = s_staging.getUChar("fp_count", 0);
        uint8_t old_fp_count = s_live.getUChar("fp_count", 0);
        char fp_key[16], fp_name[32];

        // Remove name keys for fingers no longer enrolled
        for (uint8_t i = new_fp_count; i < old_fp_count; i++) {
            snprintf(fp_key, sizeof(fp_key), "fp_%d_name", i);
            s_live.remove(fp_key);
        }
        // Copy staged names (only those explicitly written)
        for (uint8_t i = 0; i < new_fp_count; i++) {
            snprintf(fp_key, sizeof(fp_key), "fp_%d_name", i);
            if (s_staging.isKey(fp_key)) {
                s_staging.getString(fp_key, fp_name, sizeof(fp_name));
                s_live.putString(fp_key, fp_name);
            }
        }
        s_live.putUChar("fp_count", new_fp_count);
    }

    if (s_staging.getBool("name_staged", false)) {
        char name[64] = {};
        s_staging.getString("dev_name", name, sizeof(name));
        s_live.putString("dev_name", name);
#ifdef DEBUG_SERIAL
        Serial.printf("[NVS] Device name committed: '%s'\n", name);
#endif
    }

    if (s_staging.getBool("disp_staged", false)) {
        uint16_t sec = s_staging.getUShort("disp_sec", TOTP_DISPLAY_SEC_DEFAULT);
        s_live.putUShort("disp_sec", sec);
#ifdef DEBUG_SERIAL
        Serial.printf("[NVS] Display timeout committed: %us\n", sec);
#endif
    }

    if (s_staging.getBool("imu_staged", false)) {
        s_live.putBool("imu_tap",   s_staging.getBool("imu_tap",   false));
        s_live.putBool("imu_adapt", s_staging.getBool("imu_adapt", false));
        s_live.putBool("imu_flip",  s_staging.getBool("imu_flip",  false));
#ifdef DEBUG_SERIAL
        Serial.println("[NVS] IMU settings committed");
#endif
    }

    s_live.putBool("provisioned", true);
    s_staging.clear();
    return true;
}

void nvs_wipe_all() {
    s_live.clear();
    s_staging.clear();
    // s_rtdata intentionally kept — time is independent of provisioning state
}

void nvs_save_time(time_t t) {
    s_rtdata.putLong64("last_time", (int64_t)t);
}

time_t nvs_load_time() {
    return (time_t)s_rtdata.getLong64("last_time", 0);
}

void nvs_set_last_ntp_sync(time_t t) {
    s_rtdata.putLong64("last_ntp", (int64_t)t);
}

time_t nvs_get_last_ntp_sync() {
    return (time_t)s_rtdata.getLong64("last_ntp", 0);
}

void nvs_request_reprovision() {
    s_rtdata.putBool("reprov_req", true);
}

bool nvs_consume_reprovision_request() {
    bool req = s_rtdata.getBool("reprov_req", false);
    if (req) s_rtdata.remove("reprov_req");
    return req;
}
