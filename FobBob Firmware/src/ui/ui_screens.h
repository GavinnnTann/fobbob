#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Lifecycle ────────────────────────────────────────────────────────────────
// Call once in setup() after display_init().
void ui_screens_init(void);

// Call on a 1-second interval from loop() — updates TOTP, arc, datetime, timeout.
void ui_tick(void);

// ── Screen transitions — call from main task only ─────────────────────────
void ui_show_splash(void);        // call immediately after display_init(); auto-dismissed by next screen load
void ui_show_ble_provision(void);
void ui_show_fp_enroll(void);
void ui_show_fp_verify(void);
void ui_show_totp(uint8_t account_count);

// ── Data updates — call from main task only ───────────────────────────────
void ui_totp_update(uint8_t idx, const char *name, uint32_t code, uint8_t secs_remaining);
void ui_totp_next(void);
void ui_totp_prev(void);
void ui_totp_toggle_hide(void);  // toggle masking the displayed code (privacy)

// Flash a brief green WiFi glyph on the TOTP screen after a successful background
// NTP sync (auto-hides after a few seconds). No-op if TOTP isn't built yet.
void ui_totp_set_synced(void);

// Play the charging animation once as a modal overlay on the TOTP screen.
// No-op if TOTP isn't the active screen or a charging modal is already showing.
void ui_totp_show_charging(int pct);

void ui_ble_set_status(const char *msg);

void ui_fp_enroll_update(uint8_t captures_done, bool success);
void ui_fp_verify_result(bool matched);

// True while a tick/cross result animation is currently playing.
bool ui_fp_result_active(void);

// Override the fingerprint screen's status text (e.g. reprovision gate prompt).
void ui_fp_set_status(const char *msg);

// Restart the idle fingerprint Lottie from frame 0 and play exactly one cycle,
// then stop. Call when the user touches the sensor. No-op if Lottie didn't load.
void     ui_fp_anim_trigger(void);

// Duration of one idle Lottie cycle in ms (fallback 1200 if not loaded).
uint32_t ui_fp_anim_duration_ms(void);

// Reset display-off timer; call on any user input.
void ui_activity(void);

// Set the TOTP screen sleep timeout. Call once on boot with nvs_get_display_sec().
// Range enforced: TOTP_DISPLAY_SEC_MIN .. TOTP_DISPLAY_SEC_MAX.
void ui_set_display_timeout(uint16_t sec);

// ── Settings screen (touch) ───────────────────────────────────────────────
// Result of activating (tapping) the currently-selected settings item.
typedef enum {
    UI_SETTINGS_NONE,         // a toggle/value/focus change — stay on screen, give feedback
    UI_SETTINGS_IGNORED,      // tap hit nothing actionable (battery row / empty) — no feedback
    UI_SETTINGS_REPROVISION,  // user activated "Reprovision (BLE)"
    UI_SETTINGS_SYNC_NOW,     // user tapped "Sync now" — caller forces a WiFi/NTP sync
    UI_SETTINGS_EXIT,         // user activated "Exit" — caller should save + return to TOTP
} UiSettingsAction;

// Build + show the settings screen, seeded with current values. The screen is a
// center-locked picker: one fixed focus slot at the viewport centre, options
// scroll under it like a wheel, and whichever option sits in the focus slot is
// the "selected" row.
void ui_show_settings(bool tap_sleep, bool adaptive, bool orient_flip,
                      bool haptics, uint16_t disp_sec, bool wifi_enabled);

// Step the focus by one option (dir > 0 → next/down, dir < 0 → prev/up), animated.
// Returns the newly focused row index (caller fires the per-detent haptic).
int ui_settings_nav(int dir);

// Activate the focused (centre) row: toggles bools / cycles timeout / returns
// action. Hold-to-trigger rows (Reprovision) return UI_SETTINGS_NONE — they are
// driven by the caller's hold gesture, not a tap.
UiSettingsAction ui_settings_select(void);

// Direct touch. Given a tap point in screen coordinates (0..239, already
// de-rotated by the caller): if it lands on the focused (centre) row, activate it;
// otherwise scroll the tapped row into the focus slot (select, don't toggle).
// Taps outside the list viewport do nothing (return UI_SETTINGS_NONE).
UiSettingsAction ui_settings_tap(int x, int y);

// Live drag: scroll the wheel by dy pixels immediately (finger-follows-content),
// clamped so the first/last option can travel no further than the focus slot.
// Updates the focused row from the new scroll position and returns its index, so
// the caller can fire a haptic detent whenever the focused row changes.
int ui_settings_scroll_by(int dy);

// Release: snap the nearest option exactly into the focus slot (animated).
void ui_settings_snap(void);

// The row currently in the focus slot. Does not change selection.
int ui_settings_centered_row(void);

// Closest-anchor row index under a screen-coordinate point (0..239), or -1 if
// the point is outside the visible list. Does not change selection.
int ui_settings_row_at(int x, int y);

// True if the given row is a hold-to-trigger action (Reprovision).
bool ui_settings_row_is_hold(int row);

// Set the hold-to-trigger fill on the Reprovision row, 0..100 percent.
// 0 hides the fill (press cancelled); 100 = fully committed.
void ui_settings_set_hold_progress(int pct);

// Read back the (possibly edited) values. Call on UI_SETTINGS_EXIT before saving.
// Any out-param may be null.
void ui_settings_get(bool* tap_sleep, bool* adaptive, bool* orient_flip,
                     bool* haptics, uint16_t* disp_sec, bool* wifi_enabled);

// Live-update the "Sync now" row value — also the WiFi status indicator.
// `state`: 0 neutral, 1 ok (green + WiFi glyph), 2 fail (red + WiFi glyph),
// 3 busy (blue). Call each second while the settings screen is open.
void ui_settings_set_sync_status(const char* text, int state);

// True when the WiFi row is the focused (centre) row.
bool ui_settings_focused_is_wifi(void);

// ── WiFi credentials page ───────────────────────────────────────────────────
// Full-screen view of the stored WiFi SSID + password, opened by holding the
// WiFi row in settings. Dismissed by the caller on any tap/swipe.
void ui_show_wifi_info(const char* ssid, const char* pass);
bool ui_wifi_info_active(void);

// Update the (display-only) battery status row. pct 0..100; charging shows a
// bolt + green text, else colour grades amber < 20% and red < 5%.
void ui_settings_set_battery(int pct, bool charging);

// True while the settings screen is the active screen.
bool ui_settings_active(void);

// True while the TOTP screen is the active screen (codes are showing).
// Used to gate touch handling so swipes/long-press are ignored on FP/BLE screens.
bool ui_totp_active(void);

#ifdef __cplusplus
}
#endif
