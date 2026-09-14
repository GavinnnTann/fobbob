#include "fp_wrapper.h"
#include "hardware.h"
#include "config.h"

static FingerprintModule s_fp(Serial2, FP_UART_RX, FP_UART_TX, FP_TOUCH_PIN, FP_CTRL_PIN);

namespace {
    enum class EnrollPhase : uint8_t { SCAN1, WAIT_LIFT, SCAN2 };
    EnrollPhase s_phase = EnrollPhase::SCAN1;

#ifdef DEBUG_SERIAL
    const char* stage_name(FpStage s) {
        switch (s) {
            case FpStage::CAPTURE: return "CAPTURE";
            case FpStage::FEATURE: return "FEATURE";
            case FpStage::SEARCH:  return "SEARCH";
            default:               return "NONE";
        }
    }
#endif
}

bool fp_init(uint32_t timeoutMs) {
    (void)timeoutMs;
    s_fp.powerOn();
    bool ok = s_fp.begin(FP_UART_BAUD);
#ifdef DEBUG_SERIAL
    // lastCC 0xFF = nothing came back at all — sensor unpowered, U14 not closing,
    // or the UART is miswired. 0xFE = a reply arrived but failed its checksum,
    // which points at signal integrity or a sagging rail rather than no power.
    if (!ok) Serial.printf("[FP] init FAILED — no handshake (lastCC=0x%02X)\n", s_fp.lastCC);
#endif
    return ok;
}

void fp_sleep() {
    s_fp.sleep();
}

FpResult fp_verify(uint8_t *matched_id, uint32_t timeoutMs) {
    uint16_t score = 0;
    int16_t id = s_fp.matchFingerprint(score, timeoutMs);
    if (id >= 0) {
        if (matched_id) *matched_id = (uint8_t)id;
        return FpResult::OK;
    }
    if (id == -1) return FpResult::NO_MATCH;
#ifdef DEBUG_SERIAL
    Serial.printf("[FP] verify ERROR — stage=%s lastCC=0x%02X\n",
                  stage_name(s_fp.lastStage), s_fp.lastCC);
#endif
    return FpResult::ERROR;
}

void fp_enroll_reset() {
    s_phase = EnrollPhase::SCAN1;
}

// Non-blocking tick — call once per main-loop iteration.
// Each call issues one GetImage command and returns the current enrollment state.
FpEnrollStep fp_enroll_tick(uint8_t slot) {
    switch (s_phase) {

        case EnrollPhase::SCAN1:
            if (!s_fp.getImage()) {
                // cc 0x02 = no finger yet; anything else is a hardware error
                if (s_fp.lastCC == 0x02) return FpEnrollStep::PLACE_FIRST;
                return FpEnrollStep::FAILED;
            }
            if (!s_fp.image2Tz(1)) {
                s_phase = EnrollPhase::SCAN1;
                return FpEnrollStep::FAILED;
            }
            s_phase = EnrollPhase::WAIT_LIFT;
            return FpEnrollStep::LIFT_FINGER;

        case EnrollPhase::WAIT_LIFT:
            s_fp.getImage();
            if (s_fp.lastCC == 0x02) {
                s_phase = EnrollPhase::SCAN2;
                return FpEnrollStep::PLACE_SECOND;
            }
            return FpEnrollStep::LIFT_FINGER;

        case EnrollPhase::SCAN2:
            if (!s_fp.getImage()) {
                if (s_fp.lastCC == 0x02) return FpEnrollStep::PLACE_SECOND;
                s_phase = EnrollPhase::SCAN1;
                return FpEnrollStep::FAILED;
            }
            if (!s_fp.image2Tz(2) || !s_fp.createModel() ||
                !s_fp.storeTemplate(1, (uint16_t)slot)) {
                s_phase = EnrollPhase::SCAN1;
                return FpEnrollStep::FAILED;
            }
            s_phase = EnrollPhase::SCAN1;
            return FpEnrollStep::DONE;
    }
    return FpEnrollStep::FAILED;
}

bool fp_delete(uint8_t id) {
    return s_fp.deleteFingerprint((uint16_t)id);
}

bool fp_delete_all() {
    return s_fp.deleteAllFingerprints();
}

uint8_t fp_count() {
    uint16_t n = s_fp.getTemplateCount();
    if (s_fp.lastCC != 0x00) return 0xFF;
    return (n > 254) ? 0xFE : (uint8_t)n;
}

bool fp_get_storage_map(bool* occupied, uint8_t len) {
    bool states[50] = {};
    if (!s_fp.getStorageMap(states)) return false;
    for (uint8_t i = 0; i < len; i++) occupied[i] = (i < 50) && states[i];
    return true;
}

// Retry once if the command fails: a dropped/garbled ledOff leaves the sensor on
// its default breathing aura, which then lingers on the TOTP screen. One resend
// covers an occasional desync without adding noticeable latency.
void fp_led_off()          { if (!s_fp.ledOff()) s_fp.ledOff(); }
void fp_led_steady_blue()  { s_fp.ledSteady(FP_LED_BLUE); }
void fp_led_breathe_blue() { s_fp.ledBreathing(FP_LED_BLUE); }
