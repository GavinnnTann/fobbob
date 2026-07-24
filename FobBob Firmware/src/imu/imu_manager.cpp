#include "imu_manager.h"
#include "config.h"
#include <Wire.h>
#include <Arduino.h>

// ── QMI8658C register map (verified against SensorLib + QMI8658C datasheet) ──
static constexpr uint8_t QMI_ADDR       = 0x6B;  // SA0 pulled HIGH on Waveshare module

static constexpr uint8_t REG_WHO_AM_I   = 0x00;  // read: 0x05
static constexpr uint8_t REG_CTRL1      = 0x02;
static constexpr uint8_t REG_CTRL2      = 0x03;  // accel ODR + full-scale
static constexpr uint8_t REG_CTRL7      = 0x08;  // sensor enable
static constexpr uint8_t REG_CTRL8      = 0x09;  // motion / tap config
static constexpr uint8_t REG_CTRL9      = 0x0A;  // host command
static constexpr uint8_t REG_STATUSINT  = 0x2D;  // interrupt status
static constexpr uint8_t REG_AX_L       = 0x35;  // accel XYZ, 6 bytes little-endian
static constexpr uint8_t REG_RESET      = 0x60;  // write 0xB0 to soft-reset
static constexpr uint8_t REG_RST_RESULT = 0x4D;  // 0x80 when reset complete

// CTRL1 bit assignments:
//   bit 6: EN.ADDR_AI  — I2C address auto-increment for burst reads  ← REQUIRED
//   bit 4: INT2_EN     — enable INT2 output
//   bit 3: INT1_EN     — enable INT1 output
//   bit 1: INT2_POL    — 0=active HIGH, 1=active LOW
//   bit 0: INT1_POL    — 0=active HIGH (default), 1=active LOW
// After soft reset the chip auto-sets bit 6; we set it explicitly for safety.
static constexpr uint8_t CTRL1_RUN      = 0x48;  // EN.ADDR_AI | INT1_EN, INT1 active-HIGH

// CTRL2 bits[6:4]=accel range (0=±2g,1=±4g,2=±8g,3=±16g), bits[3:0]=ODR
//   ODR 8=31.25Hz, 12=LP-128Hz, 13=LP-21Hz, 14=LP-11Hz, 15=LP-3Hz
static constexpr uint8_t CTRL2_RUN      = 0x2C;  // ±8g, LP-128Hz

// CTRL8 constant bits:
//   bit 7: use STATUS_INT.bit7 as CTRL9 CMD_DONE handshake — always keep
static constexpr uint8_t CTRL8_HANDSHAKE = 0x80;

// CTRL9 commands
static constexpr uint8_t CMD_ACK         = 0x00;

// ── I²C helpers ──────────────────────────────────────────────────────────────
static bool qmi_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool qmi_read(uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(QMI_ADDR, (uint8_t)len);
    for (uint8_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

static bool qmi_read1(uint8_t reg, uint8_t* val) {
    return qmi_read(reg, val, 1);
}

// Send CTRL9 command, wait for CMD_DONE (STATUS_INT bit7), then ACK.
static bool qmi_ctrl9(uint8_t cmd) {
    if (!qmi_write(REG_CTRL9, cmd)) return false;
    uint32_t deadline = millis() + 150;
    uint8_t st = 0;
    while (millis() < deadline) {
        if (qmi_read1(REG_STATUSINT, &st) && (st & 0x80)) break;
        delay(2);
    }
    qmi_write(REG_CTRL9, CMD_ACK);
    return (st & 0x80) != 0;
}

// Soft reset — chip re-initialises and auto-sets EN.ADDR_AI in CTRL1.
static bool qmi_soft_reset() {
    qmi_write(REG_RESET, 0xB0);
    delay(15);
    uint32_t t = millis();
    uint8_t rst = 0;
    while (millis() - t < 100) {
        if (qmi_read1(REG_RST_RESULT, &rst) && rst == 0x80) return true;
        delay(5);
    }
    return rst == 0x80;
}

// ── State ─────────────────────────────────────────────────────────────────────
static bool        s_ok       = false;
static ImuSettings s_cfg      = {};

// Motion detection: previous accel sample (per-axis) + a debounce run-length.
static int16_t     s_prev_ax = 0, s_prev_ay = 0, s_prev_az = 0;
static bool        s_prev_valid = false;   // false until the first sample is taken
static uint8_t     s_move_run   = 0;       // consecutive above-threshold polls (hysteresis)

// ── Public API ────────────────────────────────────────────────────────────────
bool imu_init() {
#ifdef DEBUG_SERIAL
    Serial.printf("[IMU] init — I2C addr 0x%02X, trying soft reset...\n", QMI_ADDR);
#endif

    // Probe the bus first: if the device isn't there the reset write will NACK.
    Wire.beginTransmission(QMI_ADDR);
    int probe = Wire.endTransmission();
#ifdef DEBUG_SERIAL
    Serial.printf("[IMU] bus probe: %s (endTransmission=%d; 0=ACK, 2=NACK)\n",
        probe == 0 ? "ACK — device found" : "NACK — no device at this address", probe);
#endif
    if (probe != 0) { s_ok = false; return false; }

    bool reset_ok = qmi_soft_reset();
#ifdef DEBUG_SERIAL
    uint8_t rst_val = 0;
    qmi_read1(REG_RST_RESULT, &rst_val);
    Serial.printf("[IMU] soft reset: %s (RST_RESULT=0x%02X, expected 0x80)\n",
        reset_ok ? "OK" : "FAIL", rst_val);
#endif
    if (!reset_ok) { s_ok = false; return false; }

    uint8_t who = 0;
    bool rd_ok = qmi_read1(REG_WHO_AM_I, &who);
#ifdef DEBUG_SERIAL
    Serial.printf("[IMU] WHO_AM_I: read_ok=%d  value=0x%02X (expected 0x05)\n", rd_ok, who);
#endif
    if (!rd_ok || who != 0x05) { s_ok = false; return false; }

    qmi_write(REG_CTRL1, CTRL1_RUN);
    qmi_write(REG_CTRL2, CTRL2_RUN);
    qmi_write(REG_CTRL8, CTRL8_HANDSHAKE);
    qmi_write(REG_CTRL7, 0x01);

#ifdef DEBUG_SERIAL
    // Read back every register we wrote to confirm the writes landed.
    uint8_t rb1=0, rb2=0, rb7=0, rb8=0;
    qmi_read1(REG_CTRL1, &rb1);
    qmi_read1(REG_CTRL2, &rb2);
    qmi_read1(REG_CTRL7, &rb7);
    qmi_read1(REG_CTRL8, &rb8);
    Serial.printf("[IMU] CTRL1  wrote=0x%02X  read=0x%02X  %s\n", CTRL1_RUN,      rb1, rb1==CTRL1_RUN      ? "OK" : "MISMATCH");
    Serial.printf("[IMU] CTRL2  wrote=0x%02X  read=0x%02X  %s\n", CTRL2_RUN,      rb2, rb2==CTRL2_RUN      ? "OK" : "MISMATCH");
    Serial.printf("[IMU] CTRL7  wrote=0x01    read=0x%02X  %s\n",                  rb7, rb7==0x01           ? "OK" : "MISMATCH");
    Serial.printf("[IMU] CTRL8  wrote=0x%02X  read=0x%02X  %s\n", CTRL8_HANDSHAKE, rb8, rb8==CTRL8_HANDSHAKE? "OK" : "MISMATCH");
    // Quick sanity: read 6 accel bytes to see if burst reads work
    int16_t ax=0, ay=0, az=0;
    bool ar = imu_read_accel(&ax, &ay, &az);
    Serial.printf("[IMU] first accel read: %s  ax=%d  ay=%d  az=%d\n", ar ? "OK" : "FAIL", ax, ay, az);
    Serial.println("[IMU] init complete — s_ok=true");
#endif

    s_ok = true;
    return true;
}

void imu_configure(const ImuSettings& cfg) {
    s_cfg = cfg;
    if (!s_ok) return;

    // Accel-only run mode. The IMU now drives just adaptive-timeout (motion) and
    // orientation-flip — both polled from the accel registers. Tap detection moved
    // to the capacitive screen, so no CTRL8 motion/tap interrupt config is needed.
    qmi_write(REG_CTRL7, 0x00);         // disable accel before changing CTRL2
    qmi_write(REG_CTRL2, CTRL2_RUN);
    qmi_write(REG_CTRL8, CTRL8_HANDSHAKE);
    qmi_write(REG_CTRL7, 0x01);

#ifdef DEBUG_SERIAL
    uint8_t rb_c2=0, rb_c7=0;
    qmi_read1(REG_CTRL2, &rb_c2);
    qmi_read1(REG_CTRL7, &rb_c7);
    Serial.printf("[IMU] configure: adaptive=%d flip=%d  CTRL2=0x%02X CTRL7=0x%02X\n",
        cfg.adaptive_timeout, cfg.orient_flip, rb_c2, rb_c7);
#endif
}

void imu_sleep_mode() {
    if (!s_ok) return;
    qmi_write(REG_CTRL7, 0x00);  // accel off during deep sleep
}

bool imu_read_accel(int16_t* ax, int16_t* ay, int16_t* az) {
    if (!s_ok || !ax || !ay || !az) return false;
    uint8_t buf[6];
    if (!qmi_read(REG_AX_L, buf, 6)) return false;
    *ax = (int16_t)((buf[1] << 8) | buf[0]);
    *ay = (int16_t)((buf[3] << 8) | buf[2]);
    *az = (int16_t)((buf[5] << 8) | buf[4]);
    return true;
}

bool imu_is_moving() {
    if (!s_ok || !s_cfg.adaptive_timeout) return false;
    int16_t ax, ay, az;
    if (!imu_read_accel(&ax, &ay, &az)) {
#ifdef DEBUG_SERIAL
        Serial.println("[IMU] imu_is_moving: accel read FAILED");
#endif
        return false;
    }

    // Per-axis vector delta from the previous sample — a high-pass on acceleration
    // that rejects the (large, constant) gravity term. The old code differenced the
    // sum-of-squares magnitude, whose derivative scales with gravity (≈2·az·Δaz), so a
    // few LSB of noise on the gravity axis read as huge "motion" and the screen never
    // slept on a still table. The true vector change is tiny when stationary.
    int32_t dx = (int32_t)ax - s_prev_ax;
    int32_t dy = (int32_t)ay - s_prev_ay;
    int32_t dz = (int32_t)az - s_prev_az;
    s_prev_ax = ax; s_prev_ay = ay; s_prev_az = az;

    if (!s_prev_valid) { s_prev_valid = true; return false; }  // no baseline yet

    int32_t delta2  = dx*dx + dy*dy + dz*dz;
    int32_t thresh2 = (int32_t)IMU_MOTION_THRESHOLD * IMU_MOTION_THRESHOLD;

    // Hysteresis: a single above-threshold sample isn't motion. Require
    // IMU_MOTION_DEBOUNCE consecutive above-threshold polls before reporting
    // "moving", and reset the run on any quiet sample. This stops a stray spike
    // from resetting the idle timer while still catching sustained handling.
    if (delta2 > thresh2) { if (s_move_run < 255) s_move_run++; }
    else                  { s_move_run = 0; }
    bool moving = (s_move_run >= IMU_MOTION_DEBOUNCE);

#ifdef DEBUG_SERIAL
    Serial.printf("[IMU] MOTION  d=(%ld,%ld,%ld)  delta2=%ld  thresh2=%ld  run=%u  moving=%d\n",
        (long)dx, (long)dy, (long)dz, (long)delta2, (long)thresh2, s_move_run, moving);
#endif
    return moving;
}

bool imu_is_flipped(bool current_state) {
    if (!s_ok || !s_cfg.orient_flip) return false;
    int16_t ax, ay, az;
    if (!imu_read_accel(&ax, &ay, &az)) {
#ifdef DEBUG_SERIAL
        Serial.println("[IMU] imu_is_flipped: accel read FAILED");
#endif
        return current_state;
    }
    // Only re-evaluate when the device is in portrait orientation (|ax| > 2000).
    // When tilted sideways, gravity shifts to Y and ax weakens below this guard,
    // so we hold the current state to prevent spurious flips while tilting left/right.
    //   Normal portrait:  ax ≈ -5600  → |ax| > 2000, evaluate  → flip=false
    //   Upside-down:      ax ≈ +2400  → |ax| > 2000, evaluate  → flip=true
    //   Right/Left/Flat:  |ax| ≈ 1000–1700  → hold current_state
    int16_t ax_abs = ax < 0 ? -ax : ax;
    if (ax_abs <= 2000) {
#ifdef DEBUG_SERIAL
        Serial.printf("[IMU] FLIP    ax=%-6d ay=%-6d az=%-6d  sideways — holding state=%d\n",
            ax, ay, az, current_state);
#endif
        return current_state;
    }
    bool flipped = ax > 500;
#ifdef DEBUG_SERIAL
    Serial.printf("[IMU] FLIP    ax=%-6d ay=%-6d az=%-6d  threshold ax>500  flipped=%d\n",
        ax, ay, az, flipped);
#endif
    return flipped;
}
