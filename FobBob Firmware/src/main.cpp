#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <ArduinoJson.h>
#include <time.h>

#include "hardware/hardware.h"
#include "nvs/nvs_manager.h"
#include "boot/boot_manager.h"
#include "ble/ble_provision.h"
#include "hardware/fp_wrapper.h"
#include "totp/totp_engine.h"
#include "buttons/buttons.h"
#include "power/power.h"
#include "display/display.h"
#include "ui/ui_screens.h"
#include "rtc/rtc_manager.h"
#include "imu/imu_manager.h"
#include "touch/touch_manager.h"
#include "battery/battery.h"
#include "haptics/haptics.h"
#include "wifi/wifi_ntp.h"
#include "config.h"
#ifdef DEBUG_SERIAL
#include <lvgl.h>   // lv_mem_monitor — watch the fixed 64 KB LVGL pool
#endif
#include <sys/time.h>

// Parse unix-ms string from BLE, set system clock, persist to PCF85063 + NVS.
static void apply_time(const char* unix_ms_str) {
    uint64_t unix_ms = (uint64_t)strtoull(unix_ms_str, nullptr, 10);
    if (unix_ms == 0) return;
    time_t t = (time_t)(unix_ms / 1000);
    struct timeval tv = { .tv_sec = t, .tv_usec = (suseconds_t)((unix_ms % 1000) * 1000) };
    settimeofday(&tv, nullptr);
    rtc_write(t);
    nvs_save_time(t);
#ifdef DEBUG_SERIAL
    Serial.printf("[TIME] Set — unix:%lld\n", (long long)t);
#endif
}

static BootMode    s_mode       = BootMode::DEEP_SLEEP;
static uint8_t     s_totp_count = 0;
static uint32_t    s_last_print = 0;
static ImuSettings s_imu_cfg    = {};
static bool        s_flipped    = false;  // current orientation state
static uint32_t    s_imu_poll_ms = 0;    // last IMU poll timestamp
static bool        s_was_charging = false; // charge-detect edge tracking (plug-in → modal)
#ifdef DEBUG_SERIAL
static bool        s_calib_mode = false; // stream raw accel when true (CALIB command)
#endif

#ifdef DEBUG_SERIAL
// Worst display_tick() duration since the last [MEM] report — catches creeping
// render slowdowns that a heap snapshot alone would miss.
static uint32_t s_lv_max_us = 0;
#endif

#ifdef DEBUG_SERIAL
// ── Serial command handler ─────────────────────────────────────────────────
static char    s_cmd_buf[32];
static uint8_t s_cmd_len = 0;

static void check_serial_commands() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (s_cmd_len == 0) continue;
            s_cmd_buf[s_cmd_len] = '\0';
            for (uint8_t i = 0; i < s_cmd_len; i++)
                s_cmd_buf[i] = toupper((unsigned char)s_cmd_buf[i]);

            if (strcmp(s_cmd_buf, "BLE") == 0) {
                Serial.println("[CMD] Restarting into BLE provisioning (accounts preserved)...");
                boot_request_reprovision();
                delay(200);
                ESP.restart();
            } else if (strcmp(s_cmd_buf, "WIPE") == 0) {
                Serial.println("[CMD] Wiping NVS — full factory reset...");
                nvs_wipe_all();
                delay(200);
                ESP.restart();
            } else if (strcmp(s_cmd_buf, "STATUS") == 0) {
                time_t now = time(nullptr);
                struct tm* t = gmtime(&now);
                Serial.printf("[STATUS] Time : %04d-%02d-%02d %02d:%02d:%02d UTC (unix:%lld)\n",
                    t->tm_year+1900, t->tm_mon+1, t->tm_mday,
                    t->tm_hour, t->tm_min, t->tm_sec, (long long)now);
                Serial.printf("[STATUS] Mode : %d  Accounts: %d  Provisioned: %s\n",
                    (int)s_mode, s_totp_count, nvs_is_provisioned() ? "yes" : "no");
            } else if (strcmp(s_cmd_buf, "UNPAIR") == 0) {
                if (ble_provision_clear_bonds()) {
                    Serial.println("[CMD] All BLE bonds deleted — also remove FobBob from the OS Bluetooth list");
                } else {
                    Serial.println("[CMD] BLE not active — enter BLE mode first, then UNPAIR");
                }
            } else if (strcmp(s_cmd_buf, "CALIB") == 0) {
                s_calib_mode = !s_calib_mode;
                if (s_calib_mode) {
                    Serial.println("[CALIB] Streaming raw accel — hold device at each position, note ax/ay/az");
                    Serial.println("[CALIB] 1g ≈ 4096 LSB  |  flip trigger should be on the axis perpendicular to screen");
                    Serial.println("[CALIB] Type CALIB again to stop.");
                } else {
                    Serial.println("[CALIB] Stopped.");
                }
            } else {
                Serial.printf("[CMD] Unknown command '%s'\n", s_cmd_buf);
                Serial.println("[CMD] Commands: BLE, WIPE, STATUS, UNPAIR, CALIB");
            }
            s_cmd_len = 0;
        } else if (s_cmd_len < sizeof(s_cmd_buf) - 1) {
            s_cmd_buf[s_cmd_len++] = c;
        }
    }
}
#endif

// ── IMU polling (adaptive timeout, double-tap, orientation) ──────────────
// Called every loop_fp_wake() iteration. No-op if features are disabled.
static void imu_poll() {
    uint32_t now_ms = millis();
    if ((now_ms - s_imu_poll_ms) < IMU_POLL_MS) return;
    s_imu_poll_ms = now_ms;

#ifdef DEBUG_SERIAL
    if (s_calib_mode) {
        int16_t ax, ay, az;
        if (imu_read_accel(&ax, &ay, &az)) {
            Serial.printf("[CALIB] ax=%-6d ay=%-6d az=%-6d   (1g≈4096)\n", ax, ay, az);
        }
    }
#endif

    // Adaptive timeout: motion resets the display idle timer
    if (s_imu_cfg.adaptive_timeout && imu_is_moving()) {
        power_reset_idle_timer();
        ui_activity();
    }

    // Orientation: rotate display and swap button directions when upside-down
    if (s_imu_cfg.orient_flip) {
        bool flipped = imu_is_flipped(s_flipped);
        if (flipped != s_flipped) {
            s_flipped = flipped;
            display_set_rotation(flipped ? 180 : 0);
            touch_set_flipped(flipped);
        }
    }
}

// ── Touch handling (CST816 gestures) ──────────────────────────────────────
// Swipe up/down navigates; long-press opens settings. Settings is a modal
// overlay driven entirely from here via the ui_settings_* API.
static bool s_in_settings  = false;
static bool s_in_wifi_info = false;   // WiFi credentials page (held WiFi row) is showing

// Persist on-device setting changes, re-apply live, and return to the TOTP screen.
static void settings_exit_and_save() {
    haptic_tick();   // confirm leaving settings (all exit paths route through here)
    bool tap, adapt, flip, haptics, wifi_en;
    uint16_t disp = TOTP_DISPLAY_SEC_DEFAULT;
    ui_settings_get(&tap, &adapt, &flip, &haptics, &disp, &wifi_en);

    s_imu_cfg.tap_sleep        = tap;
    s_imu_cfg.adaptive_timeout = adapt;
    s_imu_cfg.orient_flip      = flip;

    nvs_save_imu_settings(s_imu_cfg);
    nvs_save_display_sec(disp);
    nvs_save_haptic_enabled(haptics);
    nvs_save_wifi_enabled(wifi_en);   // flips the toggle only — credentials untouched
    haptic_set_enabled(haptics);
    imu_configure(s_imu_cfg);
    power_set_config(disp);
    ui_set_display_timeout(disp);

    // Auto-rotate may have just been turned off — restore upright orientation.
    if (!s_imu_cfg.orient_flip && s_flipped) {
        s_flipped = false;
        display_set_rotation(0);
        touch_set_flipped(false);
    }

    s_in_settings = false;
    ui_show_totp(s_totp_count);
}

// Touch state machine — one transformed contact point drives tap, swipe, drag
// and long-press. Gestures are derived from finger travel, so tap and swipe
// always agree about orientation (single transform in touch_read_point()).
// Gesture thresholds live in config.h (TAP_MOVE_MAX, SWIPE_MIN_*, TOUCH_LONG_MS,
// REPROV_HOLD_MS, DOUBLE_TAP_MS).

// Navigation detent feedback (settings wheel + TOTP account scroll). Rate-limited
// (NAV_HAPTIC_MIN_MS, config.h) so a fast drag / held button reads as separate
// clicks instead of one long buzz.
static uint32_t s_nav_haptic_ms = 0;
static void nav_haptic() {
    uint32_t n = millis();
    if (n - s_nav_haptic_ms >= NAV_HAPTIC_MIN_MS) { haptic_click(); s_nav_haptic_ms = n; }
}

// A single screen tap toggles code hide/reveal; a double tap sleeps (when enabled).
// When double-tap-to-sleep is on, the single-tap action is deferred by DOUBLE_TAP_MS
// so a second tap can be recognised as a double instead.
static bool     s_pending_tap    = false;
static uint32_t s_pending_tap_ms = 0;

// Enter deep sleep: power down the fingerprint sensor first, then sleep (never returns).
static void device_sleep() {
    haptic_blocking(HAPTIC_SLEEP_MS);   // "goodbye" pulse — loop won't run again to poll a pattern
    fp_sleep();
    display_off();
    power_go_to_sleep();
}

static bool     s_t_down     = false;
static int      s_t_start_x = 0, s_t_start_y = 0, s_t_last_x = 0, s_t_last_y = 0;
static uint32_t s_t_start_ms = 0;
static bool     s_t_moved    = false;
static bool     s_t_long     = false;
static bool     s_t_hold     = false;  // press began on a hold-to-trigger row (Reprovision)
static bool     s_t_hold_done = false; // hold completed → action fired
static int      s_set_detent = -1;     // last focused settings row during a drag (haptic per detent)
static uint32_t s_rattle_ms  = 0;      // last reprovision-hold rattle pulse

// Reprovision hold "rattle": accelerating clicks that build anticipation as the bar
// fills — slow at the start, blurring into a near-continuous buzz just before commit.
// Spacing range is RATTLE_MS_SLOW → RATTLE_MS_FAST (config.h).

// Sample the battery and push it to the settings status row.
static void update_settings_battery() {
    BatteryStatus b = battery_read();
    ui_settings_set_battery(b.percent, b.charging);
}

// Compute + push the "Sync now" row value: "Syncing..." while a sync is in flight,
// otherwise the age of the last successful sync ("Never" / "12s ago" / "3h ago").
static void update_settings_sync_status() {
    char buf[24];
    int  state = 0;   // 0 neutral, 1 ok, 2 fail, 3 busy
    switch (wifi_ntp_state()) {
        case WifiNtpState::CONNECTING:   strncpy(buf, "Connecting...", sizeof(buf)); state = 3; break;
        case WifiNtpState::SYNCING:      strncpy(buf, "Syncing...",    sizeof(buf)); state = 3; break;
        case WifiNtpState::CONNECT_FAIL: strncpy(buf, "WiFi failed",   sizeof(buf)); state = 2; break;
        case WifiNtpState::NTP_FAIL:     strncpy(buf, "No NTP",        sizeof(buf)); state = 2; break;
        default: {
            time_t last = nvs_get_last_ntp_sync();
            if (last <= 0) {
                strncpy(buf, "Never", sizeof(buf));
            } else {
                long age = (long)(time(nullptr) - last);
                if (age < 0)          age = 0;
                if (age < 60)         snprintf(buf, sizeof(buf), "%lds ago", age);
                else if (age < 3600)  snprintf(buf, sizeof(buf), "%ldm ago", age / 60);
                else if (age < 86400) snprintf(buf, sizeof(buf), "%ldh ago", age / 3600);
                else                  snprintf(buf, sizeof(buf), "%ldd ago", age / 86400);
                state = 1;   // a prior sync succeeded → green WiFi glyph
            }
            break;
        }
    }
    buf[sizeof(buf) - 1] = '\0';
    ui_settings_set_sync_status(buf, state);
}

static void open_settings() {
    s_in_settings = true;
    haptic_tick();               // confirm the long-press landed
    ui_show_settings(s_imu_cfg.tap_sleep, s_imu_cfg.adaptive_timeout,
                     s_imu_cfg.orient_flip, nvs_get_haptic_enabled(),
                     nvs_get_display_sec(), nvs_get_wifi_enabled());
    s_set_detent = ui_settings_centered_row();   // baseline so the first nav step clicks
    update_settings_battery();        // seed the status rows immediately
    update_settings_sync_status();
}

static void settings_do_action(UiSettingsAction act) {
    if (act == UI_SETTINGS_REPROVISION) {
#ifdef DEBUG_SERIAL
        Serial.println("[TOUCH] Reprovision selected — restarting into BLE");
#endif
        haptic_blocking(HAPTIC_COMMIT_MS);   // strong commit confirm; device restarts right after
        boot_request_reprovision();
        delay(100);
        ESP.restart();
    } else if (act == UI_SETTINGS_SYNC_NOW) {
        // Force a background sync regardless of the 12 h interval. If WiFi is
        // disabled or unprovisioned the request is a silent no-op; the row will
        // reflect "Syncing..." then the result on the next 1 Hz status update.
        haptic_tick();
        // Honour a just-flipped WiFi toggle that hasn't been persisted yet.
        bool wifi_en = false;
        ui_settings_get(nullptr, nullptr, nullptr, nullptr, nullptr, &wifi_en);
        if (wifi_en) nvs_save_wifi_enabled(true);
        wifi_ntp_request_sync(true);
        update_settings_sync_status();
    } else if (act == UI_SETTINGS_EXIT) {
        settings_exit_and_save();   // ticks itself
    }
}

// Hold the WiFi settings row → full-screen view of the stored credentials.
// Non-destructive, so it's a plain long-press (not a hold-to-commit fill).
static void open_wifi_info() {
    char ssid[64] = {}, pass[64] = {};
    nvs_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    s_in_settings  = false;
    s_in_wifi_info = true;
    haptic_tick();
    ui_show_wifi_info(ssid, pass);
}

static void close_wifi_info() {
    s_in_wifi_info = false;
    open_settings();   // back to the settings wheel (re-seeds, centres on WiFi)
}

static void handle_touch() {
    // Only react on the TOTP screen, the settings overlay, or the WiFi-info page.
    if (!s_in_settings && !s_in_wifi_info && !ui_totp_active()) { s_t_down = false; return; }

    // ~16 ms poll (≈60 Hz) — smooth enough for drag, light on the shared I²C bus.
    static uint32_t s_poll_ms = 0;
    uint32_t now = millis();
    if ((now - s_poll_ms) < TOUCH_POLL_MS) return;
    s_poll_ms = now;

    // A deferred single tap fires once the double-tap window passes with no 2nd tap.
    if (s_pending_tap && (now - s_pending_tap_ms) >= DOUBLE_TAP_MS) {
        s_pending_tap = false;
        if (ui_totp_active()) { ui_totp_toggle_hide(); haptic_tick(); }
    }

    int x = 0, y = 0;
    bool pressed = touch_read_point(&x, &y);

    // WiFi credentials page: any tap/swipe release dismisses it back to settings.
    if (s_in_wifi_info) {
        if (pressed) { s_t_down = true; ui_activity(); }
        else if (s_t_down) { s_t_down = false; close_wifi_info(); }
        return;
    }

    if (pressed) {
        if (!s_t_down) {
            // Finger down — start a new gesture.
            s_t_down = true;
            s_t_start_x = x; s_t_start_y = y; s_t_last_x = x; s_t_last_y = y;
            s_t_start_ms = now;
            s_t_moved = false; s_t_long = false;
            s_t_hold = false; s_t_hold_done = false;
            // Hold-to-trigger only arms on the Reprovision row while it's already in
            // the focus slot — pressing it off-centre is just a tap (scroll-to-focus).
            if (s_in_settings) {
                int r = ui_settings_row_at(x, y);
                s_t_hold = ui_settings_row_is_hold(r) && (r == ui_settings_centered_row());
                s_set_detent = ui_settings_centered_row();   // seed the per-detent haptic baseline
                s_rattle_ms = now;                           // start the hold rattle fresh
            }
            ui_activity();
        } else {
            int dy = y - s_t_last_y;
            s_t_last_x = x;
            s_t_last_y = y;
            if (abs(x - s_t_start_x) > TAP_MOVE_MAX || abs(y - s_t_start_y) > TAP_MOVE_MAX)
                s_t_moved = true;

            // Moving off a hold row cancels the commit and becomes a scroll.
            if (s_t_moved && s_t_hold) {
                s_t_hold = false;
                ui_settings_set_hold_progress(0);
            }

            if (s_t_hold && !s_t_hold_done) {
                // Progressive hold-to-trigger: fill the bar; fire at 100%.
                uint32_t held = now - s_t_start_ms;
                int pct = (int)(held * 100 / REPROV_HOLD_MS);
                if (pct >= 100) {
                    s_t_hold_done = true;
                    ui_settings_set_hold_progress(100);
                    settings_do_action(UI_SETTINGS_REPROVISION);  // restarts the device
                } else {
                    ui_settings_set_hold_progress(pct);
                    // Accelerating rattle: pulse spacing shrinks SLOW → FAST as the bar
                    // fills, so the buzz tightens into anticipation right before commit.
                    uint32_t interval = RATTLE_MS_SLOW -
                        (uint32_t)pct * (RATTLE_MS_SLOW - RATTLE_MS_FAST) / 100;
                    if (now - s_rattle_ms >= interval) {
                        s_rattle_ms = now;
                        haptic_click();
                    }
                }
                ui_activity();
            } else if (s_in_settings && s_t_moved) {
                // Live drag-scroll the wheel (content follows the finger). Fire a
                // haptic detent each time a different option lands in the focus slot
                // (rate-limited so a fast drag stays a series of clicks, not a buzz).
                int c = ui_settings_scroll_by(dy);
                if (c != s_set_detent) { s_set_detent = c; nav_haptic(); }
                ui_activity();
            }

            // Long-press (held still):
            //   • from TOTP        → open settings
            //   • on the WiFi row  → open the WiFi credentials page
            if (!s_t_moved && !s_t_long && !s_t_hold && (now - s_t_start_ms) > TOUCH_LONG_MS) {
                s_t_long = true;
                if (!s_in_settings) {
                    open_settings();
                } else if (ui_settings_focused_is_wifi() &&
                           ui_settings_row_at(s_t_start_x, s_t_start_y) == ui_settings_centered_row()) {
                    open_wifi_info();
                }
            }
        }
    } else if (s_t_down) {
        // Finger up — classify the completed gesture.
        s_t_down = false;
        uint32_t dt = now - s_t_start_ms;
        int net_dy = s_t_last_y - s_t_start_y;
        int net_dx = s_t_last_x - s_t_start_x;

        if (s_t_long || s_t_hold_done) {
            // Already handled on the down-stroke (opened settings / fired commit).
        } else if (s_t_hold) {
            // Released a hold row before completion — cancel the fill, no action.
            ui_settings_set_hold_progress(0);
        } else if (!s_t_moved && dt < 600) {
            // Tap.
            if (s_in_settings) {
                UiSettingsAction act = ui_settings_tap(s_t_start_x, s_t_start_y);
                if (act == UI_SETTINGS_NONE) {
                    // Toggling Haptics applies live so the confirm buzz reflects the
                    // new state (off → silent, on → buzz). Sync before the tick.
                    bool hap; ui_settings_get(nullptr, nullptr, nullptr, &hap, nullptr, nullptr);
                    haptic_set_enabled(hap);
                    haptic_tick();   // toggle / cycle / scroll-to-focus feedback
                }
                // UI_SETTINGS_IGNORED (battery / empty) → no action, no feedback.
                settings_do_action(act);                     // EXIT ticks itself
            } else if (s_imu_cfg.tap_sleep) {
                // Double-tap-to-sleep enabled: a 2nd tap within the window sleeps;
                // otherwise defer the single-tap hide/reveal toggle to disambiguate.
                if (s_pending_tap && (now - s_pending_tap_ms) < DOUBLE_TAP_MS) {
                    s_pending_tap = false;
                    device_sleep();   // never returns
                } else {
                    s_pending_tap = true;
                    s_pending_tap_ms = now;
                }
            } else {
                // No double-tap action — toggle hide/reveal immediately.
                ui_totp_toggle_hide();
                haptic_tick();
            }
        } else if (s_t_moved) {
            if (s_in_settings) {
                // A clear horizontal swipe (either direction) is a quick "back" to
                // the TOTP screen; otherwise it was a vertical scroll — snap to slot.
                if (abs(net_dx) >= SWIPE_MIN_X && abs(net_dx) > abs(net_dy)) {
                    settings_exit_and_save();
                } else {
                    ui_settings_snap();
                }
            } else {
                // TOTP: page by net vertical travel (swipe up → next account).
                if      (net_dy <= -SWIPE_MIN_Y) { ui_totp_next(); nav_haptic(); }
                else if (net_dy >=  SWIPE_MIN_Y) { ui_totp_prev(); nav_haptic(); }
            }
        }
    }
}

// ── Time sync BLE state ───────────────────────────────────────────────────
static uint32_t s_timesync_start_ms = 0;
static bool     s_timesync_active   = false;

// Run the fingerprint UART task on the app core (core 1), NOT core 0. NimBLE's
// controller + host run on core 0 at a priority well above this task; when
// ble_timesync_start() fires at the same instant as a wake-time verify, BLE
// starves the FP task on core 0 — stretching init/LED/match (visible lag + LED
// flicker) and occasionally delaying the post-verify ledOff so the sensor's
// default breathing aura lingers on the TOTP screen. On core 1 this is the
// highest-priority app task (loopTask/LVGL is priority 1) so it stays responsive,
// and it never touches LVGL so there's no re-entrancy risk sharing that core.
static constexpr BaseType_t FP_TASK_CORE = 1;

// ── Fingerprint verify — async (FreeRTOS task so display keeps pumping) ────
struct FpVerifyResult_t {
    FpResult result;
    uint8_t  id;
    // ── Diagnostics, filled by fp_diag_finish() ──
    bool        init_ok;   // did fp_init() complete a handshake?
    uint8_t     cc;        // driver's last confirm code (0xFF none, 0xFE bad checksum)
    const char* stage;     // stage a failed match gave up at (static string)
    uint16_t    v_start;   // pack mV before the sensor was powered
    uint16_t    v_init;    // pack mV immediately after fp_init()
    uint16_t    v_min;     // lowest pack mV seen while the sensor was active
};

// Fingerprint diagnostics: report the failure reason on the verify screen and
// hold it before sleeping. On battery there is no serial cable attached, so the
// panel is the only channel that can say what actually failed. Set to 0 once the
// fingerprint path is trusted again.
#define FP_DIAG_ON_SCREEN      1
#define FP_DIAG_HOLD_MS     6000

// ── Fingerprint rail monitor ───────────────────────────────────────────────
// Samples the pack voltage unfiltered while the sensor is powered and keeps the
// minimum. battery_read()'s EMA exists to smooth momentary load sag away, which
// is the one thing that matters when asking whether a LIR2450 holds up under the
// capture burst. Pinned to the core the FP task is not on.
static constexpr BaseType_t VMON_TASK_CORE = 0;
static volatile uint16_t s_vmin     = 0xFFFF;
static volatile bool     s_vmon_run = false;

static void vmon_task(void*) {
    while (s_vmon_run) {
        uint16_t mv = battery_raw_mv();
        if (mv && mv < s_vmin) s_vmin = mv;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelete(nullptr);
}
static QueueHandle_t    s_fp_queue           = nullptr;
static uint32_t         s_fp_anim_trigger_ms = 0;   // when the latest anim cycle was triggered
static uint32_t         s_fp_anim_end_ms     = 0;   // end of the current anim cycle
static bool             s_fp_got_result      = false;
static FpVerifyResult_t s_fp_last            = { FpResult::ERROR, 0 };
static bool             s_fp_showing_result  = false; // cross is playing
static bool             s_fp_retry_after_cross = false; // retry if true, sleep if false
static uint8_t          s_fp_attempts_left   = FP_MAX_ATTEMPTS;

// Stop the rail monitor and stamp the collected diagnostics into res.
static void fp_diag_finish(FpVerifyResult_t &res) {
    res.cc     = fp_last_cc();
    res.stage  = fp_last_stage();
    s_vmon_run = false;
    vTaskDelay(pdMS_TO_TICKS(12));          // let vmon_task observe the flag and exit
    res.v_min  = (s_vmin == 0xFFFF) ? 0 : s_vmin;
}

static void fp_verify_task(void*) {
    FpVerifyResult_t res = {};
    res.result = FpResult::ERROR;
    res.stage  = "NONE";

    // Start the monitor before the sensor is powered so the minimum covers the
    // load-switch inrush as well as the capture burst.
    res.v_start = battery_raw_mv();
    s_vmin      = 0xFFFF;
    s_vmon_run  = true;
    xTaskCreatePinnedToCore(vmon_task, "vmon", 2048, nullptr, 1, nullptr, VMON_TASK_CORE);

    res.init_ok = fp_init(2000);
    res.v_init  = battery_raw_mv();

    if (res.init_ok) {
        fp_led_steady_blue();
        res.result = fp_verify(&res.id, FP_MATCH_TIMEOUT_MS);
        if (res.result == FpResult::OK || res.result == FpResult::NO_MATCH) {
            fp_diag_finish(res);
            // Deliver the result immediately so the UI reacts without delay,
            // THEN let the sensor's own green/red flash finish before going dark.
            xQueueSend(s_fp_queue, &res, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (res.result == FpResult::OK) {
                // Verified → heading to the TOTP screen; the sensor isn't used
                // again until the next deep-sleep wake. Cut its power instead of
                // a software LED-off: the module re-lights its default breathing
                // aura on its own while a finger lingers on the pad, overriding
                // any ledOff. Killing VCC keeps it dark and saves power; the next
                // wake's fp_init() powers it back on. (Mirrors deep-sleep, where
                // power_go_to_sleep() already powers the sensor down.)
                fp_sleep();
            } else {
                // NO_MATCH may retry — keep the sensor powered, just LED off.
                fp_led_off();
            }
            vTaskDelete(nullptr);
        }
        // ERROR (timeout/comm): no finger flash to wait for — go dark now so
        // the silently-respawned verify task never overlaps us on the UART.
        fp_led_off();
    }
    fp_diag_finish(res);
    xQueueSend(s_fp_queue, &res, portMAX_DELAY);
    vTaskDelete(nullptr);
}

// ── Reprovision fingerprint gate ───────────────────────────────────────────
// The button chord / serial BLE command alone must not open BLE provisioning:
// require a verified enrolled finger first. Skipped when nothing is enrolled
// (first boot / wiped device) and in DEBUG_SERIAL builds (no FP hardware).
// Fail-closed: sensor errors count as failed attempts → device sleeps.
#ifndef DEBUG_SERIAL
static bool reprovision_fp_gate() {
    if (nvs_get_fp_count() == 0) return true;   // no finger enrolled — nothing to verify against

    ui_show_fp_verify();
    ui_fp_set_status("Verify finger to\nreprovision");
    for (int i = 0; i < 10; i++) { display_tick(); vTaskDelay(pdMS_TO_TICKS(10)); }

    for (uint8_t att = 0; att < FP_MAX_ATTEMPTS; att++) {
        s_fp_queue = xQueueCreate(1, sizeof(FpVerifyResult_t));
        xTaskCreatePinnedToCore(fp_verify_task, "fp_verify", 4096, nullptr, 2, nullptr, FP_TASK_CORE);
        FpVerifyResult_t res;
        while (xQueueReceive(s_fp_queue, &res, 0) != pdTRUE) {
            display_tick();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        vQueueDelete(s_fp_queue);
        s_fp_queue = nullptr;

        if (res.result == FpResult::OK) {
            ui_fp_set_status("Verified");
            // Pump through the verify task's 1 s LED wind-down so the
            // fp_init that follows (enroll readiness) can't collide with
            // its fp_led_off on the UART — and the user sees feedback
            // instead of a frozen verify screen.
            uint32_t until = millis() + 1100;
            while ((int32_t)(until - millis()) > 0) {
                display_tick();
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            return true;
        }
        if (res.result == FpResult::NO_MATCH) {
            // Cross animation, then back to the idle prompt. Pumping through the
            // full animation (>=1.5 s) also guarantees the finished verify task's
            // 1 s LED wind-down can't overlap the next task on the UART.
            ui_fp_verify_result(false);
            while (ui_fp_result_active()) { display_tick(); vTaskDelay(pdMS_TO_TICKS(5)); }
            ui_fp_set_status("Verify finger to\nreprovision");
        }
        // ERROR (no finger within timeout / comm fail) — counts as a failed attempt
    }
    return false;
}
#endif

// ── BLE provisioning state ─────────────────────────────────────────────────
// Provisioning is BLE-only. WiFi is used solely for optional background NTP sync
// (src/wifi/wifi_ntp.*) and never runs alongside the BLE radio.
static uint32_t     s_prov_start_ms   = 0;
static bool         s_fp_enrolling    = false;
static bool         s_fp_ready        = false;  // true once fp_init succeeded this session
static uint8_t      s_fp_slot         = 0;
static uint8_t      s_fp_next_slot    = 0;   // session-local counter; avoids reading stale live NVS
static char         s_fp_name[32]     = {};
static FpEnrollStep s_enroll_prev     = FpEnrollStep::PLACE_FIRST;


// ── TOTP loop ──────────────────────────────────────────────────────────────
static void loop_fp_wake() {
    uint32_t now_ms = millis();
    time_t   now    = time(nullptr);

    // Track anim cycle boundary for result-display timing; animation loops on its own (INFINITE)
    if (s_fp_queue && !s_fp_got_result) {
        uint32_t anim_dur = ui_fp_anim_duration_ms();
        if ((int32_t)(now_ms - s_fp_anim_trigger_ms) >= (int32_t)anim_dur) {
            s_fp_anim_trigger_ms = now_ms;
            s_fp_anim_end_ms     = now_ms + anim_dur;
        }
        FpVerifyResult_t res;
        if (xQueueReceive(s_fp_queue, &res, 0) == pdTRUE) {
            vQueueDelete(s_fp_queue);
            s_fp_queue      = nullptr;
            s_fp_last       = res;
            s_fp_got_result = true;
#ifdef DEBUG_SERIAL
            Serial.printf("[FP] Verify: %s (id=%d, left=%d) | init=%s cc=0x%02X stage=%s | "
                          "Vstart=%umV Vinit=%umV Vmin=%umV (sag %dmV)\n",
                res.result == FpResult::OK ? "MATCH" :
                res.result == FpResult::NO_MATCH ? "NO_MATCH" : "ERROR",
                res.id, s_fp_attempts_left - 1,
                res.init_ok ? "OK" : "FAIL", res.cc, res.stage,
                res.v_start, res.v_init, res.v_min,
                (int)res.v_start - (int)res.v_min);
#endif
        }
    }

    // Apply result once FP is done AND the current anim cycle has played through
    if (s_fp_got_result && !s_fp_showing_result &&
        (int32_t)(now_ms - s_fp_anim_end_ms) >= 0) {
        s_fp_got_result = false;
        s_fp_attempts_left--;
#ifdef DEBUG_SERIAL
        Serial.printf("[FP] %d attempt(s) remaining\n", s_fp_attempts_left);
#endif
        if (s_fp_last.result == FpResult::OK) {
            s_totp_count = nvs_get_totp_count();
            haptic_tick();               // verified — crisp confirmation
            ui_fp_verify_result(true);   // tick → auto-transitions to TOTP
        } else if (s_fp_last.result == FpResult::NO_MATCH) {
            // Wrong finger — show cross, then retry or sleep
            s_fp_showing_result    = true;
            s_fp_retry_after_cross = (s_fp_attempts_left > 0);
            haptic_double();             // rejected — distinct double buzz
            ui_fp_verify_result(false);
        } else {
            // ERROR: timeout (no finger placed) or comm failure — silent restart
#if FP_DIAG_ON_SCREEN
            {
                char dbg[128];
                snprintf(dbg, sizeof(dbg), "%s cc=%02X\n%s\n%u>%u min %u mV",
                         s_fp_last.init_ok ? "init ok" : "INIT FAIL",
                         s_fp_last.cc, s_fp_last.stage,
                         s_fp_last.v_start, s_fp_last.v_init, s_fp_last.v_min);
                ui_fp_set_status(dbg);
            }
#endif
            if (s_fp_attempts_left > 0) {
                ui_fp_anim_trigger();
                s_fp_anim_trigger_ms = millis();
                s_fp_anim_end_ms     = millis() + ui_fp_anim_duration_ms();
                s_fp_queue = xQueueCreate(1, sizeof(FpVerifyResult_t));
                xTaskCreatePinnedToCore(fp_verify_task, "fp_verify", 4096, nullptr, 2, nullptr, FP_TASK_CORE);
            } else {
#if FP_DIAG_ON_SCREEN
                uint32_t until = millis() + FP_DIAG_HOLD_MS;   // long enough to read
                while ((int32_t)(until - millis()) > 0) {
                    display_tick();
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
#endif
                fp_sleep();
                power_go_to_sleep();
            }
        }
    }

    // After cross animation finishes: retry or sleep
    if (s_fp_showing_result && !ui_fp_result_active()) {
        s_fp_showing_result = false;
        if (s_fp_retry_after_cross) {
            // Restart the idle screen and launch a fresh verify task
            ui_show_fp_verify();
            for (int i = 0; i < 5; i++) { display_tick(); vTaskDelay(pdMS_TO_TICKS(10)); }
            ui_fp_anim_trigger();
            s_fp_anim_trigger_ms = millis();
            s_fp_anim_end_ms     = millis() + ui_fp_anim_duration_ms();
            s_fp_got_result      = false;
            s_fp_queue = xQueueCreate(1, sizeof(FpVerifyResult_t));
            xTaskCreatePinnedToCore(fp_verify_task, "fp_verify", 4096, nullptr, 2, nullptr, FP_TASK_CORE);
        } else {
            fp_sleep();
            power_go_to_sleep();
        }
    }

    // BLE time-sync window — close on receipt or timeout
    if (s_timesync_active) {
        BleProvEvent ev = ble_provision_tick();
        bool timeout = (now_ms - s_timesync_start_ms) >= TIMESYNC_WINDOW_MS;
        if (ev == BleProvEvent::NTP_TIME_RECEIVED || timeout) {
            if (ev == BleProvEvent::NTP_TIME_RECEIVED)
                apply_time(ble_provision_get_ntp_payload());
            ble_provision_stop();
            s_timesync_active = false;
#ifdef DEBUG_SERIAL
            if (timeout) Serial.println("[BLE] Time sync window closed");
#endif
        }
    }

    // Background WiFi/NTP sync — fire-and-forget once the user is on the TOTP
    // screen and the BLE timesync window has closed (don't run both radios at
    // once). The interval/credentials/enabled checks live inside the call, so
    // this is a no-op when WiFi is off or a recent sync already happened.
    static bool s_ntp_kicked = false;
    if (!s_ntp_kicked && !s_timesync_active && ui_totp_active()) {
        s_ntp_kicked = true;
        wifi_ntp_request_sync(false);
    }
    // Consume the success flag so the icon flashes once and the flag doesn't leak.
    if (wifi_ntp_consume_sync_done()) {
        ui_totp_set_synced();
#ifdef DEBUG_SERIAL
        Serial.println("[WIFI] background NTP sync completed");
#endif
    }

    // Button handling — navigation + chord to re-provision
    ButtonEvent btn = buttons_tick();
    if (btn == ButtonEvent::CHORD_HELD) {
        // Both buttons held 5 s → enter BLE re-provisioning on next boot.
#ifdef DEBUG_SERIAL
        Serial.println("[BTN] Chord held — restarting into BLE provisioning");
#endif
        boot_request_reprovision();
        delay(100);
        ESP.restart();
    }
    // Power button (P4) single tap → sleep, from both the TOTP and settings screens.
    if (btn == ButtonEvent::BTN_PWR_PRESS) {
#ifdef DEBUG_SERIAL
        Serial.println("[BTN] Power tap — sleeping");
#endif
        device_sleep();   // never returns
    }
    // Swap navigation direction when display is rotated 180°. BTN1/BTN2 auto-repeat
    // while held. In settings they step the wheel; on TOTP they page accounts. Both
    // give a rate-limited detent click so a held run-through reads as distinct steps.
    if (btn == ButtonEvent::BTN1_PRESS || btn == ButtonEvent::BTN2_PRESS) {
        bool fwd = (btn == ButtonEvent::BTN1_PRESS) ^ s_flipped;
        if (s_in_settings) {
            int c = ui_settings_nav(fwd ? +1 : -1);
            if (c != s_set_detent) { s_set_detent = c; nav_haptic(); }
        } else {
            (fwd ? ui_totp_next : ui_totp_prev)();
            nav_haptic();
        }
        ui_activity();
    }

    // 1 Hz UI update (TOTP codes, arc, datetime)
    if (s_last_print == 0 || (now_ms - s_last_print) >= 1000) {
        s_last_print = now_ms;
        ui_tick();

        // Read battery once a second (also keeps the EMA warm). Drive the settings
        // row when it's open; on the TOTP screen, fire the charging modal on a
        // rising plug-in edge (not-charging → charging).
        BatteryStatus bat = battery_read();
        if (s_in_settings) {
            ui_settings_set_battery(bat.percent, bat.charging);
            update_settings_sync_status();   // refresh "Syncing..." / age live
        } else if (ui_totp_active()) {
            if (bat.charging && !s_was_charging) { ui_totp_show_charging(bat.percent); haptic_tick(); }
        }
        s_was_charging = bat.charging;

#ifdef DEBUG_SERIAL
        // 10-second memory + render-latency report. If the display degrades over
        // a long session, this shows whether it's heap exhaustion/fragmentation
        // (free/largest shrinking), an LVGL-pool problem, or render time creep.
        static uint8_t s_mem_tick = 0;
        if (++s_mem_tick >= 10) {
            s_mem_tick = 0;
            lv_mem_monitor_t lv_mon;
            lv_mem_monitor(&lv_mon);
            // loopTask stack headroom (bytes, worst case since boot). ThorVG renders
            // Lottie on THIS task (LV_OS_NONE + 1 draw unit), so the 8 KB Arduino
            // default overflows on the first FP/BLE animation — see
            // ARDUINO_LOOP_STACK_SIZE in platformio.ini. If this approaches 0 the
            // device reboot-loops; raise the flag rather than trimming animations.
            Serial.printf("[MEM] heap free:%u min-ever:%u largest:%u | psram free:%u | lv_max:%lums | loop stack free:%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned long)(s_lv_max_us / 1000),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            // LVGL has a fixed 64 KB pool with a while(1) malloc-fail handler — if
            // used_pct nears 100 the device hangs. Watch it here.
            Serial.printf("[MEM] lvgl used:%u%% free:%u frag:%u%%\n",
                (unsigned)lv_mon.used_pct, (unsigned)lv_mon.free_size, (unsigned)lv_mon.frag_pct);
            s_lv_max_us = 0;
        }

        int secs_left = (int)totp_seconds_remaining(now);

        for (uint8_t i = 0; i < s_totp_count; i++) {
            TotpAccount acct;
            if (nvs_get_totp_account(i, &acct)) {
                uint32_t code = totp_from_base32(acct.secret_b32, now);
                if (code != UINT32_MAX) {
                    Serial.printf("[TOTP] %-20s  %06lu  [%2ds] %s\n",
                        acct.name, (unsigned long)code, secs_left,
                        secs_left <= 5 ? "!!!" : (secs_left <= 10 ? " ! " : ""));
                }
            }
        }
#endif
    }

    imu_poll();
    handle_touch();

#ifndef DEBUG_SERIAL
    power_tick();
#endif
}

// ── BLE provisioning ───────────────────────────────────────────────────────
// Start the BLE GATT transport (NimBLE) and show the provisioning screen + PIN.
static void start_ble_provisioning() {
    s_prov_start_ms = millis();            // full window for the actual session
    haptic_tick();
    uint32_t pin = ble_provision_start();
#ifdef DEBUG_SERIAL
    Serial.printf("[BLE] PIN: %06lu\n", (unsigned long)pin);
#endif
    ui_show_ble_provision();
    char pin_buf[32];
    snprintf(pin_buf, sizeof(pin_buf), "PIN: %06lu", (unsigned long)pin);
    ui_ble_set_status(pin_buf);
}

static void loop_ble_provision() {
    if ((millis() - s_prov_start_ms) >= PROVISION_TIMEOUT_MS) {
        ble_provision_stop();
#ifdef DEBUG_SERIAL
        Serial.println("[BLE] Provision timeout — sleeping");
#endif
        display_off();
        power_go_to_sleep();
    }

    // Power button (P4) cancels provisioning and reboots to normal operation.
    if (buttons_tick() == ButtonEvent::BTN_PWR_PRESS) {
#ifdef DEBUG_SERIAL
        Serial.println("[BTN] Power tap — cancelling provisioning, rebooting");
#endif
        ble_provision_stop();
        delay(100);
        ESP.restart();   // reprovision flag already consumed this boot → boots normally
    }

    BleProvEvent ev = ble_provision_tick();

    switch (ev) {
        case BleProvEvent::FP_COMMAND_RECEIVED: {
            const char* payload = ble_provision_get_fp_cmd();
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, payload) != DeserializationError::Ok) break;
            const char* cmd = doc["cmd"] | "";

            if (strcmp(cmd, "START_ENROLL") == 0) {
                if (!s_fp_ready && !fp_init(2000)) {
#ifdef DEBUG_SERIAL
                    Serial.println("[FP] Init failed for enroll");
#endif
                    ble_provision_notify_fp_status("ENROLL_FAILED", 0);
                    break;
                }
                if (!s_fp_ready) {
                    s_fp_ready = true;
                    fp_led_breathe_blue();  // early BLE init failed; set LED now
                }
                s_fp_slot = s_fp_next_slot;  // session counter — avoids stale live NVS
                strncpy(s_fp_name, doc["name"] | "", sizeof(s_fp_name) - 1);
                s_fp_name[sizeof(s_fp_name) - 1] = '\0';
                if (s_fp_name[0] == '\0') {
                    snprintf(s_fp_name, sizeof(s_fp_name), "Finger %d", s_fp_slot + 1);
                }
                s_fp_enrolling = true;
                s_enroll_prev  = FpEnrollStep::PLACE_FIRST;
                fp_enroll_reset();
                ui_show_fp_enroll();
                ble_provision_notify_fp_status("PLACE_FINGER", s_fp_slot);

            } else if (strcmp(cmd, "DELETE") == 0) {
                uint8_t del_id = doc["id"] | 0;
                // Only call fp_init if not already done this session — re-calling
                // begin() on an active UART + 3 × 3-second timeouts can block long
                // enough for the BLE supervision timer to disconnect the client,
                // causing ble_provision_notify_fp_status to silently drop DELETE_OK.
                if (!s_fp_ready) {
                    if (!fp_init(2000)) {
                        ble_provision_notify_fp_status("DELETE_FAILED", del_id);
                        break;
                    }
                    s_fp_ready = true;
                    fp_led_breathe_blue();
                }
                bool del_ok = fp_delete(del_id);
                if (!del_ok) del_ok = fp_delete(del_id);  // one retry
                if (del_ok) {
                    uint8_t cnt = fp_count();
                    if (cnt != 0xFF) nvs_stage_fp_count(cnt);
                    ble_provision_notify_fp_status("DELETE_OK", del_id);
                } else {
#ifdef DEBUG_SERIAL
                    Serial.printf("[FP] Delete failed for sensor page %d\n", del_id);
#endif
                    ble_provision_notify_fp_status("DELETE_FAILED", del_id);
                }
            }
            break;
        }

        case BleProvEvent::IMU_SETTINGS_RECEIVED:
            // Already staged inside ble_provision_tick(); cache locally for immediate use.
            s_imu_cfg = ble_provision_get_imu_settings();
            imu_configure(s_imu_cfg);
            power_set_config(nvs_get_display_sec());
            break;

        case BleProvEvent::NTP_TIME_RECEIVED:
            apply_time(ble_provision_get_ntp_payload());
            break;

        case BleProvEvent::FACTORY_RESET_REQUESTED: {
            // Full factory reset triggered from the webapp (PIN-gated + "WIPE ALL"
            // confirmation enforced in FactoryResetCB). Mirrors the serial WIPE
            // command but also empties the fingerprint sensor — the sensor has
            // final authority on verification, so stale templates must be purged.
            ui_ble_set_status("Wiping...");
#ifdef DEBUG_SERIAL
            Serial.println("[BLE] Factory reset requested — wiping NVS + sensor");
#endif
            if (!s_fp_ready) s_fp_ready = fp_init(2000);
            if (s_fp_ready) {
                if (!fp_delete_all()) fp_delete_all();  // one retry
            }
            nvs_wipe_all();
            ble_provision_stop();
            delay(500);   // let the BLE write response flush before reboot
            ESP.restart();
            break;
        }

        case BleProvEvent::PROVISION_DONE: {
            // Purge any orphaned sensor templates before committing.
            // The webapp sends the exact set of valid sensor pages via FP_NAMES;
            // any page present on the sensor but not in that set is a stale template
            // (e.g. deleted during this session but not yet purged from the ZW101).
            if (s_fp_ready) {
                uint8_t valid_slots[FP_MAX_TEMPLATES] = {};
                uint8_t valid_count = ble_provision_get_fp_slots(valid_slots);
                bool occupied[50] = {};
                if (valid_count > 0 && fp_get_storage_map(occupied, 50)) {
                    for (uint8_t page = 0; page < 50; page++) {
                        if (!occupied[page]) continue;
                        bool is_valid = false;
                        for (uint8_t j = 0; j < valid_count; j++) {
                            if (valid_slots[j] == page) { is_valid = true; break; }
                        }
                        if (!is_valid) {
#ifdef DEBUG_SERIAL
                            Serial.printf("[FP] Purging orphaned sensor page %d\n", page);
#endif
                            fp_delete(page);
                        }
                    }
                }
            }
            if (nvs_commit_staging()) {
                ui_ble_set_status("Done!");
                ble_provision_stop();
#ifdef DEBUG_SERIAL
                Serial.println("[BLE] Committed — restarting");
#endif
                haptic_blocking(HAPTIC_COMMIT_MS);   // provisioning complete; device restarts next
                delay(500);
                ESP.restart();
            }
            break;
        }

        default:
            break;
    }

    if (s_fp_enrolling) {
        FpEnrollStep step = fp_enroll_tick(s_fp_slot);
        if (step != s_enroll_prev) {
            s_enroll_prev = step;
            switch (step) {
                case FpEnrollStep::LIFT_FINGER:
                    ble_provision_notify_fp_status("LIFT_FINGER", s_fp_slot);
                    ui_fp_enroll_update(1, true);   // "Scan 1/2" on display
                    break;
                case FpEnrollStep::PLACE_SECOND:
                    ble_provision_notify_fp_status("PLACE_AGAIN", s_fp_slot);
                    // No display update here — idle screen already says "Place finger again"
                    break;
                case FpEnrollStep::DONE:
                    nvs_stage_fp_finger(s_fp_slot, s_fp_slot, s_fp_name);
                    nvs_stage_fp_count(s_fp_slot + 1);
                    s_fp_next_slot = s_fp_slot + 1;  // advance session counter
                    ble_provision_notify_fp_status("ENROLLED_OK", s_fp_slot);
                    ui_fp_enroll_update(2, true);   // "Enrolled!" (captures_done >= 2)
                    haptic_tick();                  // finger enrolled
                    s_fp_enrolling = false;
                    break;
                case FpEnrollStep::FAILED:
                    ble_provision_notify_fp_status("ENROLL_FAILED", s_fp_slot);
                    ui_fp_enroll_update(0, false);
                    haptic_double();                // enroll failed
                    s_fp_enrolling = false;
                    fp_enroll_reset();
                    break;
                default:
                    break;
            }
        }
    }
}

// ── Entry points ───────────────────────────────────────────────────────────
void setup() {
#ifdef DEBUG_SERIAL
    Serial.begin(115200);
    // Never block on USB CDC. With the native-USB HWCDC, each write blocks up to
    // the TX timeout (default 100 ms) when a host is enumerated but nobody is
    // draining the port (serial monitor opened once, then closed). The 1 Hz TOTP
    // print loop then stalls the main task for hundreds of ms per second, which
    // shows up as the display getting progressively laggier "after a while".
    Serial.setTxTimeoutMs(0);
    delay(500);
    Serial.println("[BOOT] FobBob");
#endif

    hardware_init();
    nvs_init();

    // Load IMU settings and display timeout from NVS; configure power module.
    s_imu_cfg = nvs_get_imu_settings();
    uint16_t disp_sec = nvs_get_display_sec();
    ui_set_display_timeout(disp_sec);
    power_set_config(disp_sec);

    // Restore system clock. Sources, best first:
    //   1. PCF85063 hardware RTC — crystal-accurate, but only valid if it
    //      never lost power (OS flag clear). With no backup battery it dies
    //      with the board's power.
    //   2. The already-running system clock — the ESP32-S3's internal RTC
    //      timer keeps counting through deep sleep and software resets, so on
    //      a normal wake this is correct and must NOT be overwritten by a
    //      stale fallback.
    //   3. NVS last-known time (saved on every sleep entry + BLE sync) —
    //      bounds the rollback after a total power loss.
    //   4. BUILD_TIMESTAMP — compile-time floor, last resort.
    // If the PCF is invalid but we recovered a plausible time, reseed it so
    // its crystal takes over timekeeping while the board stays powered.
    {
        bool   rtc_ok  = rtc_init();
        time_t rtc_t   = rtc_ok ? rtc_read() : 0;
        if (rtc_t > 0) {
            struct timeval tv = { .tv_sec = rtc_t };
            settimeofday(&tv, nullptr);
        } else {
            time_t best = time(nullptr);              // survives deep sleep
            time_t nv   = nvs_load_time();
            if (nv > best) best = nv;
            if ((time_t)BUILD_TIMESTAMP > best) best = (time_t)BUILD_TIMESTAMP;
            time_t sys = time(nullptr);
            if (best > sys) {
                struct timeval tv = { .tv_sec = best };
                settimeofday(&tv, nullptr);
            }
            // PCF lost its time — hand it our best estimate (also clears the
            // OS flag) so it keeps ticking on its crystal from here on.
            if (best > 0) rtc_write(best);
        }
    }

    buttons_init();

    imu_init();
    imu_configure(s_imu_cfg);

    touch_init();
    battery_init();
    haptics_init();
    haptic_set_enabled(nvs_get_haptic_enabled());   // honour the saved haptics toggle
    // Seed charge state so the charging modal fires only on a real plug-in event
    // during a session, not just because the device woke up already plugged in.
    s_was_charging = battery_read().charging;

    s_mode = boot_determine_mode();

#ifdef DEBUG_SERIAL
    const char* names[] = { "FP_WAKE", "BLE_FIRSTBOOT", "BLE_REPROVISION", "DEEP_SLEEP" };
    Serial.printf("[BOOT] Mode: %s\n", names[(uint8_t)s_mode]);
    char dev_name_buf[64] = {};
    if (nvs_get_device_name(dev_name_buf, sizeof(dev_name_buf)))
        Serial.printf("[BOOT] Device name: '%s'\n", dev_name_buf);
    else
        Serial.println("[BOOT] Device name: (not set)");
#endif

    // Initialise display only for modes that show UI; skip for DEEP_SLEEP
    if (s_mode != BootMode::DEEP_SLEEP) {
        if (!display_init()) {
#ifdef DEBUG_SERIAL
            Serial.println("[BOOT] display_init FAILED — halting");
#endif
            while (true) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        ui_screens_init();
        // Splash only on full boots (BLE modes) — skip on deep-sleep wake so the
        // fingerprint screen appears immediately after the user touches the sensor.
        if (s_mode != BootMode::FINGERPRINT_WAKE) {
            ui_show_splash();
            uint32_t splash_end = millis() + 3000;
            while ((int32_t)(splash_end - millis()) > 0) {
                display_tick();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    switch (s_mode) {
        case BootMode::FINGERPRINT_WAKE: {
            ui_show_fp_verify();
            for (int i = 0; i < 10; i++) { display_tick(); vTaskDelay(pdMS_TO_TICKS(10)); }
            s_fp_attempts_left = FP_MAX_ATTEMPTS;
            ui_fp_anim_trigger();
            s_fp_anim_trigger_ms = millis();
            s_fp_anim_end_ms     = millis() + ui_fp_anim_duration_ms();
            s_fp_queue = xQueueCreate(1, sizeof(FpVerifyResult_t));
            xTaskCreatePinnedToCore(fp_verify_task, "fp_verify", 4096, nullptr, 2, nullptr, FP_TASK_CORE);
            power_reset_idle_timer();
            // Timesync method:
            //   • WiFi usable (creds + enabled) → NTP over WiFi. We do NOT open the
            //     BLE timesync window here; the background NTP sync in loop_fp_wake
            //     fires once the TOTP screen is up, syncs, then drops the radio.
            //   • Otherwise → open the 30 s BLE timesync window (legacy path).
            if (!nvs_wifi_ready()) {
                ble_timesync_start();
                s_timesync_start_ms = millis();
                s_timesync_active   = true;
            }
            break;
        }

        case BootMode::BLE_FIRSTBOOT:
        case BootMode::BLE_REPROVISION: {
            // WiFi radio stays off — provisioning is BLE-only.
            wifi_ntp_radio_off();
#ifndef DEBUG_SERIAL
            // Reprovision requires a verified enrolled finger; first boot has none.
            if (s_mode == BootMode::BLE_REPROVISION && !reprovision_fp_gate()) {
                fp_sleep();
                display_off();
                power_go_to_sleep();
            }
#endif
            s_fp_next_slot = nvs_get_fp_count();  // read once — staging not committed yet
            // Init sensor now so we can drive the LED immediately; if this fails
            // START_ENROLL will retry. s_fp_ready guards against a redundant re-init.
            if (fp_init(2000)) {
                s_fp_ready = true;
                fp_led_breathe_blue();
            }
            start_ble_provisioning();
            break;
        }

        case BootMode::DEEP_SLEEP:
        default:
            power_go_to_sleep();
            break;
    }
}

void loop() {
#ifdef DEBUG_SERIAL
    check_serial_commands();
    uint32_t lv_t0 = micros();
    display_tick();
    uint32_t lv_dt = micros() - lv_t0;
    if (lv_dt > s_lv_max_us) s_lv_max_us = lv_dt;
#else
    display_tick();   // pump LVGL as often as possible
#endif
    haptics_poll();   // advance any active vibration pattern (non-blocking)
    switch (s_mode) {
        case BootMode::FINGERPRINT_WAKE:
            loop_fp_wake();
            break;
        case BootMode::BLE_FIRSTBOOT:
        case BootMode::BLE_REPROVISION:
            loop_ble_provision();
            break;
        default:
            break;
    }
    // 5 ms pacing: lv_timer_handler is called ~2x per LV_DEF_REFR_PERIOD (16 ms)
    // so render deadlines are never missed by loop jitter, while still yielding
    // plenty of idle time to lower-priority housekeeping.
    vTaskDelay(pdMS_TO_TICKS(5));
}
