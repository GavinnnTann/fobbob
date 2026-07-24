#pragma once
#include <stdint.h>

enum class BootMode : uint8_t {
    FINGERPRINT_WAKE,   // woke from EXT1 on FP_TOUCH_PIN (GPIO4)
    BLE_FIRSTBOOT,      // no provisioned key in NVS
    BLE_REPROVISION,    // both buttons held >= BTN_CHORD_MS on boot
    DEEP_SLEEP,         // unexpected wakeup cause — sleep immediately
};

// Determine boot mode. Call once in setup() after hardware_init() and nvs_init().
BootMode boot_determine_mode();

// Set an RTC flag so the next ESP.restart() boots into BLE_REPROVISION
// without wiping NVS. Caller must call ESP.restart() after.
void boot_request_reprovision();
