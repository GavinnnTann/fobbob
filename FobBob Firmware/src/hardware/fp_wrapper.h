#pragma once
#include <fingerprint.h>

// ─── FobBob C-style fingerprint API ──────────────────────────────────────────
// Single global FingerprintModule bound to the hardware pins in hardware.h.
// Call fp_init() before any other fp_* function.

bool         fp_init(uint32_t timeoutMs = 2000);
void         fp_sleep();

// Blocking verify. Returns OK on match, NO_MATCH if not found, ERROR on comm fail.
FpResult     fp_verify(uint8_t *matched_id, uint32_t timeoutMs);

// Non-blocking enroll state machine — call once per loop() iteration.
// Call fp_enroll_reset() before starting each new enrollment sequence.
void         fp_enroll_reset();
FpEnrollStep fp_enroll_tick(uint8_t slot);  // slot = target NVS fingerprint index

bool         fp_delete(uint8_t id);
bool         fp_delete_all();   // EMPTY — wipe every template from the sensor (factory reset)
uint8_t      fp_count();    // 0xFF = comm error
// Fill occupied[0..len-1] with which sensor pages actually have templates.
// Returns false on comm error.
bool         fp_get_storage_map(bool* occupied, uint8_t len);

void         fp_led_off();
void         fp_led_steady_blue();   // solid blue — fingerprint wake idle
void         fp_led_breathe_blue();  // infinite blue breathing — BLE provisioning
