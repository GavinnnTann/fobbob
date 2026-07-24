#include "boot_manager.h"
#include "nvs/nvs_manager.h"
#include "buttons/buttons.h"
#include "hardware/hardware.h"
#include "config.h"
#include <esp_sleep.h>
#include <Arduino.h>

void boot_request_reprovision() {
    nvs_request_reprovision();
}

BootMode boot_determine_mode() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    // Highest priority: explicit reprovision request written to NVS before last restart
    if (nvs_consume_reprovision_request()) {
        return BootMode::BLE_REPROVISION;
    }

    // EXT1 is always GPIO4 (FP_TOUCH_PIN, ANY_HIGH) — finger on sensor.
    // GPIO45 (TCA6408A INT) is not an RTC GPIO and cannot source EXT1 wakeup.
    //
    // Confirm the line is genuinely driven before trusting the wakeup. IDF's
    // ext1_wakeup_prepare() disables the pad's pull resistors whenever RTC_PERIPH
    // is powered down in deep sleep (it is, by default), so GPIO4 floats whenever
    // nothing is driving it — sensor unplugged, or its V_TOUCH rail down. Noise on
    // a floating pad then fires ANY_HIGH on its own, and the device cycles
    // sleep → FP screen → FP_MAX_ATTEMPTS comm errors → sleep, indefinitely.
    // A rejected wake falls through to DEEP_SLEEP without lighting the display.
    if (cause == ESP_SLEEP_WAKEUP_EXT1 && hardware_fp_touch_held(FP_TOUCH_CONFIRM_MS)) {
        return BootMode::FINGERPRINT_WAKE;
    }

    // Check for button chord (both buttons held on power-on / reset).
    // Buttons are on TCA6408 expander — read via buttons_chord_active().
    if (buttons_chord_active()) {
        uint32_t chord_start = millis();
        bool chord_held = true;
        while ((millis() - chord_start) < BTN_CHORD_MS) {
            if (!buttons_chord_active()) {
                chord_held = false;
                break;
            }
        }
        if (chord_held) return BootMode::BLE_REPROVISION;
    }

    if (cause == ESP_SLEEP_WAKEUP_UNDEFINED && !nvs_is_provisioned()) {
        return BootMode::BLE_FIRSTBOOT;
    }

#ifdef DEBUG_SERIAL
    if (cause == ESP_SLEEP_WAKEUP_UNDEFINED && nvs_is_provisioned()) {
        return BootMode::FINGERPRINT_WAKE;
    }
#endif

    return BootMode::DEEP_SLEEP;
}
