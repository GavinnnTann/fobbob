#include "buttons.h"
#include "hardware/hardware.h"
#include "config.h"
#include <Wire.h>

static const uint32_t DEBOUNCE_MS      = 30;
static const uint32_t BTN_REPEAT_DELAY = 450;  // hold this long before auto-repeat starts
static const uint32_t BTN_REPEAT_MS    = 300;  // step interval while held — a readable run-through
static constexpr uint8_t TCA6408_ADDR  = 0x20;

struct BtnState {
    bool     last_raw;
    bool     debounced;
    uint32_t last_change_ms;
};

static BtnState s_btn1 = {};
static BtnState s_btn2 = {};
static BtnState s_btn_pwr = {};
static bool     s_chord_fired = false;
static uint32_t s_chord_start_ms = 0;

// Per-button auto-repeat tracking (hold = run through accounts at BTN_REPEAT_MS).
static uint32_t s_btn1_down_ms = 0;
static uint32_t s_btn2_down_ms = 0;
static uint32_t s_btn1_rep_ms  = 0;
static uint32_t s_btn2_rep_ms  = 0;

// Read TCA6408 input port register (0x00). Returns 0xFF on I²C error (all not-pressed).
static uint8_t tca6408_read_port_internal() {
    Wire.beginTransmission(TCA6408_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    Wire.requestFrom(TCA6408_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static void update_btn(BtnState& state, bool raw) {
    if (raw != state.last_raw) {
        state.last_raw = raw;
        state.last_change_ms = millis();
    }
    if ((millis() - state.last_change_ms) >= DEBOUNCE_MS) {
        state.debounced = raw;
    }
}

void buttons_init() {
    // Set all TCA6408 pins as inputs (config register 0x03 = 0xFF)
    Wire.beginTransmission(TCA6408_ADDR);
    Wire.write(0x03);
    Wire.write(0xFF);
    Wire.endTransmission();

    uint8_t port = tca6408_read_port_internal();
    bool b1  = !(port & (1 << BTN1_BIT));
    bool b2  = !(port & (1 << BTN2_BIT));
    bool bpw = !(port & (1 << BTN_PWR_BIT));
    s_btn1    = { b1,  b1,  millis() };
    s_btn2    = { b2,  b2,  millis() };
    s_btn_pwr = { bpw, bpw, millis() };
}

bool buttons_chord_active() {
    uint8_t port = tca6408_read_port_internal();
    return !(port & (1 << BTN1_BIT)) && !(port & (1 << BTN2_BIT));
}

uint8_t buttons_read_port() {
    return tca6408_read_port_internal();
}

ButtonEvent buttons_tick() {
    bool prev1   = s_btn1.debounced;
    bool prev2   = s_btn2.debounced;
    bool prev_pw = s_btn_pwr.debounced;

    uint8_t port = tca6408_read_port_internal();
    update_btn(s_btn1,    !(port & (1 << BTN1_BIT)));
    update_btn(s_btn2,    !(port & (1 << BTN2_BIT)));
    update_btn(s_btn_pwr, !(port & (1 << BTN_PWR_BIT)));

    bool     both = s_btn1.debounced && s_btn2.debounced;
    uint32_t now  = millis();

    // Chord: both UP+DOWN held >= BTN_CHORD_MS
    if (both) {
        if (s_chord_start_ms == 0) s_chord_start_ms = millis();
        if (!s_chord_fired && (now - s_chord_start_ms) >= BTN_CHORD_MS) {
            s_chord_fired = true;
            return ButtonEvent::CHORD_HELD;
        }
    } else {
        s_chord_start_ms = 0;
        s_chord_fired    = false;
    }

    // SW_Power single tap — independent of chord / hold detection
    if (!prev_pw && s_btn_pwr.debounced)     return ButtonEvent::BTN_PWR_PRESS;

    // Single-button handling only when the other isn't also down (that's a chord).
    // One step on the press edge, then auto-repeat after BTN_REPEAT_DELAY so a held
    // button runs through accounts at BTN_REPEAT_MS — release to stop where you are.
    if (!both) {
        // BTN1
        if (s_btn1.debounced && !prev1) {
            s_btn1_down_ms = now; s_btn1_rep_ms = now;
            return ButtonEvent::BTN1_PRESS;
        }
        if (s_btn1.debounced && prev1 &&
            (now - s_btn1_down_ms) >= BTN_REPEAT_DELAY &&
            (now - s_btn1_rep_ms)  >= BTN_REPEAT_MS) {
            s_btn1_rep_ms = now;
            return ButtonEvent::BTN1_PRESS;
        }
        // BTN2
        if (s_btn2.debounced && !prev2) {
            s_btn2_down_ms = now; s_btn2_rep_ms = now;
            return ButtonEvent::BTN2_PRESS;
        }
        if (s_btn2.debounced && prev2 &&
            (now - s_btn2_down_ms) >= BTN_REPEAT_DELAY &&
            (now - s_btn2_rep_ms)  >= BTN_REPEAT_MS) {
            s_btn2_rep_ms = now;
            return ButtonEvent::BTN2_PRESS;
        }
    }

    return ButtonEvent::NONE;
}
