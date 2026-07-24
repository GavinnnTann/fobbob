#include "touch_manager.h"
#include "hardware/hardware.h"
#include <Wire.h>
#include <Arduino.h>

static constexpr uint8_t CST816_ADDR     = 0x15;
static constexpr uint8_t REG_GESTURE     = 0x01;  // burst start: gesture,fingers,xH,xL,yH,yL
static constexpr uint8_t REG_CHIP_ID     = 0xA7;  // 0xB4=CST816S, 0xB5=CST816T, 0xB6=CST816D
static constexpr uint8_t REG_IRQ_CTL     = 0xFA;  // interrupt mode
static constexpr uint8_t REG_MOTION_MASK = 0xEC;  // gesture enable mask
static constexpr uint8_t REG_DIS_SLEEP   = 0xFE;  // auto-sleep disable

static bool s_ok   = false;
static bool s_flip = false;   // current display flip — mirrors coordinates when false

static bool cst_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool cst_read(uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(CST816_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(CST816_ADDR, (uint8_t)len);
    for (uint8_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

bool touch_init() {
    // Hardware reset. GPIO0 is the ESP32-S3 BOOT strapping pin but is safe to
    // drive after setup() — the ROM bootloader has already sampled it.
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(5);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(100);

    uint8_t id = 0;
    bool ok = cst_read(REG_CHIP_ID, &id, 1);
#ifdef DEBUG_SERIAL
    Serial.printf("[TOUCH] probe: %s  chip_id=0x%02X (expect 0xB4/B5/B6)\n",
        ok ? "ACK" : "NACK", id);
#endif
    if (!ok || (id != 0xB4 && id != 0xB5 && id != 0xB6)) {
        s_ok = false;
        return false;
    }

    // mode_motion (0x11): INT fires on click, swipe, double-click, long-press.
    // Motion mask 0x01: enable double-click detection.
    // DIS_SLEEP 0xFF: prevent the chip auto-sleeping after ~5s idle.
    cst_write(REG_IRQ_CTL,     0x11);
    cst_write(REG_MOTION_MASK, 0x01);
    cst_write(REG_DIS_SLEEP,   0xFF);

    s_ok = true;
    return true;
}

void touch_set_flipped(bool flipped) {
    s_flip = flipped;
}

bool touch_read_point(int* x, int* y) {
    if (!s_ok) return false;

    // Burst-read 6 bytes: [gesture, finger_count, x_high, x_low, y_high, y_low]
    uint8_t buf[6] = {};
    if (!cst_read(REG_GESTURE, buf, 6)) return false;

    if (buf[1] == 0) return false;   // finger_count == 0 → no contact

    int rx = ((int)(buf[2] & 0x0F) << 8) | buf[3];
    int ry = ((int)(buf[4] & 0x0F) << 8) | buf[5];

    // Normal orientation: panel mounted 180° (MADCTL 0xC8) vs. the displayed
    // image, so the raw contact must be mirrored to land where the user sees it.
    // Flipped orientation (MADCTL 0x08): panel and image agree — use raw.
    if (!s_flip) { rx = 239 - rx; ry = 239 - ry; }

    if (rx < 0) rx = 0; else if (rx > 239) rx = 239;
    if (ry < 0) ry = 0; else if (ry > 239) ry = 239;

    if (x) *x = rx;
    if (y) *y = ry;
    return true;
}
