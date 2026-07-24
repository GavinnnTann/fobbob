#pragma once
#include <stdint.h>
#include <stdbool.h>

// Live state of the WiFi/NTP path, for the settings indicator.
enum class WifiNtpState : uint8_t {
    IDLE,          // nothing happening
    CONNECTING,    // associating with the AP
    SYNCING,       // connected, waiting for the NTP packet
    OK,            // last sync succeeded
    CONNECT_FAIL,  // couldn't join the network (wrong creds / out of range)
    NTP_FAIL,      // connected but no NTP packet arrived
};
WifiNtpState wifi_ntp_state();

// ── Optional WiFi → NTP background time sync ────────────────────────────────
// Connects to the provisioned WiFi network, pulls time over SNTP, persists it
// (system clock + PCF85063 + NVS), then drops the radio. Designed to run on its
// own FreeRTOS task so the TOTP display never blocks: the user sees their code
// immediately and WiFi connects silently in the background only when due.
//
// All entry points are safe to call when WiFi is disabled / unprovisioned — they
// simply do nothing. Failures are silent (no error UI on a 30 s display).

// Connect to the stored SSID/pass in STA mode. Blocks up to timeout_ms.
// Returns true once associated + got an IP. Leaves the radio up on success.
bool wifi_connect_blocking(uint32_t timeout_ms);

// Full synchronous sync: connect → SNTP → persist → drop radio. Returns true if
// a sane time was obtained. Blocking — call from a task, not the main loop.
bool wifi_ntp_sync_blocking();

// Spawn the background sync task. No-op if a sync is already running or WiFi is
// not ready (disabled or no credentials). `force` skips the interval check.
void wifi_ntp_request_sync(bool force);

// True while a background sync task is in flight.
bool wifi_ntp_busy();

// One-shot: returns true exactly once after a background sync succeeds, so the
// UI can flash a "synced" icon. Clears itself on read.
bool wifi_ntp_consume_sync_done();

// Force the WiFi radio fully off (STA/AP both down). Called at provisioning entry
// so the radio can't contend with BLE — provisioning is BLE-only.
void wifi_ntp_radio_off();
