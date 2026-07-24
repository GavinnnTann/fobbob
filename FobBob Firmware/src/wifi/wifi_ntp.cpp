#include "wifi_ntp.h"
#include "config.h"
#include "nvs/nvs_manager.h"
#include "rtc/rtc_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <esp_sntp.h>   // sntp_set_time_sync_notification_cb — confirm a REAL sync
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Keep the WiFi/NTP task off core 0 (NimBLE host) when the timesync BLE window
// might still be open — mirrors the fingerprint task's core choice in main.cpp.
static constexpr BaseType_t WIFI_TASK_CORE = 1;

static volatile bool         s_busy      = false;   // a sync task is currently running
static volatile bool         s_sync_done = false;   // set on success, consumed by the UI once
static volatile WifiNtpState s_state     = WifiNtpState::IDLE;

// Set true by the SNTP stack when an actual time packet lands. This is the ONLY
// reliable "the clock was really updated" signal — checking time()>epoch is not,
// because the stale RTC clock already reads post-2023, so the old code declared
// success instantly and wrote the WRONG time back (codes never matched).
static volatile bool s_ntp_packet = false;
static void on_ntp_sync(struct timeval*) { s_ntp_packet = true; }

bool wifi_connect_blocking(uint32_t timeout_ms) {
    char ssid[64] = {}, pass[64] = {};
    if (!nvs_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) return false;  // no SSID

    WiFi.persistent(false);          // don't thrash NVS with the WiFi driver's own copy
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if ((millis() - start) >= timeout_ms) {
#ifdef DEBUG_SERIAL
            Serial.println("[WIFI] connect timeout");
#endif
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#ifdef DEBUG_SERIAL
    Serial.printf("[WIFI] connected, ip=%s\n", WiFi.localIP().toString().c_str());
#endif
    return true;
}

static void wifi_radio_off() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void wifi_ntp_radio_off() { wifi_radio_off(); }

bool wifi_ntp_sync_blocking() {
    s_state = WifiNtpState::CONNECTING;
    if (!wifi_connect_blocking(WIFI_CONNECT_TIMEOUT_MS)) {
        s_state = WifiNtpState::CONNECT_FAIL;
        wifi_radio_off();
        return false;
    }

    s_state = WifiNtpState::SYNCING;

    // UTC; the device keeps all time in UTC (see rtc_manager). Register the sync
    // callback BEFORE configTime so we never miss the first packet.
    s_ntp_packet = false;
    sntp_set_time_sync_notification_cb(on_ntp_sync);
    configTime(0, 0, "pool.ntp.org", "time.google.com");

    // Wait for an ACTUAL packet (s_ntp_packet), not merely a plausible clock.
    uint32_t start = millis();
    while (!s_ntp_packet && (millis() - start) < NTP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    time_t now = time(nullptr);
    bool ok = s_ntp_packet && now > NTP_SANITY_EPOCH;
    if (ok) {
        rtc_write(now);                 // refresh the crystal-backed RTC
        nvs_save_time(now);             // bound rollback on a future power loss
        nvs_set_last_ntp_sync(now);     // remember when we last synced
        s_state = WifiNtpState::OK;
#ifdef DEBUG_SERIAL
        Serial.printf("[WIFI] NTP sync OK: unix=%lld\n", (long long)now);
#endif
    } else {
        s_state = WifiNtpState::NTP_FAIL;   // connected but no NTP packet in time
#ifdef DEBUG_SERIAL
        Serial.println("[WIFI] NTP sync failed (no packet)");
#endif
    }

    wifi_radio_off();
    return ok;
}

static void wifi_ntp_task(void*) {
    bool ok = wifi_ntp_sync_blocking();
    if (ok) s_sync_done = true;
    s_busy = false;
    vTaskDelete(nullptr);
}

WifiNtpState wifi_ntp_state() { return s_state; }

void wifi_ntp_request_sync(bool force) {
    if (s_busy) return;
    if (!nvs_wifi_ready()) return;      // disabled or no credentials

    if (!force) {
        time_t now  = time(nullptr);
        time_t last = nvs_get_last_ntp_sync();
        // Skip if we synced recently. If the clock looks unset (now < sanity), sync.
        if (now > NTP_SANITY_EPOCH && last > 0 &&
            (now - last) < NTP_SYNC_INTERVAL_SEC) {
            return;
        }
    }

    s_busy  = true;
    s_state = WifiNtpState::CONNECTING;   // reflect immediately in the settings UI
    if (xTaskCreatePinnedToCore(wifi_ntp_task, "ntp_sync", 4096, nullptr, 1,
                                nullptr, WIFI_TASK_CORE) != pdPASS) {
        s_busy  = false;   // task didn't spawn — stay idle rather than wedged
        s_state = WifiNtpState::IDLE;
    }
}

bool wifi_ntp_busy() { return s_busy; }

bool wifi_ntp_consume_sync_done() {
    if (!s_sync_done) return false;
    s_sync_done = false;
    return true;
}
