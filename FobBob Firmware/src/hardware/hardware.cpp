#include "hardware.h"
#include "config.h"
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <Wire.h>

void hardware_init() {
    // I²C bus — shared by PCF85063 RTC and TCA6408 expander (buttons)
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    // FP sensor VCC switch (U14 enable). Park it in a defined OFF state here so
    // the pad is never left floating, and — more importantly — so the pin is
    // registered as a GPIO before fp_init() ever writes to it. Arduino-ESP32 3.x
    // silently ignores digitalWrite() on an unconfigured pad, which is what kept
    // the sensor powered down through the first fp_init() of every boot.
    // powerOn() raises it when the sensor is actually needed.
    pinMode(FP_CTRL_PIN, OUTPUT);
    digitalWrite(FP_CTRL_PIN, LOW);

    // After a deep-sleep wake every digital pad is still latched by
    // gpio_deep_sleep_hold_en() (see hardware_configure_wakeup below), and while a
    // pad is held, pinMode()/digitalWrite() on it are inert. GPIO39 happened to be
    // latched HIGH so V_TOUCH survived by luck; release it explicitly so the writes
    // below actually take effect on a wake as well as on a cold boot.
    gpio_hold_dis((gpio_num_t)FP_VTOUCH_PIN);

    // V_TOUCH supply: temporarily driven from GPIO39 (no PMOS circuit yet).
    // Must be HIGH before the UART handshake so the touch circuit is live.
    pinMode(FP_VTOUCH_PIN, OUTPUT);
    digitalWrite(FP_VTOUCH_PIN, HIGH);

    pinMode(FP_TOUCH_PIN, INPUT);
    Serial2.begin(FP_UART_BAUD, SERIAL_8N1, FP_UART_RX, FP_UART_TX);

    // Backlight PWM — skip until display hardware is wired
    // ledcAttach(TFT_BL_PIN, 5000, 8);  // Arduino-ESP32 3.x API
    // ledcWrite(TFT_BL_PIN, 0);
}

void hardware_configure_wakeup() {
    // GPIO42 (backlight) is outside the RTC domain (GPIOs 0-21 only).
    // gpio_hold_en alone only latches during light sleep; for deep sleep on a
    // digital GPIO you must also call gpio_deep_sleep_hold_en() so the pad
    // isolation latch engages when the digital power domain drops.
    // Without this the pin floats and residual current keeps the backlight glowing.
    gpio_set_level((gpio_num_t)TFT_BL_PIN, 0);
    gpio_hold_en((gpio_num_t)TFT_BL_PIN);
    gpio_deep_sleep_hold_en();

    // GPIO45 (TCA6408A INT) is outside the ESP32-S3 RTC domain (GPIOs 0–21 only)
    // and cannot be used as an EXT1 wakeup source. Always wake via FP_TOUCH_PIN.
    esp_sleep_enable_ext1_wakeup(1ULL << FP_TOUCH_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
}

bool hardware_fp_touch_held(uint32_t window_ms) {
    // A real finger holds TOUCH_OUT high for as long as it rests on the pad
    // (hundreds of ms); a floating pad only reads high intermittently, so one
    // LOW sample anywhere in the window is enough to call it noise.
    uint32_t deadline = millis() + window_ms;
    while ((int32_t)(deadline - millis()) > 0) {
        if (digitalRead(FP_TOUCH_PIN) == LOW) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return digitalRead(FP_TOUCH_PIN) == HIGH;
}
