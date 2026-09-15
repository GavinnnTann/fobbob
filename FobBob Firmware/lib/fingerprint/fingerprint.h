#pragma once
#include <Arduino.h>

//Precusor custom library for HLK-ZW library by GavinnnTann

// ─── Enums ────────────────────────────────────────────────────────────────────

enum class FpEnrollStep : uint8_t { PLACE_FIRST, LIFT_FINGER, PLACE_SECOND, DONE, FAILED };
enum class FpResult     : uint8_t { OK, NO_MATCH, ERROR };

// Stage at which the last matchFingerprint() call gave up. Purely diagnostic —
// it tells a failed verify apart from a sensor that never came up, which is the
// difference between a firmware bug and a supply problem.
enum class FpStage : uint8_t {
    NONE,       // no failure recorded
    CAPTURE,    // GETIMAGE never returned a usable frame
    FEATURE,    // IMAGE2TZ failed on the captured frame
    SEARCH,     // HISPEEDSEARCH/SEARCH failed
};

// ─── Timing ──────────────────────────────────────────────────────────────────
// The ZW101's power-on self-test needs ~200 ms before it will answer on the UART,
// and that is a typical figure, not a guaranteed one — a slower VCC rise (softer
// supply, bigger bulk cap behind the load switch) pushes it out. 400 ms gives the
// module real margin instead of racing its own datasheet number.
#define FP_POWER_SETTLE_MS      400   // after VCC switch-on, before talking to it
#define FP_UART_SETTLE_MS       200   // after _serial.begin(), before the handshake
#define FP_HANDSHAKE_RETRIES      3   // verifyPassword attempts in begin()
#define FP_HANDSHAKE_RETRY_MS   150   // gap between handshake attempts
// A single 0xFF (no response) or 0xFE (bad checksum) is a glitched byte or a
// momentary supply dip, not a dead sensor. Tolerate a few during the capture poll
// so one bad packet can't fail an entire verify.
#define FP_COMM_RETRIES           3
#define FP_COMM_RETRY_MS         30

// ─── LED color wire values for AURALEDCONFIG (0x3C) ──────────────────────────
// These are the actual bytes sent to the module, NOT UI label numbers.
#define FP_LED_BLUE    0x01
#define FP_LED_GREEN   0x02
#define FP_LED_CYAN    0x03   // green + blue
#define FP_LED_RED     0x04
#define FP_LED_PURPLE  0x05   // red + blue
#define FP_LED_YELLOW  0x06   // red + green
#define FP_LED_WHITE   0x07   // all channels
#define FP_LED_OFF     0x00

// LED function codes for ledAura()
#define FP_LED_FUNC_BREATHING   1
#define FP_LED_FUNC_FLASH       2
#define FP_LED_FUNC_STEADY      3
#define FP_LED_FUNC_OFF         4
#define FP_LED_FUNC_GRAD_OPEN   5
#define FP_LED_FUNC_GRAD_CLOSE  6

// ─── FingerprintModule — ZW101 EF-01 UART driver ─────────────────────────────

class FingerprintModule {
public:
    // Last confirm code returned by the module (0x00 = OK; see fingerprint.md §7)
    // 0xFF = no response at all, 0xFE = response arrived but failed the checksum.
    uint8_t lastCC = 0;

    // Where the last matchFingerprint() gave up (see FpStage).
    FpStage lastStage = FpStage::NONE;

    // ── Construction / init ────────────────────────────────────────────────────
    FingerprintModule(HardwareSerial &serial,
                      int rxPin, int txPin,
                      int touchPin = -1,   // TOUCH_OUT GPIO; -1 if unused
                      int pwrPin   = -1);  // VCC enable GPIO; -1 if always-on

    // Start serial, configure pins, wait 200 ms, verify password.
    bool begin(uint32_t baud = 57600);
    void powerOn();    // assert pwrPin HIGH, wait 200 ms
    void powerOff();   // assert pwrPin LOW  (V_SENSOR stays powered)

    // Returns true when a finger is touching the sensor.
    // Uses TOUCH_OUT pin if wired, otherwise polls GetImage.
    bool isFingerPresent();

    // Send VerifyPassword (0x13) with pw (default = 0x00000000).
    // Also serves as module presence check / ping.
    bool verifyPassword(uint32_t pw = 0);

    // ── Enrollment ────────────────────────────────────────────────────────────

    // High-level blocking 2-scan enroll.
    // id = 0xFFFF → auto-assign to first free slot from storage map.
    // Returns enrolled ID (0–49) on success, -1 on error/timeout.
    int16_t enrollFingerprint(uint16_t id = 0xFFFF, uint32_t timeoutMs = 15000);

    // Low-level single-step commands (used by the non-blocking tick).
    bool getImage();                        // GETIMAGE (0x01): sets lastCC
    bool image2Tz(uint8_t bufIdx);         // IMAGE2TZ (0x02): buf 1 or 2
    bool createModel();                     // REGMODEL (0x05): merge buf1+buf2
    bool storeTemplate(uint8_t bufIdx, uint16_t id);  // STORE (0x06)
    bool loadTemplate(uint8_t bufIdx, uint16_t id);   // LOAD  (0x07)

    // ── Matching ──────────────────────────────────────────────────────────────

    // Blocking 1:N match. Polls GetImage within timeoutMs, extracts features,
    // runs HiSpeedSearch (falls back to Search if needed).
    // Returns matched ID (≥0), -1 = no match, -2 = error / timeout.
    int16_t matchFingerprint(uint16_t &score, uint32_t timeoutMs = 10000);

    // ── Deletion ──────────────────────────────────────────────────────────────

    bool deleteFingerprint(uint16_t id);                // DELETE count=1
    bool deleteRange(uint16_t firstId, uint16_t lastId);// DELETE with count
    bool deleteAllFingerprints();                       // EMPTY (0x0D)

    // ── Template management ───────────────────────────────────────────────────

    // Returns true if slot id has a stored template (uses LOAD to probe).
    bool     templateExists(uint16_t id);

    // Returns total enrolled count via TEMPLATECOUNT (0x1D).
    uint16_t getTemplateCount();

    // Read slot occupancy bitmap via READ_INDEX (0x1F, page 0).
    // states[i] = true if slot i is occupied. Returns false if not supported.
    bool     getStorageMap(bool states[50]);

    // Export: LOAD → UpChar data stream → caller-supplied buf.
    // len is set to the number of bytes written. Returns false on error.
    bool exportTemplate(uint16_t id, uint8_t *buf, uint16_t &len, uint16_t maxLen);

    // Import: DownChar data stream → STORE at target id.
    bool importTemplate(uint16_t id, const uint8_t *buf, uint16_t len);

    // ── System settings (all persist to module flash) ─────────────────────────

    // Read READSYSPARAM (0x0F). All out-params are optional (pass nullptr to skip).
    bool readSysParam(uint16_t *capacity  = nullptr,
                      uint8_t  *secLevel  = nullptr,
                      uint8_t  *pktIdx    = nullptr,
                      uint8_t  *baudN     = nullptr);

    bool setSecurityLevel(uint8_t level);  // WRITE_REG 0x05, range 1–5
    bool setBaudReg(uint8_t n);            // WRITE_REG 0x04: 1/2/4/6/12 → ×9600
    bool setPacketSize(uint8_t idx);       // WRITE_REG 0x06: 0–3 (32/64/128/256 B)
    bool setPassword(uint32_t newPw);      // SETPASSWORD (0x12), persists to flash

    // ── LED ───────────────────────────────────────────────────────────────────

    // General Aura LED command (AURALEDCONFIG 0x3C).
    // func: FP_LED_FUNC_* constants.  color: FP_LED_* constants.
    // cycles = 0 for breathing/flash means infinite.
    bool ledAura(uint8_t func, uint8_t color, uint8_t cycles = 0);

    // Convenience wrappers
    bool ledOff();
    bool ledOn();                               // simple LEDON  (0x50)
    bool ledBreathing(uint8_t color = FP_LED_WHITE);
    bool ledFlash(uint8_t color, uint8_t cycles = 3);
    bool ledSteady(uint8_t color = FP_LED_WHITE);
    bool ledGradOpen(uint8_t color = FP_LED_WHITE);
    bool ledGradClose(uint8_t color = FP_LED_WHITE);

    // ── Module control ────────────────────────────────────────────────────────

    // Convenience alias for verifyPassword(0) — confirms module is alive.
    bool ping();

    // Put module into sleep / power-save. The EF-01 ZW101 has no UART sleep
    // command; this asserts pwrPin LOW (powerOff) if a power pin is wired.
    // V_SENSOR remains on so TOUCH_OUT keeps working for deep-sleep wakeup.
    void sleep();

private:
    HardwareSerial &_serial;
    int _rxPin, _txPin, _touchPin, _pwrPin;

    // Build command packet into out[]. Returns total byte count.
    // out[] must be at least 11 + plen bytes.
    static uint16_t _buildPacket(uint8_t ins, const uint8_t *params, uint8_t plen,
                                  uint8_t *out);
    static uint16_t _checksum(uint8_t pid, uint8_t lh, uint8_t ll,
                               const uint8_t *body, uint16_t blen);

    // Read one full packet (header + length + body + checksum) into buf.
    // Returns total bytes (0 = timeout or framing error).
    uint16_t _readPacket(uint8_t *buf, uint16_t maxLen, uint32_t timeoutMs);

    // Read a PID=0x02/0x08 data stream (used after UpChar ACK).
    // Returns total payload bytes written into out[].
    uint16_t _recvStream(uint8_t *out, uint16_t maxLen, uint32_t timeoutMs = 5000);

    // Send data as a PID=0x02/0x08 packet stream (used before DownChar store).
    void _sendStream(const uint8_t *data, uint16_t len, uint16_t pktSize = 128);

    // Core send/receive: build packet, flush RX, write TX, read + parse response.
    // Returns the confirm code (CC). 0xFF = timeout, 0xFE = parse error.
    // dataOut: caller buffer for response payload (may be nullptr).
    // dataLen: set to actual payload byte count on return.
    uint8_t _sendRecv(uint8_t ins, const uint8_t *params, uint8_t plen,
                      uint8_t *dataOut, uint8_t &dataLen,
                      uint32_t timeoutMs = 3000);
};
