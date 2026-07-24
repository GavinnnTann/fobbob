#include "ble_provision.h"
#include "nvs/nvs_manager.h"
#include "config.h"
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_random.h>
#include <esp_mac.h>
#include <string.h>
#include <time.h>

// ── UUIDs ─────────────────────────────────────────────────────────────────
#define UUID_SVC         BLE_SERVICE_UUID
#define UUID_PIN_VERIFY  "12345678-1234-1234-1234-123456789001"
#define UUID_DEVICE_INFO "12345678-1234-1234-1234-123456789002"
#define UUID_WIFI_CREDS  "12345678-1234-1234-1234-123456789003"
#define UUID_TOTP_SEC    "12345678-1234-1234-1234-123456789004"
#define UUID_NTP_TIME    "12345678-1234-1234-1234-123456789005"
#define UUID_FP_COMMAND  "12345678-1234-1234-1234-123456789006"
#define UUID_FP_STATUS   "12345678-1234-1234-1234-123456789007"
#define UUID_PROV_DONE   "12345678-1234-1234-1234-123456789008"
#define UUID_TOTP_READ   "12345678-1234-1234-1234-123456789009"
#define UUID_DEV_NAME      "12345678-1234-1234-1234-12345678900a"
#define UUID_FP_NAMES      "12345678-1234-1234-1234-12345678900b"
#define UUID_DISP_TIMEOUT  "12345678-1234-1234-1234-12345678900c"
#define UUID_FACTORY_RESET "12345678-1234-1234-1234-12345678900d"
#define UUID_IMU_SETTINGS  "12345678-1234-1234-1234-12345678900e"

struct BleEvent {
    BleProvEvent type;
};

// Static payload buffers — written by BLE callbacks, read by ble_provision_tick().
// One write at a time is safe because the webapp serialises steps.
static char     s_wifi_payload[256]     = {};
static char     s_totp_payload[2048]    = {};
static uint16_t s_disp_sec_payload      = TOTP_DISPLAY_SEC_DEFAULT;
static char s_ntp_payload[24]       = {};
static char s_fp_cmd_payload[256]   = {};
static char    s_fp_names_payload[512]            = {};
static uint8_t s_fp_named_slots[FP_MAX_TEMPLATES] = {};  // sensor page IDs from last FP_NAMES write
static uint8_t s_fp_named_count                   = 0;
static char    s_dev_name_payload[64]             = {};
static ImuSettings s_imu_settings_payload         = {};
static char s_mac_suffix[5]         = {};   // 4 hex chars, e.g. "A3F2"

static QueueHandle_t          s_evt_queue      = nullptr;
static NimBLECharacteristic*  s_fp_status_char = nullptr;
static bool                   s_ble_radio      = false;  // NimBLE actually initialised
static bool                   s_pin_verified   = false;
static bool                   s_connected      = false;
static bool                   s_pin_locked     = false;  // true after PIN_MAX_ATTEMPTS failures
static uint8_t                s_pin_attempts   = 0;
static uint32_t               s_pin            = 0;

static constexpr uint8_t PIN_MAX_ATTEMPTS = 3;

static void pin_lockout() {
    s_pin_locked = true;
    NimBLEDevice::stopAdvertising();
    NimBLEServer* srv = NimBLEDevice::getServer();
    if (srv) {
        for (auto handle : srv->getPeerDevices())
            srv->disconnect(handle);
    }
#ifdef DEBUG_SERIAL
    Serial.printf("[BLE] PIN locked after %d failed attempts — reboot to unlock\n", PIN_MAX_ATTEMPTS);
#endif
}

// ── Provisioning helpers ───────────────────────────────────────────────────
// Free functions owning the PIN check, the DEVICE_INFO / TOTP_READ JSON builders,
// and the staging payload buffers, called from the NimBLE characteristic callbacks.

// Verify a candidate PIN against the session PIN. Updates s_pin_verified and the
// attempt counter; returns true on match.
static bool do_check_pin(const char* val, size_t len) {
    if (s_pin_locked) return false;
    if (len != BLE_PIN_LENGTH) return false;
    char pin_str[8];
    snprintf(pin_str, sizeof(pin_str), "%06lu", (unsigned long)s_pin);
    if (strncmp(val, pin_str, BLE_PIN_LENGTH) == 0) {
        s_pin_verified = true;
        s_pin_attempts = 0;
        return true;
    }
    s_pin_verified = false;
    s_pin_attempts++;
    return false;
}

static void build_device_info_json(char* out, size_t out_sz) {
    StaticJsonDocument<512> doc;
    doc["provisioned"]  = nvs_is_provisioned();
    doc["fp_count"]     = nvs_get_fp_count();
    doc["totp_count"]   = nvs_get_totp_count();
    doc["mac_suffix"]   = s_mac_suffix;
    doc["pin_verified"] = s_pin_verified;
    char dev_name[64] = {};
    if (nvs_get_device_name(dev_name, sizeof(dev_name)))
        doc["device_name"] = dev_name;
    uint8_t fp_cnt = nvs_get_fp_count();
    if (fp_cnt > 0) {
        JsonArray fp_arr = doc.createNestedArray("fp_names");
        FpFinger finger;
        for (uint8_t i = 0; i < fp_cnt; i++) {
            JsonObject f = fp_arr.createNestedObject();
            f["slot_id"] = i;
            if (nvs_get_fp_finger(i, &finger)) {
                f["name"] = finger.name;
            } else {
                char def[12];
                snprintf(def, sizeof(def), "Finger %d", i + 1);
                f["name"] = def;
            }
        }
    }
    serializeJson(doc, out, out_sz);
}

// ── Server callbacks (NimBLE v1.4.x signatures) ───────────────────────────
class ServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*) override {
        s_connected    = true;
        s_pin_verified = false;
#ifdef DEBUG_SERIAL
        Serial.println("[BLE] Client connected");
#endif
        BleEvent ev = { BleProvEvent::CLIENT_CONNECTED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
    void onDisconnect(NimBLEServer*) override {
        s_connected    = false;
        s_pin_verified = false;
#ifdef DEBUG_SERIAL
        Serial.println("[BLE] Client disconnected");
#endif
        BleEvent ev = { BleProvEvent::CLIENT_DISCONNECTED };
        xQueueSend(s_evt_queue, &ev, 0);
        if (!s_pin_locked) NimBLEDevice::startAdvertising();
    }
};

// ── Characteristic callbacks (NimBLE v1.4.x signatures) ──────────────────
class PinVerifyCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (s_pin_locked) return;
        std::string val = c->getValue();
        if (do_check_pin(val.c_str(), val.length())) {
#ifdef DEBUG_SERIAL
            Serial.println("[BLE] PIN verified");
#endif
        } else {
#ifdef DEBUG_SERIAL
            Serial.printf("[BLE] Wrong PIN (%u/%u)\n", s_pin_attempts, PIN_MAX_ATTEMPTS);
#endif
            // Lockout is BLE-specific (stops advertising + disconnects).
            if (s_pin_attempts >= PIN_MAX_ATTEMPTS) pin_lockout();
        }
    }
};

class DeviceInfoCB : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        char buf[512];
        build_device_info_json(buf, sizeof(buf));
        c->setValue((uint8_t*)buf, strlen(buf));
    }
};

class WifiCredsCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        strncpy(s_wifi_payload, val.c_str(), sizeof(s_wifi_payload) - 1);
        s_wifi_payload[sizeof(s_wifi_payload) - 1] = '\0';
        BleEvent ev = { BleProvEvent::WIFI_CREDS_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

class TotpSecretsCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        strncpy(s_totp_payload, val.c_str(), sizeof(s_totp_payload) - 1);
        s_totp_payload[sizeof(s_totp_payload) - 1] = '\0';
        BleEvent ev = { BleProvEvent::TOTP_SECRETS_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

class NtpTimeCB : public NimBLECharacteristicCallbacks {
    const bool m_require_pin;
public:
    explicit NtpTimeCB(bool require_pin) : m_require_pin(require_pin) {}
    void onWrite(NimBLECharacteristic* c) override {
        if (m_require_pin && !s_pin_verified) return;
        std::string val = c->getValue();
        strncpy(s_ntp_payload, val.c_str(), sizeof(s_ntp_payload) - 1);
        s_ntp_payload[sizeof(s_ntp_payload) - 1] = '\0';
        BleEvent ev = { BleProvEvent::NTP_TIME_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

class FpCommandCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        strncpy(s_fp_cmd_payload, val.c_str(), sizeof(s_fp_cmd_payload) - 1);
        s_fp_cmd_payload[sizeof(s_fp_cmd_payload) - 1] = '\0';
        BleEvent ev = { BleProvEvent::FP_COMMAND_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

class ProvDoneCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        StaticJsonDocument<64> doc;
        if (deserializeJson(doc, val.c_str()) != DeserializationError::Ok) return;
        if (!doc["confirm"].as<bool>()) return;
        BleEvent ev = { BleProvEvent::PROVISION_DONE };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

class DevNameCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        strncpy(s_dev_name_payload, val.c_str(), sizeof(s_dev_name_payload) - 1);
        s_dev_name_payload[sizeof(s_dev_name_payload) - 1] = '\0';
        BleEvent ev = { BleProvEvent::DEVICE_NAME_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

class FpNamesCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        strncpy(s_fp_names_payload, val.c_str(), sizeof(s_fp_names_payload) - 1);
        s_fp_names_payload[sizeof(s_fp_names_payload) - 1] = '\0';
        BleEvent ev = { BleProvEvent::FP_NAMES_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

class DisplayTimeoutCB : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", nvs_get_display_sec());
        c->setValue((uint8_t*)buf, strlen(buf));
    }
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        uint16_t sec = (uint16_t)strtoul(val.c_str(), nullptr, 10);
        if (sec < TOTP_DISPLAY_SEC_MIN) sec = TOTP_DISPLAY_SEC_MIN;
        if (sec > TOTP_DISPLAY_SEC_MAX) sec = TOTP_DISPLAY_SEC_MAX;
        s_disp_sec_payload = sec;
        BleEvent ev = { BleProvEvent::DISPLAY_TIMEOUT_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

// ── Factory reset characteristic ──────────────────────────────────────────
// PIN-gated AND requires the literal confirmation string {"confirm":"WIPE ALL"}
// so an accidental or malformed write can never trigger a wipe. The actual
// erase (NVS + sensor templates) happens in main.cpp on FACTORY_RESET_REQUESTED.
class FactoryResetCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        StaticJsonDocument<64> doc;
        if (deserializeJson(doc, val.c_str()) != DeserializationError::Ok) return;
        if (strcmp(doc["confirm"] | "", "WIPE ALL") != 0) return;
        BleEvent ev = { BleProvEvent::FACTORY_RESET_REQUESTED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

// ── TOTP read characteristic ──────────────────────────────────────────────
// WARNING: secrets transit over unencrypted BLE (link encryption reverted —
// see note at PROP_*_SEC). Mitigated by the PIN gate; use in a trusted
// environment and keep the provisioning window short.
static char s_totp_read_buf[1024];

// Build the TOTP_READ JSON (the stored accounts). Returns "[]" if the PIN has not
// been verified. Shared by the BLE read callback and the REST /api/totp-read.
static size_t build_totp_read_json(char* out, size_t out_sz) {
    if (!s_pin_verified) {
        strncpy(out, "[]", out_sz);
        return strlen(out);
    }
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();
    uint8_t count = nvs_get_totp_count();
    TotpAccount acct;
    for (uint8_t i = 0; i < count; i++) {
        if (nvs_get_totp_account(i, &acct)) {
            JsonObject obj = arr.createNestedObject();
            obj["name"]       = acct.name;
            obj["secret_b32"] = acct.secret_b32;
        }
    }
    return serializeJson(doc, out, out_sz);
}

class TotpReadCB : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        size_t len = build_totp_read_json(s_totp_read_buf, sizeof(s_totp_read_buf));
        c->setValue((uint8_t*)s_totp_read_buf, len);
    }
};

class ImuSettingsCB : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) override {
        if (!s_pin_verified) { c->setValue("{}"); return; }
        ImuSettings cur = nvs_get_imu_settings();
        StaticJsonDocument<128> doc;
        doc["tap_sleep"]        = cur.tap_sleep;
        doc["adaptive_timeout"] = cur.adaptive_timeout;
        doc["orient_flip"]      = cur.orient_flip;
        char buf[128];
        serializeJson(doc, buf, sizeof(buf));
        c->setValue((uint8_t*)buf, strlen(buf));
    }
    void onWrite(NimBLECharacteristic* c) override {
        if (!s_pin_verified) return;
        std::string val = c->getValue();
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, val.c_str()) != DeserializationError::Ok) return;
        s_imu_settings_payload.tap_sleep        = doc["tap_sleep"]        | false;
        s_imu_settings_payload.adaptive_timeout = doc["adaptive_timeout"] | false;
        s_imu_settings_payload.orient_flip      = doc["orient_flip"]      | false;
        BleEvent ev = { BleProvEvent::IMU_SETTINGS_RECEIVED };
        xQueueSend(s_evt_queue, &ev, 0);
    }
};

// ── Public API ─────────────────────────────────────────────────────────────
static void init_mac_suffix() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_mac_suffix, sizeof(s_mac_suffix), "%02X%02X", mac[4], mac[5]);
}

// NOTE — BLE link encryption was implemented (Just Works + bonding + ENC perms)
// and REVERTED 2026-06: Chrome's Windows WinRT backend neither auto-pairs on
// ATT insufficient-encryption nor completes pairing on a peripheral SMP
// Security Request (auth always finished encrypted:0 and the link dropped).
// Until a workable pairing path exists (native app / Capacitor, or manual OS
// pre-pairing), characteristics are plain READ/WRITE and the app-layer PIN is
// the only transport gate.
static constexpr uint32_t PROP_WRITE_SEC = NIMBLE_PROPERTY::WRITE;
static constexpr uint32_t PROP_READ_SEC  = NIMBLE_PROPERTY::READ;

static void ble_provision_begin_shared() {
    // Initialise the provisioning state: event queue, session PIN, and MAC suffix.
    if (!s_evt_queue) s_evt_queue = xQueueCreate(8, sizeof(BleEvent));
    s_pin          = esp_random() % 1000000;
    s_pin_verified = false;
    s_pin_attempts = 0;
    s_pin_locked   = false;
    init_mac_suffix();
}

uint32_t ble_provision_start() {
    ble_provision_begin_shared();

    char adv_name[16];
    snprintf(adv_name, sizeof(adv_name), "%s-%s", BLE_DEVICE_NAME, s_mac_suffix);
    NimBLEDevice::init(adv_name);
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCB());

    NimBLEService* svc = server->createService(UUID_SVC);

    auto* pin_char = svc->createCharacteristic(UUID_PIN_VERIFY, PROP_WRITE_SEC);
    pin_char->setCallbacks(new PinVerifyCB());

    auto* info_char = svc->createCharacteristic(UUID_DEVICE_INFO, PROP_READ_SEC);
    info_char->setCallbacks(new DeviceInfoCB());

    auto* wifi_char = svc->createCharacteristic(UUID_WIFI_CREDS, PROP_WRITE_SEC);
    wifi_char->setCallbacks(new WifiCredsCB());

    auto* totp_char = svc->createCharacteristic(UUID_TOTP_SEC, PROP_WRITE_SEC);
    totp_char->setCallbacks(new TotpSecretsCB());

    auto* ntp_char = svc->createCharacteristic(UUID_NTP_TIME, PROP_WRITE_SEC);
    ntp_char->setCallbacks(new NtpTimeCB(true));   // PIN required during provisioning

    auto* fp_cmd_char = svc->createCharacteristic(UUID_FP_COMMAND, PROP_WRITE_SEC);
    fp_cmd_char->setCallbacks(new FpCommandCB());

    s_fp_status_char = svc->createCharacteristic(UUID_FP_STATUS,
                            NIMBLE_PROPERTY::NOTIFY | PROP_READ_SEC);

    auto* done_char = svc->createCharacteristic(UUID_PROV_DONE, PROP_WRITE_SEC);
    done_char->setCallbacks(new ProvDoneCB());

    auto* totp_read_char = svc->createCharacteristic(UUID_TOTP_READ, PROP_READ_SEC);
    totp_read_char->setCallbacks(new TotpReadCB());

    auto* dev_name_char = svc->createCharacteristic(UUID_DEV_NAME, PROP_WRITE_SEC);
    dev_name_char->setCallbacks(new DevNameCB());

    auto* fp_names_char = svc->createCharacteristic(UUID_FP_NAMES, PROP_WRITE_SEC);
    fp_names_char->setCallbacks(new FpNamesCB());

    auto* disp_char = svc->createCharacteristic(UUID_DISP_TIMEOUT,
                          PROP_READ_SEC | PROP_WRITE_SEC);
    disp_char->setCallbacks(new DisplayTimeoutCB());

    auto* reset_char = svc->createCharacteristic(UUID_FACTORY_RESET, PROP_WRITE_SEC);
    reset_char->setCallbacks(new FactoryResetCB());

    auto* imu_char = svc->createCharacteristic(UUID_IMU_SETTINGS,
                         PROP_READ_SEC | PROP_WRITE_SEC);
    imu_char->setCallbacks(new ImuSettingsCB());

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(UUID_SVC);
    adv->start();
    s_ble_radio = true;

#ifdef DEBUG_SERIAL
    Serial.printf("[BLE] Advertising as '%s-%s', PIN: %06lu\n",
                  BLE_DEVICE_NAME, s_mac_suffix, (unsigned long)s_pin);
#endif

    return s_pin;
}

void ble_provision_stop() {
    // Only tear down NimBLE if it was actually started — the WiFi-AP path uses the
    // shared queue/PIN without ever initialising the BLE radio.
    if (s_ble_radio) {
        NimBLEDevice::stopAdvertising();
        NimBLEDevice::deinit(true);
        s_ble_radio = false;
    }
    if (s_evt_queue) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = nullptr;
    }
    s_fp_status_char = nullptr;
    s_connected      = false;
}

BleProvEvent ble_provision_tick() {
    if (!s_evt_queue) return BleProvEvent::NONE;

    BleEvent ev;
    if (xQueueReceive(s_evt_queue, &ev, 0) != pdTRUE) return BleProvEvent::NONE;

    switch (ev.type) {
        case BleProvEvent::WIFI_CREDS_RECEIVED: {
            StaticJsonDocument<256> doc;
            if (deserializeJson(doc, s_wifi_payload) == DeserializationError::Ok) {
                nvs_stage_wifi(doc["ssid"] | "", doc["password"] | "");
#ifdef DEBUG_SERIAL
                Serial.printf("[BLE] WiFi creds staged: ssid='%s'\n", doc["ssid"] | "");
#endif
            }
            break;
        }
        case BleProvEvent::TOTP_SECRETS_RECEIVED: {
            DynamicJsonDocument doc(4096);
            if (deserializeJson(doc, s_totp_payload) == DeserializationError::Ok) {
                JsonArray arr = doc.as<JsonArray>();
                uint8_t count = 0;
                TotpAccount accounts[TOTP_MAX_ACCOUNTS];
                for (JsonObject entry : arr) {
                    if (count >= TOTP_MAX_ACCOUNTS) break;
                    strncpy(accounts[count].name,       entry["name"]       | "", 63);
                    strncpy(accounts[count].secret_b32, entry["secret_b32"] | "", 127);
                    accounts[count].name[63]        = '\0';
                    accounts[count].secret_b32[127] = '\0';
                    count++;
                }
                nvs_stage_totp(accounts, count);
            }
            break;
        }
        /*case BleProvEvent::NTP_TIME_RECEIVED: {
            uint64_t unix_ms = (uint64_t)strtoull(s_ntp_payload, nullptr, 10);
            if (unix_ms > 0) wifi_set_time_from_ms(unix_ms);
            break;
        }*/
        case BleProvEvent::FP_COMMAND_RECEIVED:
            break;
        case BleProvEvent::FP_NAMES_RECEIVED: {
            DynamicJsonDocument doc(512);
            s_fp_named_count = 0;
            if (deserializeJson(doc, s_fp_names_payload) == DeserializationError::Ok) {
                JsonArray arr = doc.as<JsonArray>();
                uint8_t count = 0;
                for (JsonObject entry : arr) {
                    if (count >= FP_MAX_TEMPLATES) break;
                    uint8_t sensorSlot = entry["slot_id"] | count;
                    nvs_stage_fp_finger(count, sensorSlot, entry["name"] | "Finger");
                    s_fp_named_slots[count] = sensorSlot;
                    count++;
                }
                s_fp_named_count = count;
                nvs_stage_fp_count(count);
#ifdef DEBUG_SERIAL
                Serial.printf("[BLE] FP names staged: %d finger(s)\n", count);
                for (uint8_t i = 0; i < count; i++)
                    Serial.printf("[BLE]   NVS[%d] -> sensor page %d\n", i, s_fp_named_slots[i]);
#endif
            }
            break;
        }
        case BleProvEvent::DEVICE_NAME_RECEIVED:
            if (s_dev_name_payload[0] != '\0') {
                nvs_stage_device_name(s_dev_name_payload);
#ifdef DEBUG_SERIAL
                Serial.printf("[BLE] Device name staged: '%s'\n", s_dev_name_payload);
#endif
            }
            break;
        case BleProvEvent::DISPLAY_TIMEOUT_RECEIVED:
            nvs_stage_display_sec(s_disp_sec_payload);
#ifdef DEBUG_SERIAL
            Serial.printf("[BLE] Display timeout staged: %us\n", s_disp_sec_payload);
#endif
            break;
        case BleProvEvent::IMU_SETTINGS_RECEIVED:
            nvs_stage_imu_settings(s_imu_settings_payload);
#ifdef DEBUG_SERIAL
            Serial.printf("[BLE] IMU settings staged: tap=%d adaptive=%d flip=%d\n",
                s_imu_settings_payload.tap_sleep,
                s_imu_settings_payload.adaptive_timeout, s_imu_settings_payload.orient_flip);
#endif
            break;
        case BleProvEvent::PROVISION_DONE:
            nvs_stage_mark_valid();
            break;
        default:
            break;
    }

    return ev.type;
}

// Latest FP status, cached for the BLE FP_STATUS notify characteristic.
static char s_fp_status_json[64] = "{\"status\":\"IDLE\",\"fp_id\":0}";

void ble_provision_notify_fp_status(const char* status, uint8_t fp_id) {
    StaticJsonDocument<64> doc;
    doc["status"] = status;
    doc["fp_id"]  = fp_id;
    serializeJson(doc, s_fp_status_json, sizeof(s_fp_status_json));
    // BLE notify only when a client is actually connected.
    if (s_fp_status_char && s_connected) {
        s_fp_status_char->setValue((uint8_t*)s_fp_status_json, strlen(s_fp_status_json));
        s_fp_status_char->notify();
    }
}

bool ble_provision_clear_bonds() {
    if (!NimBLEDevice::getInitialized()) return false;
    NimBLEDevice::deleteAllBonds();
    return true;
}

bool ble_provision_connected() { return s_connected; }
uint32_t ble_provision_get_pin() { return s_pin; }
const char* ble_provision_get_fp_cmd()      { return s_fp_cmd_payload; }
const char* ble_provision_get_ntp_payload() { return s_ntp_payload; }
uint16_t    ble_provision_get_display_sec()  { return s_disp_sec_payload; }
ImuSettings ble_provision_get_imu_settings() { return s_imu_settings_payload; }

uint8_t ble_provision_get_fp_slots(uint8_t out[FP_MAX_TEMPLATES]) {
    for (uint8_t i = 0; i < s_fp_named_count; i++) out[i] = s_fp_named_slots[i];
    return s_fp_named_count;
}

void ble_timesync_start() {
    s_evt_queue    = xQueueCreate(4, sizeof(BleEvent));
    s_connected    = false;
    s_pin_verified = false;

    init_mac_suffix();
    char adv_name[16];
    snprintf(adv_name, sizeof(adv_name), "%s-%s", BLE_DEVICE_NAME, s_mac_suffix);
    NimBLEDevice::init(adv_name);
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCB());

    NimBLEService* svc = server->createService(UUID_SVC);

    auto* ntp_char = svc->createCharacteristic(UUID_NTP_TIME, PROP_WRITE_SEC);
    ntp_char->setCallbacks(new NtpTimeCB(false));  // no PIN — timesync only

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(UUID_SVC);
    adv->start();
    s_ble_radio = true;

#ifdef DEBUG_SERIAL
    Serial.printf("[BLE] Time sync window open (%ds)\n", TIMESYNC_WINDOW_MS / 1000);
#endif
}
