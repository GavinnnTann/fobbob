#include "power.h"
#include "hardware/hardware.h"
#include "display/display.h"
#include "nvs/nvs_manager.h"
#include "imu/imu_manager.h"
#include "config.h"
#include <esp_sleep.h>
#include <time.h>

static uint32_t s_idle_start_ms    = 0;
static bool     s_timer_active     = false;
static uint32_t s_timeout_ms       = TOTP_DISPLAY_SEC_DEFAULT * 1000UL;

void power_set_config(uint16_t display_sec) {
    if (display_sec < TOTP_DISPLAY_SEC_MIN) display_sec = TOTP_DISPLAY_SEC_MIN;
    if (display_sec > TOTP_DISPLAY_SEC_MAX) display_sec = TOTP_DISPLAY_SEC_MAX;
    s_timeout_ms = (uint32_t)display_sec * 1000UL;
}

void power_go_to_sleep() {
    time_t now = time(nullptr);
    if (now > 1000000000) nvs_save_time(now);

    display_sleep();
    imu_sleep_mode();
    hardware_configure_wakeup();
    esp_deep_sleep_start();
}

void power_reset_idle_timer() {
    s_idle_start_ms = millis();
    s_timer_active  = true;
}

void power_tick() {
    if (!s_timer_active) return;
    if ((millis() - s_idle_start_ms) >= s_timeout_ms) {
        power_go_to_sleep();
    }
}
