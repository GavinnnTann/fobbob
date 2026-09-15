#include "fingerprint.h"
#include <string.h>

// ─── EF-01 wire constants ─────────────────────────────────────────────────────

#define PID_CMD  0x01   // host → module command packet
#define PID_ACK  0x07   // module → host response packet
#define PID_DATA 0x02   // data stream intermediate packet
#define PID_END  0x08   // data stream final packet

// INS bytes (command opcodes)
#define INS_GETIMAGE       0x01
#define INS_IMAGE2TZ       0x02
#define INS_SEARCH         0x04
#define INS_REGMODEL       0x05
#define INS_STORE          0x06
#define INS_LOAD           0x07
#define INS_UPCHAR         0x08
#define INS_DOWNCHAR       0x09
#define INS_DELETE         0x0C
#define INS_EMPTY          0x0D
#define INS_WRITE_REG      0x0E
#define INS_READSYSPARAM   0x0F
#define INS_SETPASSWORD    0x12
#define INS_VERIFYPASSWORD 0x13
#define INS_HISPEEDSEARCH  0x1B
#define INS_TEMPLATECOUNT  0x1D
#define INS_READ_INDEX     0x1F
#define INS_AURALEDCONFIG  0x3C
#define INS_LEDON          0x50
#define INS_LEDOFF         0x51

// ─── Packet builder / parser ──────────────────────────────────────────────────

// 16-bit checksum: sum of PID + Length_H + Length_L + all body bytes, masked to 16 bits.
uint16_t FingerprintModule::_checksum(uint8_t pid, uint8_t lh, uint8_t ll,
                                       const uint8_t *body, uint16_t blen) {
    uint32_t s = (uint32_t)pid + lh + ll;
    for (uint16_t i = 0; i < blen; i++) s += body[i];
    return (uint16_t)(s & 0xFFFF);
}

// Assemble a command packet into out[].
// Packet layout: EF 01 | FF FF FF FF | PID_CMD | Length_H Length_L | INS params... | CS_H CS_L
// Length field = len(body) + 2  (body = INS + params; +2 for the two checksum bytes).
uint16_t FingerprintModule::_buildPacket(uint8_t ins, const uint8_t *params,
                                          uint8_t plen, uint8_t *out) {
    uint8_t  blen   = 1 + plen;          // body = INS + params
    uint16_t length = blen + 2;          // length field includes the 2 CS bytes
    uint8_t  lh     = length >> 8;
    uint8_t  ll     = length & 0xFF;

    // Assemble body into a local buffer to compute checksum
    uint8_t body[40];
    body[0] = ins;
    if (plen && params) memcpy(body + 1, params, plen);

    uint16_t cs = _checksum(PID_CMD, lh, ll, body, blen);

    uint16_t i = 0;
    out[i++] = 0xEF; out[i++] = 0x01;
    out[i++] = 0xFF; out[i++] = 0xFF; out[i++] = 0xFF; out[i++] = 0xFF;
    out[i++] = PID_CMD;
    out[i++] = lh; out[i++] = ll;
    memcpy(out + i, body, blen); i += blen;
    out[i++] = (uint8_t)(cs >> 8);
    out[i++] = (uint8_t)(cs & 0xFF);
    return i;
}

// Read one full response packet into buf[maxLen].
// Returns total bytes (includes header + length + body + checksum), or 0 on failure.
uint16_t FingerprintModule::_readPacket(uint8_t *buf, uint16_t maxLen, uint32_t timeoutMs) {
    uint32_t deadline = millis() + timeoutMs;
    uint16_t pos = 0;

    // Read 9 bytes: header(6) + PID(1) + Length(2)
    // Yield while no byte is pending — the ZW101 can take 100-200 ms to respond
    // to GETIMAGE/SEARCH, and a tight read loop in a high-priority task starves
    // every lower-priority task (including the LVGL render loop) for that whole
    // window, which shows up as visible animation stutter.
    while (pos < 9) {
        if (millis() > deadline) return 0;
        if (!_serial.available()) { delay(1); continue; }
        int b = _serial.read();
        if (b >= 0) buf[pos++] = (uint8_t)b;
    }
    if (buf[0] != 0xEF || buf[1] != 0x01) return 0;

    uint16_t length = ((uint16_t)buf[7] << 8) | buf[8];
    uint16_t total  = 9 + length;
    if (total > maxLen) return 0;

    while (pos < total) {
        if (millis() > deadline) return 0;
        if (!_serial.available()) { delay(1); continue; }
        int b = _serial.read();
        if (b >= 0) buf[pos++] = (uint8_t)b;
    }
    return total;
}

// Core send/receive. Flushes RX, sends command, reads response, validates checksum.
// Returns the confirm code (CC). 0xFF = timeout, 0xFE = parse/checksum error.
uint8_t FingerprintModule::_sendRecv(uint8_t ins, const uint8_t *params, uint8_t plen,
                                      uint8_t *dataOut, uint8_t &dataLen,
                                      uint32_t timeoutMs) {
    uint8_t txBuf[52];  // 9 header + 1 INS + up to 40 params + 2 CS
    uint16_t txLen = _buildPacket(ins, params, plen, txBuf);

    while (_serial.available()) _serial.read();  // discard stale bytes
    _serial.write(txBuf, txLen);
    _serial.flush();

    // rxBuf: header(9) + body + CS. Max body for any non-stream command is ~35 bytes.
    uint8_t rxBuf[80];
    uint16_t total = _readPacket(rxBuf, sizeof(rxBuf), timeoutMs);
    if (total < 12) { lastCC = 0xFF; dataLen = 0; return 0xFF; }

    uint8_t  pid    = rxBuf[6];
    uint16_t length = ((uint16_t)rxBuf[7] << 8) | rxBuf[8];
    uint16_t blen   = length - 2;           // body bytes (excludes the 2 CS bytes)

    const uint8_t *body = rxBuf + 9;
    uint16_t cs_recv = ((uint16_t)rxBuf[9 + blen] << 8) | rxBuf[9 + blen + 1];
    uint16_t cs_calc = _checksum(pid, rxBuf[7], rxBuf[8], body, blen);
    if (cs_calc != cs_recv || blen == 0) { lastCC = 0xFE; dataLen = 0; return 0xFE; }

    uint8_t cc   = body[0];
    uint8_t dlen = (uint8_t)(blen - 1);
    if (dataOut && dlen > 0) memcpy(dataOut, body + 1, dlen);
    dataLen = dlen;
    lastCC  = cc;
    return cc;
}

// ─── Data stream helpers (UpChar / DownChar) ──────────────────────────────────

// Receive PID=0x02/0x08 data packets following an UpChar ACK.
// Each packet: header(9) + data_chunk + CS(2). PID=0x08 signals end.
uint16_t FingerprintModule::_recvStream(uint8_t *out, uint16_t maxLen, uint32_t timeoutMs) {
    uint16_t received = 0;
    uint32_t deadline = millis() + timeoutMs;

    while (true) {
        uint8_t hdr[9];
        uint16_t pos = 0;
        while (pos < 9) {
            if (millis() > deadline) return received;
            if (!_serial.available()) { delay(1); continue; }
            int b = _serial.read();
            if (b >= 0) hdr[pos++] = (uint8_t)b;
        }
        if (hdr[0] != 0xEF || hdr[1] != 0x01) return received;

        uint8_t  pid    = hdr[6];
        uint16_t length = ((uint16_t)hdr[7] << 8) | hdr[8];
        if (length < 2) return received;
        uint16_t blen   = length - 2;   // chunk bytes (no CS)

        // Read chunk + 2 CS bytes
        uint8_t pktBuf[300];
        pos = 0;
        while (pos < length) {
            if (millis() > deadline) return received;
            if (!_serial.available()) { delay(1); continue; }
            int b = _serial.read();
            if (b >= 0) pktBuf[pos++] = (uint8_t)b;
        }

        uint16_t cs_recv = ((uint16_t)pktBuf[blen] << 8) | pktBuf[blen + 1];
        uint16_t cs_calc = _checksum(pid, hdr[7], hdr[8], pktBuf, blen);
        if (cs_recv != cs_calc) return received;

        if (received + blen > maxLen) return received;
        memcpy(out + received, pktBuf, blen);
        received += blen;

        if (pid == PID_END) break;
    }
    return received;
}

// Send data as PID=0x02 chunks + a final PID=0x08 end packet.
void FingerprintModule::_sendStream(const uint8_t *data, uint16_t len, uint16_t pktSize) {
    uint16_t offset = 0;
    while (offset < len) {
        uint16_t chunk  = (uint16_t)(len - offset);
        if (chunk > pktSize) chunk = pktSize;
        bool   isLast  = (offset + chunk >= len);
        uint8_t pid    = isLast ? PID_END : PID_DATA;

        uint16_t length = chunk + 2;
        uint8_t  lh = (uint8_t)(length >> 8);
        uint8_t  ll = (uint8_t)(length & 0xFF);
        uint16_t cs = _checksum(pid, lh, ll, data + offset, chunk);

        uint8_t pktBuf[9 + 256 + 2];
        uint16_t i = 0;
        pktBuf[i++] = 0xEF; pktBuf[i++] = 0x01;
        pktBuf[i++] = 0xFF; pktBuf[i++] = 0xFF; pktBuf[i++] = 0xFF; pktBuf[i++] = 0xFF;
        pktBuf[i++] = pid;
        pktBuf[i++] = lh; pktBuf[i++] = ll;
        memcpy(pktBuf + i, data + offset, chunk); i += chunk;
        pktBuf[i++] = (uint8_t)(cs >> 8);
        pktBuf[i++] = (uint8_t)(cs & 0xFF);
        _serial.write(pktBuf, i);
        offset += chunk;
    }
    _serial.flush();
}

// ─── Constructor / init ───────────────────────────────────────────────────────

FingerprintModule::FingerprintModule(HardwareSerial &serial,
                                     int rxPin, int txPin,
                                     int touchPin, int pwrPin)
    : _serial(serial), _rxPin(rxPin), _txPin(txPin),
      _touchPin(touchPin), _pwrPin(pwrPin) {}

bool FingerprintModule::begin(uint32_t baud) {
    if (_touchPin >= 0) pinMode(_touchPin, INPUT);
    // _pwrPin is deliberately left alone here — powerOn() owns it and has already
    // raised it. The pinMode(OUTPUT) that used to live on this line was what
    // actually switched the sensor on, 200 ms later than intended (see powerOn).
    _serial.begin(baud, SERIAL_8N1, _rxPin, _txPin);
    delay(FP_UART_SETTLE_MS);

    // Retry the handshake rather than trusting one packet: a dropped or garbled
    // response is not proof the module is absent — it may still be finishing its
    // power-on self-test.
    for (uint8_t i = 0; i < FP_HANDSHAKE_RETRIES; i++) {
        if (verifyPassword(0)) return true;
        delay(FP_HANDSHAKE_RETRY_MS);
    }
    return false;
}

void FingerprintModule::powerOn() {
    if (_pwrPin < 0) return;
    // pinMode() must precede digitalWrite(). On this core (Arduino-ESP32 2.0.9)
    // digitalWrite() is a bare gpio_set_level(): it sets the output register while
    // the pad's driver is still disabled, so the pin does not actually go HIGH
    // until begin() calls pinMode(OUTPUT). The settle below was therefore spent on
    // a rail that was still down, leaving the ZW101 only the 200 ms inside begin()
    // between VCC rising and the first packet — about its power-on-ready figure,
    // and the first thing to give way on a slower, battery-fed rail. Configuring
    // the pad first makes the settle real. Arduino-ESP32 3.x requires this order
    // outright: there, a digitalWrite() to an unregistered pad is silently dropped.
    pinMode(_pwrPin, OUTPUT);
    digitalWrite(_pwrPin, HIGH);
    delay(FP_POWER_SETTLE_MS);
}

void FingerprintModule::powerOff() {
    if (_pwrPin < 0) return;
    pinMode(_pwrPin, OUTPUT);   // keep the pad configured before writing (see powerOn)
    digitalWrite(_pwrPin, LOW);
}

bool FingerprintModule::isFingerPresent() {
    if (_touchPin >= 0) return digitalRead(_touchPin) == HIGH;
    // Fallback: send GetImage and check CC
    uint8_t dout[4]; uint8_t dlen = 0;
    _sendRecv(INS_GETIMAGE, nullptr, 0, dout, dlen, 1000);
    return lastCC == 0x00;
}

bool FingerprintModule::ping() {
    return verifyPassword(0);
}

void FingerprintModule::sleep() {
    // The EF-01 ZW101 has no UART sleep command. Cut VCC via power pin if wired.
    powerOff();
}

// ─── Password ─────────────────────────────────────────────────────────────────

bool FingerprintModule::verifyPassword(uint32_t pw) {
    uint8_t p[4] = {
        (uint8_t)(pw >> 24), (uint8_t)(pw >> 16),
        (uint8_t)(pw >> 8),  (uint8_t)(pw & 0xFF)
    };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_VERIFYPASSWORD, p, 4, dout, dlen) == 0x00;
}

bool FingerprintModule::setPassword(uint32_t newPw) {
    uint8_t p[4] = {
        (uint8_t)(newPw >> 24), (uint8_t)(newPw >> 16),
        (uint8_t)(newPw >> 8),  (uint8_t)(newPw & 0xFF)
    };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_SETPASSWORD, p, 4, dout, dlen) == 0x00;
}

// ─── Low-level image / buffer commands ───────────────────────────────────────

bool FingerprintModule::getImage() {
    uint8_t dout[4]; uint8_t dlen = 0;
    // Short timeout: module responds immediately with 0x00 or 0x02
    return _sendRecv(INS_GETIMAGE, nullptr, 0, dout, dlen, 1000) == 0x00;
}

bool FingerprintModule::image2Tz(uint8_t bufIdx) {
    uint8_t p[1] = { bufIdx };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_IMAGE2TZ, p, 1, dout, dlen) == 0x00;
}

bool FingerprintModule::createModel() {
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_REGMODEL, nullptr, 0, dout, dlen) == 0x00;
}

bool FingerprintModule::storeTemplate(uint8_t bufIdx, uint16_t id) {
    uint8_t p[3] = { bufIdx, (uint8_t)(id >> 8), (uint8_t)(id & 0xFF) };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_STORE, p, 3, dout, dlen) == 0x00;
}

bool FingerprintModule::loadTemplate(uint8_t bufIdx, uint16_t id) {
    uint8_t p[3] = { bufIdx, (uint8_t)(id >> 8), (uint8_t)(id & 0xFF) };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_LOAD, p, 3, dout, dlen) == 0x00;
}

// ─── Enrollment ───────────────────────────────────────────────────────────────

int16_t FingerprintModule::enrollFingerprint(uint16_t id, uint32_t timeoutMs) {
    uint32_t deadline = millis() + timeoutMs;

    // Auto-assign: find first free slot from storage map
    if (id == 0xFFFF) {
        bool states[50];
        if (getStorageMap(states)) {
            id = 0xFFFF;
            for (uint8_t i = 0; i < 50; i++) {
                if (!states[i]) { id = i; break; }
            }
        }
        if (id == 0xFFFF) { lastCC = 0x0B; return -1; }  // library full
    }

    // Scan 1 — wait for first finger press
    while (millis() < deadline) {
        if (getImage()) break;
        if (lastCC != 0x02) return -1;  // error other than "no finger"
        delay(20);
    }
    if (millis() >= deadline) return -1;

    if (!image2Tz(1)) return -1;

    // Wait for finger lift
    while (millis() < deadline) {
        getImage();
        if (lastCC == 0x02) break;
        delay(50);
    }
    delay(250);  // extra pause before second scan

    // Scan 2 — wait for second finger press
    while (millis() < deadline) {
        if (getImage()) break;
        if (lastCC != 0x02) return -1;
        delay(20);
    }
    if (millis() >= deadline) return -1;

    if (!image2Tz(2))   return -1;
    if (!createModel()) return -1;  // cc 0x0A = scans didn't match

    if (!storeTemplate(1, id)) return -1;

    return (int16_t)id;
}

// ─── Matching ─────────────────────────────────────────────────────────────────

int16_t FingerprintModule::matchFingerprint(uint16_t &score, uint32_t timeoutMs) {
    uint32_t deadline = millis() + timeoutMs;
    lastStage = FpStage::NONE;

    // Poll for finger — delay between attempts to yield CPU to the display task.
    // comm_errs absorbs isolated 0xFF/0xFE responses (see FP_COMM_RETRIES): one
    // glitched byte or a momentary dip on the sensor's rail used to abort the
    // whole verify, which made a marginal supply look like a dead sensor.
    bool     captured  = false;
    uint8_t  comm_errs = 0;
    while ((int32_t)(deadline - millis()) > 0) {
        if (getImage()) { captured = true; break; }
        if (lastCC == 0x02) {            // no finger on the pad yet
            comm_errs = 0;
            delay(20);
            continue;
        }
        if ((lastCC == 0xFF || lastCC == 0xFE) && ++comm_errs <= FP_COMM_RETRIES) {
            delay(FP_COMM_RETRY_MS);
            continue;
        }
        lastStage = FpStage::CAPTURE;    // real module-level error, or retries spent
        return -2;
    }
    // Distinguish a timeout from a frame that landed on the final poll: the old
    // `millis() >= deadline` recheck discarded a good capture taken right on the
    // deadline and reported it as an error.
    if (!captured) { lastStage = FpStage::CAPTURE; return -2; }

    if (!image2Tz(1)) { lastStage = FpStage::FEATURE; return -2; }

    // HiSpeedSearch across all 50 slots (count = 0x00A3 = 163, covers all with margin)
    uint8_t p[5] = { 0x01, 0x00, 0x00, 0x00, 0xA3 };
    uint8_t dout[8]; uint8_t dlen = 0;
    uint8_t cc = _sendRecv(INS_HISPEEDSEARCH, p, 5, dout, dlen);

    // Some firmware returns CC=0x00 but sends no payload on HiSpeedSearch — fall back
    if (cc == 0x00 && dlen < 4) {
        cc = _sendRecv(INS_SEARCH, p, 5, dout, dlen);
    }

    if (cc == 0x00 && dlen >= 4) {
        uint16_t matchId = ((uint16_t)dout[0] << 8) | dout[1];
        score = ((uint16_t)dout[2] << 8) | dout[3];
        return (int16_t)matchId;
    }
    if (cc == 0x09) return -1;      // 0x09 = no match against any stored template
    lastStage = FpStage::SEARCH;
    return -2;
}

// ─── Deletion ─────────────────────────────────────────────────────────────────

bool FingerprintModule::deleteFingerprint(uint16_t id) {
    uint8_t p[4] = { (uint8_t)(id >> 8), (uint8_t)(id & 0xFF), 0x00, 0x01 };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_DELETE, p, 4, dout, dlen) == 0x00;
}

bool FingerprintModule::deleteRange(uint16_t firstId, uint16_t lastId) {
    uint16_t count = lastId - firstId + 1;
    uint8_t p[4] = {
        (uint8_t)(firstId >> 8), (uint8_t)(firstId & 0xFF),
        (uint8_t)(count  >> 8),  (uint8_t)(count  & 0xFF)
    };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_DELETE, p, 4, dout, dlen) == 0x00;
}

bool FingerprintModule::deleteAllFingerprints() {
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_EMPTY, nullptr, 0, dout, dlen) == 0x00;
}

// ─── Template management ──────────────────────────────────────────────────────

bool FingerprintModule::templateExists(uint16_t id) {
    // LOAD into CharBuffer1 — cc 0x00 means template exists; 0x0C means not found
    return loadTemplate(1, id);
}

uint16_t FingerprintModule::getTemplateCount() {
    uint8_t dout[4]; uint8_t dlen = 0;
    if (_sendRecv(INS_TEMPLATECOUNT, nullptr, 0, dout, dlen) != 0x00 || dlen < 2) return 0;
    return ((uint16_t)dout[0] << 8) | dout[1];
}

bool FingerprintModule::getStorageMap(bool states[50]) {
    uint8_t p[1] = { 0x00 };   // page 0
    uint8_t dout[36]; uint8_t dlen = 0;
    if (_sendRecv(INS_READ_INDEX, p, 1, dout, dlen) != 0x00) return false;
    // Need at least 7 bytes to cover all 50 slots (50 / 8 = 6.25 → 7 bytes)
    if (dlen < 7) return false;
    for (uint8_t i = 0; i < 50; i++) {
        uint8_t byteIdx = i / 8;
        uint8_t bitIdx  = i % 8;
        states[i] = (byteIdx < dlen) && (dout[byteIdx] & (1 << bitIdx));
    }
    return true;
}

bool FingerprintModule::exportTemplate(uint16_t id, uint8_t *buf,
                                       uint16_t &len, uint16_t maxLen) {
    // Load template from flash into CharBuffer1
    if (!loadTemplate(1, id)) return false;

    // Send UpChar command manually (need to receive data stream immediately after ACK)
    uint8_t upParam[1] = { 0x01 };
    uint8_t txBuf[16];
    uint16_t txLen = _buildPacket(INS_UPCHAR, upParam, 1, txBuf);

    while (_serial.available()) _serial.read();
    _serial.write(txBuf, txLen);
    _serial.flush();

    // Read UpChar ACK
    uint8_t rxBuf[32];
    uint16_t total = _readPacket(rxBuf, sizeof(rxBuf), 3000);
    if (total == 0) return false;

    uint8_t  pid    = rxBuf[6];
    uint16_t length = ((uint16_t)rxBuf[7] << 8) | rxBuf[8];
    uint16_t blen   = length - 2;
    const uint8_t *body = rxBuf + 9;
    uint16_t cs_recv = ((uint16_t)rxBuf[9 + blen] << 8) | rxBuf[9 + blen + 1];
    uint16_t cs_calc = _checksum(pid, rxBuf[7], rxBuf[8], body, blen);
    if (cs_calc != cs_recv || blen == 0 || body[0] != 0x00) return false;

    // Receive data stream
    len = _recvStream(buf, maxLen);
    return len > 0;
}

bool FingerprintModule::importTemplate(uint16_t id, const uint8_t *buf, uint16_t len) {
    // Send DownChar command
    uint8_t downParam[1] = { 0x01 };
    uint8_t txBuf[16];
    uint16_t txLen = _buildPacket(INS_DOWNCHAR, downParam, 1, txBuf);

    while (_serial.available()) _serial.read();
    _serial.write(txBuf, txLen);
    _serial.flush();

    // Read DownChar ACK
    uint8_t rxBuf[32];
    uint16_t total = _readPacket(rxBuf, sizeof(rxBuf), 3000);
    if (total == 0) return false;

    uint8_t  pid    = rxBuf[6];
    uint16_t length = ((uint16_t)rxBuf[7] << 8) | rxBuf[8];
    uint16_t blen   = length - 2;
    const uint8_t *body = rxBuf + 9;
    uint16_t cs_recv = ((uint16_t)rxBuf[9 + blen] << 8) | rxBuf[9 + blen + 1];
    uint16_t cs_calc = _checksum(pid, rxBuf[7], rxBuf[8], body, blen);
    if (cs_calc != cs_recv || blen == 0 || body[0] != 0x00) return false;

    // Send template data stream then store
    _sendStream(buf, len, 128);
    return storeTemplate(1, id);
}

// ─── System settings ──────────────────────────────────────────────────────────

bool FingerprintModule::readSysParam(uint16_t *capacity, uint8_t *secLevel,
                                      uint8_t *pktIdx, uint8_t *baudN) {
    uint8_t dout[20]; uint8_t dlen = 0;
    if (_sendRecv(INS_READSYSPARAM, nullptr, 0, dout, dlen) != 0x00 || dlen < 16) return false;
    if (capacity) *capacity = ((uint16_t)dout[4]  << 8) | dout[5];
    if (secLevel) *secLevel = (uint8_t)(((uint16_t)dout[6]  << 8) | dout[7]);
    if (pktIdx)   *pktIdx   = (uint8_t)(((uint16_t)dout[12] << 8) | dout[13]);
    if (baudN)    *baudN     = (uint8_t)(((uint16_t)dout[14] << 8) | dout[15]);
    return true;
}

bool FingerprintModule::setSecurityLevel(uint8_t level) {
    uint8_t p[2] = { 0x05, level };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_WRITE_REG, p, 2, dout, dlen) == 0x00;
}

bool FingerprintModule::setBaudReg(uint8_t n) {
    uint8_t p[2] = { 0x04, n };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_WRITE_REG, p, 2, dout, dlen) == 0x00;
}

bool FingerprintModule::setPacketSize(uint8_t idx) {
    uint8_t p[2] = { 0x06, idx };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_WRITE_REG, p, 2, dout, dlen) == 0x00;
}

// ─── LED ──────────────────────────────────────────────────────────────────────

bool FingerprintModule::ledAura(uint8_t func, uint8_t color, uint8_t cycles) {
    uint8_t p[4] = { func, color, color, cycles };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_AURALEDCONFIG, p, 4, dout, dlen) == 0x00;
}

bool FingerprintModule::ledOff() {
    // HLK-ZW101 does not support INS_LEDOFF (0x51); use AURALEDCONFIG func=4, color=0x00
    uint8_t p[4] = { FP_LED_FUNC_OFF, FP_LED_OFF, FP_LED_OFF, 0 };
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_AURALEDCONFIG, p, 4, dout, dlen) == 0x00;
}

bool FingerprintModule::ledOn() {
    uint8_t dout[4]; uint8_t dlen = 0;
    return _sendRecv(INS_LEDON, nullptr, 0, dout, dlen) == 0x00;
}

bool FingerprintModule::ledBreathing(uint8_t color) {
    return ledAura(FP_LED_FUNC_BREATHING, color, 0);  // 0 = infinite
}

bool FingerprintModule::ledFlash(uint8_t color, uint8_t cycles) {
    return ledAura(FP_LED_FUNC_FLASH, color, cycles);
}

bool FingerprintModule::ledSteady(uint8_t color) {
    return ledAura(FP_LED_FUNC_STEADY, color, 0);
}

bool FingerprintModule::ledGradOpen(uint8_t color) {
    return ledAura(FP_LED_FUNC_GRAD_OPEN, color, 0);
}

bool FingerprintModule::ledGradClose(uint8_t color) {
    return ledAura(FP_LED_FUNC_GRAD_CLOSE, color, 0);
}
