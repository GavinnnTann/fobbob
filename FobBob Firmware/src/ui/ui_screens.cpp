#include "ui_screens.h"
#include "ui.h"
#include "screens/ui_Screen1.h"
#include "../nvs/nvs_manager.h"
#include "../totp/totp_engine.h"
#include "../display/display.h"
#include "../power/power.h"
#include "config.h"
#include "assets/ble_anim.h"
#include "assets/fobbob_icon.h"
#include "assets/fp_anim.h"      // idle: fingerprint trim-path (4.8KB, 1 layer)
#include "assets/tick_anim.h"    // success: tick animation (2.9KB, 2 layers)
#include "assets/cross_anim.h"   // failure: cross animation (4.2KB, 3 layers)
#include "assets/battery_charging_anim.h"  // charging modal (plays once on plug-in)
#include <time.h>
#include <lvgl.h>
#include <Arduino.h>
// Private LVGL + ThorVG headers — needed to read the actual animation duration
// and correct lv_lottie_set_src_data's hardcoded 60 fps assumption.
#include <src/widgets/lottie/lv_lottie_private.h>

// ── State ─────────────────────────────────────────────────────────────────────

static uint8_t   s_account_count       = 0;
static uint8_t   s_current_idx         = 0;
static uint32_t  s_last_activity_ms    = 0;
static uint32_t  s_display_timeout_ms  = TOTP_DISPLAY_SEC_DEFAULT * 1000;
static bool      s_code_hidden         = false;   // privacy mask — toggled by a screen tap

// TOTP tileview — one vertical tile per account; arc/name/time stay static.
static lv_obj_t *s_tileview     = nullptr;
static lv_obj_t *s_tile_account[TOTP_MAX_ACCOUNTS] = {};
static lv_obj_t *s_tile_code[TOTP_MAX_ACCOUNTS]    = {};
static lv_obj_t *s_code_mask    = nullptr;   // ONE shared privacy dot-mask overlay (not per-tile)
static uint8_t   s_built_count  = 0;   // tiles currently built (>=1 once built)

// Vertical page-dot indicator (Instagram-style sliding window, max 5 visible).
// s_dots_view is a fixed clipping window; s_dots_strip slides inside it so the
// active dot stays at the window centre while the strip moves underneath.
static lv_obj_t *s_dots_view    = nullptr;
static lv_obj_t *s_dots_strip   = nullptr;
static lv_obj_t *s_dots[TOTP_MAX_ACCOUNTS] = {};
static uint8_t   s_dot_count    = 0;

static constexpr int DOT_SIZE     = 8;    // inactive dot diameter
static constexpr int DOT_ACTIVE_H = 14;   // active dot pill height
static constexpr int DOT_PITCH    = 15;   // centre-to-centre spacing
static constexpr int DOT_WINDOW   = 5;    // max dots visible at once
static constexpr int DOT_VIEW_W   = 16;

// BLE provision screen
static lv_obj_t *s_screen_ble     = nullptr;
static lv_obj_t *s_lbl_ble_status = nullptr;
static lv_obj_t *s_lottie_ble     = nullptr;  // tracked so we can delete it before creating FP canvas

// Shared Lottie render buffer (PSRAM, allocated once on first use).
// 120×120×4 = 57,600 bytes — enough for BLE (120×120) and FP results (120×120).
// Each Lottie widget gets its own PSRAM buffer. ThorVG's SwCanvas holds a target
// pointer for each canvas; sharing one buffer across multiple canvases causes ThorVG
// to deadlock when a second canvas tries to draw into an already-owned buffer.
// 120×120×4 = 57,600 bytes each — 172KB total, trivial on 16MB PSRAM.
static uint8_t  *s_lottie_buf         = nullptr;  // BLE animation buffer
static constexpr int   BLE_LOTTIE_SIZE   = 120;
static constexpr int   FP_LOTTIE_SIZE    = 120;
static constexpr size_t LOTTIE_BUF_BYTES = (size_t)FP_LOTTIE_SIZE * FP_LOTTIE_SIZE * 4; // 57,600

// Fingerprint screen (shared for enroll and verify)
static lv_obj_t *s_screen_fp      = nullptr;
static lv_obj_t *s_lbl_fp_status  = nullptr;
static lv_obj_t *s_ring_fp        = nullptr;  // dim background ring (fallback only)
static lv_obj_t *s_arc_fp         = nullptr;  // fallback arc (shown if fp_anim fails to load)
static lv_obj_t *s_icon_fp        = nullptr;  // "FP" label (fallback only)
static lv_obj_t *s_lottie_fp_idle = nullptr;  // fingerprint idle animation (looping)
static lv_obj_t  *s_lottie_result  = nullptr;  // lazily-created result animation (tick or cross)
static uint8_t   *s_fp_idle_buf    = nullptr;
static uint8_t   *s_fp_result_buf  = nullptr;  // shared PSRAM buffer reused for each result Lottie
static lv_timer_t *s_fp_result_timer = nullptr; // pending transition timer — cancelled on new result

// ── Internal helpers ──────────────────────────────────────────────────────────

// Returns the accent colour for the current seconds-remaining value.
// Thresholds: ≤5s → red, else → blue (no amber).
static lv_color_t accent_color(uint8_t secs_remaining) {
    if (secs_remaining <= 5) return lv_color_hex(0xF44336);   // red
    return lv_color_hex(0x2196F3);                             // blue
}

static void arc_set_color(uint8_t secs_remaining) {
    lv_obj_set_style_arc_color(ui_Scroll_bar, accent_color(secs_remaining),
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

static void code_set_color(uint8_t secs_remaining) {
    lv_obj_set_style_text_color(ui_Code, accent_color(secs_remaining),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void update_arc(uint8_t secs_remaining) {
    lv_arc_set_value(ui_Scroll_bar, secs_remaining);
    arc_set_color(secs_remaining);
}

// ── Red-zone flash ───────────────────────────────────────────────────────────
// At ≤5s the code and arc turn red. The code also flashes between full red
// and a grey-scaled version (rather than blanking) so it remains readable.
// 600 ms period (~0.8 Hz) — slow enough to read on each flash.
static lv_timer_t *s_flash_timer = nullptr;
static bool        s_flash_on    = true;

static void flash_timer_cb(lv_timer_t *) {
    if (!ui_Screen1 || lv_screen_active() != ui_Screen1 || !ui_Code) return;

    uint8_t secs = totp_seconds_remaining(time(nullptr));
    if (secs <= 5 && s_account_count > 0) {
        s_flash_on = !s_flash_on;
        lv_color_t col = s_flash_on ? lv_color_hex(0xF44336)   // full red
                                    : lv_color_hex(0x888888);   // grey (dimmed, still readable)
        lv_obj_set_style_text_color(ui_Code, col, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else if (!s_flash_on) {
        // Exiting the red zone on a new TOTP period — restore full-brightness accent.
        s_flash_on = true;
        code_set_color(secs);
    }
}

static void update_datetime(void) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(ui_Time, buf);
}

static void refresh_current_account(void) {
    if (!ui_Screen1) return;

    // Repoint the shared label handles at the active tile so all existing
    // update paths (arc colour, red-zone flash, ui_totp_update) keep working.
    if (s_tileview && s_current_idx < s_built_count) {
        ui_Account = s_tile_account[s_current_idx];
        ui_Code    = s_tile_code[s_current_idx];
    }
    if (!ui_Account || !ui_Code) return;

    if (s_account_count == 0) {
        lv_label_set_text(ui_Account, "No accounts");
        lv_label_set_text(ui_Code, "--- ---");
        lv_arc_set_value(ui_Scroll_bar, 30);
        arc_set_color(30);
        code_set_color(30);
        update_datetime();
        return;
    }

    time_t now = time(nullptr);
    uint8_t secs = totp_seconds_remaining(now);
    TotpAccount acct;
    if (nvs_get_totp_account(s_current_idx, &acct)) {
        lv_label_set_text(ui_Account, acct.name);
        uint32_t code = totp_from_base32(acct.secret_b32, now);
        if (code != UINT32_MAX) {
            // Format as "123 456" for readability.
            char buf[8];
            snprintf(buf, sizeof(buf), "%03lu %03lu",
                     (unsigned long)(code / 1000),
                     (unsigned long)(code % 1000));
            lv_label_set_text(ui_Code, buf);
        } else {
            lv_label_set_text(ui_Code, "--- ---");
        }
    }
    update_arc(secs);
    code_set_color(secs);   // keep code colour in sync with arc (flash_timer overrides in red zone)
    update_datetime();
}

static lv_obj_t *make_screen_with_bg(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    return scr;
}

// ── TOTP tileview + page-dot indicator ───────────────────────────────────────

// Build one vertical tile per account (min 1, for the "No accounts" state).
// Tiles and the tileview itself are fully transparent so the static arc,
// datetime and device-name widgets show through while content slides.
static void totp_build_tiles(uint8_t count) {
    uint8_t tiles = (count > 0) ? count : 1;
    if (tiles > TOTP_MAX_ACCOUNTS) tiles = TOTP_MAX_ACCOUNTS;
    if (s_tileview && tiles == s_built_count) return;   // already correct — reuse

    if (s_tileview) {
        lv_obj_delete(s_tileview);
        s_tileview = nullptr;
    }

    s_tileview = lv_tileview_create(ui_Screen1);
    lv_obj_set_size(s_tileview, 240, 240);
    lv_obj_set_style_bg_opa(s_tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tileview, 0, 0);
    lv_obj_set_style_pad_all(s_tileview, 0, 0);
    lv_obj_set_scrollbar_mode(s_tileview, LV_SCROLLBAR_MODE_OFF);

    for (uint8_t i = 0; i < tiles; i++) {
        lv_obj_t *tile = lv_tileview_add_tile(s_tileview, 0, i, LV_DIR_VER);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);

        lv_obj_t *acct = lv_label_create(tile);
        lv_obj_set_align(acct, LV_ALIGN_CENTER);
        lv_obj_set_y(acct, -35);
        lv_label_set_text(acct, "");
        lv_obj_set_style_text_font(acct, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(acct, lv_color_white(), 0);
        s_tile_account[i] = acct;

        lv_obj_t *code = lv_label_create(tile);
        lv_obj_set_align(code, LV_ALIGN_CENTER);
        lv_obj_set_y(code, 0);
        lv_label_set_text(code, "--- ---");
        lv_obj_set_style_text_font(code, &lv_font_montserrat_40, 0);
        lv_obj_set_style_text_color(code, lv_color_hex(0x007EF2), 0);
        s_tile_code[i] = code;
    }
    s_built_count = tiles;
    ui_Account = s_tile_account[0];
    ui_Code    = s_tile_code[0];

    // ONE shared privacy mask for the whole screen — 6 big dots (3 + 3) centred
    // over the code. Built once on ui_Screen1 (not per tile) so the LVGL object
    // count stays flat regardless of account count; per-tile masks exhausted the
    // 64 KB LVGL pool and hung the firmware when other screens then allocated.
    if (!s_code_mask) {
        s_code_mask = lv_obj_create(ui_Screen1);
        lv_obj_remove_flag(s_code_mask, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(s_code_mask, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_code_mask, 0, 0);
        lv_obj_set_style_pad_all(s_code_mask, 0, 0);
        lv_obj_set_size(s_code_mask, 200, 48);
        lv_obj_set_align(s_code_mask, LV_ALIGN_CENTER);
        lv_obj_set_y(s_code_mask, 0);
        lv_obj_set_flex_flow(s_code_mask, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_code_mask, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_code_mask, 10, 0);
        lv_obj_add_flag(s_code_mask, LV_OBJ_FLAG_HIDDEN);
        for (int d = 0; d < 6; d++) {
            lv_obj_t *dot = lv_obj_create(s_code_mask);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(dot, 12, 12);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x007EF2), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            if (d == 2) lv_obj_set_style_margin_right(dot, 16, 0);  // wider gap between groups
        }
    }
    lv_obj_move_foreground(s_code_mask);   // keep above the tileview
}

// Hide every tile's code label and show the shared dot-mask (or the reverse).
static void apply_code_hidden(void) {
    for (uint8_t i = 0; i < s_built_count; i++) {
        if (!s_tile_code[i]) continue;
        if (s_code_hidden) lv_obj_add_flag(s_tile_code[i],    LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_remove_flag(s_tile_code[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_code_mask) {
        if (s_code_hidden) lv_obj_remove_flag(s_code_mask, LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_add_flag(s_code_mask,    LV_OBJ_FLAG_HIDDEN);
    }
}

static void dots_strip_anim_cb(void *obj, int32_t v) {
    lv_obj_set_y((lv_obj_t *)obj, v);
}

// Restyle every dot and slide the strip so the active dot sits inside the
// 5-dot window. Edge dots are dimmed when more pages exist beyond them.
static void dots_update(bool animate) {
    if (!s_dots_strip || s_dot_count == 0) return;

    int n   = s_dot_count;
    int act = s_current_idx;

    // Window start: keep active dot centred until the strip hits either end.
    int win_start = 0;
    if (n > DOT_WINDOW) {
        win_start = act - DOT_WINDOW / 2;
        if (win_start < 0)              win_start = 0;
        if (win_start > n - DOT_WINDOW) win_start = n - DOT_WINDOW;
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *dot = s_dots[i];
        if (!dot) continue;
        bool active = (i == act);

        // Active dot is an elongated pill; it grows symmetrically into the
        // gaps so neighbouring dots never shift.
        lv_obj_set_size(dot, DOT_SIZE, active ? DOT_ACTIVE_H : DOT_SIZE);
        lv_obj_set_pos(dot, (DOT_VIEW_W - DOT_SIZE) / 2,
                       i * DOT_PITCH - (active ? (DOT_ACTIVE_H - DOT_SIZE) / 2 : 0));
        lv_obj_set_style_bg_color(dot, active ? lv_color_hex(0x2196F3)
                                              : lv_color_hex(0x666666), 0);

        // Dim the window-edge dots when pages continue past them.
        lv_opa_t opa = LV_OPA_COVER;
        if (n > DOT_WINDOW) {
            int pos = i - win_start;
            if (pos < 0 || pos >= DOT_WINDOW)
                opa = LV_OPA_TRANSP;   // outside window (clipped anyway)
            else if (!active &&
                     ((pos == 0 && win_start > 0) ||
                      (pos == DOT_WINDOW - 1 && win_start < n - DOT_WINDOW)))
                opa = LV_OPA_40;
        }
        lv_obj_set_style_bg_opa(dot, opa, 0);
    }

    // Slide the strip so the window shows [win_start .. win_start+4].
    int32_t target_y = -win_start * DOT_PITCH;
    lv_anim_delete(s_dots_strip, dots_strip_anim_cb);
    if (animate) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_dots_strip);
        lv_anim_set_exec_cb(&a, dots_strip_anim_cb);
        lv_anim_set_values(&a, lv_obj_get_y(s_dots_strip), target_y);
        lv_anim_set_duration(&a, 250);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    } else {
        lv_obj_set_y(s_dots_strip, target_y);
    }
}

// (Re)build the dot indicator for `count` pages. Hidden entirely for 0/1 page.
static void dots_build(uint8_t count) {
    if (s_dots_view) {
        lv_obj_delete(s_dots_view);
        s_dots_view  = nullptr;
        s_dots_strip = nullptr;
    }
    s_dot_count = (count <= TOTP_MAX_ACCOUNTS) ? count : TOTP_MAX_ACCOUNTS;
    if (s_dot_count < 2) return;

    int visible = (s_dot_count < DOT_WINDOW) ? s_dot_count : DOT_WINDOW;
    int view_h  = (visible - 1) * DOT_PITCH + DOT_ACTIVE_H;

    // Clipping window — children outside it are not drawn.
    s_dots_view = lv_obj_create(ui_Screen1);
    lv_obj_set_size(s_dots_view, DOT_VIEW_W, view_h);
    lv_obj_align(s_dots_view, LV_ALIGN_LEFT_MID, 20, 5);   // left of the code, clear of the arc
    lv_obj_set_style_bg_opa(s_dots_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dots_view, 0, 0);
    lv_obj_set_style_pad_all(s_dots_view, 0, 0);
    lv_obj_remove_flag(s_dots_view, LV_OBJ_FLAG_SCROLLABLE);

    // Strip holding all dots — slides vertically inside the window.
    s_dots_strip = lv_obj_create(s_dots_view);
    lv_obj_set_size(s_dots_strip, DOT_VIEW_W,
                    (s_dot_count - 1) * DOT_PITCH + DOT_ACTIVE_H);
    lv_obj_set_pos(s_dots_strip, 0, 0);
    lv_obj_set_style_bg_opa(s_dots_strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dots_strip, 0, 0);
    lv_obj_set_style_pad_all(s_dots_strip, 0, 0);
    lv_obj_remove_flag(s_dots_strip, LV_OBJ_FLAG_SCROLLABLE);

    for (uint8_t i = 0; i < s_dot_count; i++) {
        lv_obj_t *dot = lv_obj_create(s_dots_strip);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        s_dots[i] = dot;
    }
    dots_update(false);
}

// Navigate to account `idx`: refresh its labels first (so the correct code is
// visible during the slide), then animate the tileview and the dot strip.
static void totp_goto(uint8_t idx, bool animate) {
    if (!s_tileview || idx >= s_built_count) return;
    s_current_idx = idx;
    refresh_current_account();
    lv_tileview_set_tile_by_index(s_tileview, 0, idx,
                                  animate ? LV_ANIM_ON : LV_ANIM_OFF);
    dots_update(animate);
}

// ── Public API ────────────────────────────────────────────────────────────────

void ui_show_splash(void) {
    lv_obj_t *scr = make_screen_with_bg();

    lv_obj_t *img = lv_image_create(scr);
    lv_image_set_src(img, &FobBob_Icon);
    lv_image_set_scale(img, 256);   // ~78% of 240px ≈ 187px (256 = 100% in LVGL)
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);

    lv_screen_load(scr);
    display_on();
}

void ui_screens_init(void) {
    lv_display_t *dispp = lv_display_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp,
        lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
        true, LV_FONT_DEFAULT);
    lv_display_set_theme(dispp, theme);
}

void ui_set_display_timeout(uint16_t sec) {
    if (sec < TOTP_DISPLAY_SEC_MIN) sec = TOTP_DISPLAY_SEC_MIN;
    if (sec > TOTP_DISPLAY_SEC_MAX) sec = TOTP_DISPLAY_SEC_MAX;
    s_display_timeout_ms = (uint32_t)sec * 1000;
}

void ui_tick(void) {
    // Settings + WiFi-info screens share the idle-sleep timeout, no per-second content.
    if (ui_settings_active() || ui_wifi_info_active()) {
        if ((millis() - s_last_activity_ms) > s_display_timeout_ms) {
            display_off();
            power_go_to_sleep();
        }
        return;
    }

    if (!ui_Screen1 || lv_screen_active() != ui_Screen1) return;
    refresh_current_account();

    if ((millis() - s_last_activity_ms) > s_display_timeout_ms) {
        display_off();
        power_go_to_sleep();
    }
}

void ui_activity(void) {
    s_last_activity_ms = millis();
    power_reset_idle_timer();
    display_on();
}

// ── TOTP screen ───────────────────────────────────────────────────────────────

void ui_show_totp(uint8_t account_count) {
    s_account_count = account_count;
    s_current_idx   = 0;
    s_code_hidden   = false;   // codes are revealed on each fresh unlock

    if (!ui_Screen1) ui_Screen1_screen_init();

    // Free any lingering Lottie canvas from the FP/BLE screens so the TOTP screen
    // starts with NO active ThorVG canvas. Required before the charging modal can
    // safely create its own — ThorVG allows only one SwCanvas at a time.
    if (s_lottie_result)  { lv_obj_delete(s_lottie_result);  s_lottie_result  = nullptr; }
    if (s_lottie_fp_idle) { lv_obj_delete(s_lottie_fp_idle); s_lottie_fp_idle = nullptr; }
    if (s_lottie_ble)     { lv_obj_delete(s_lottie_ble);     s_lottie_ble     = nullptr; }

    // Build the sliding tile per account + the page-dot indicator.
    totp_build_tiles(account_count);
    dots_build(account_count);
    apply_code_hidden();   // reset to revealed (s_code_hidden was cleared above)
    totp_goto(0, false);

    // Populate device name from NVS
    char dev_name[64] = {};
    if (!nvs_get_device_name(dev_name, sizeof(dev_name)) || dev_name[0] == '\0') {
        strncpy(dev_name, "FobBob", sizeof(dev_name));
    }
    lv_label_set_text(ui_Name, dev_name);

    refresh_current_account();

    // Start the red-zone flash timer (once) — blinks the code in its last 5s.
    if (!s_flash_timer) {
        s_flash_timer = lv_timer_create(flash_timer_cb, 600, NULL);
    }

    s_last_activity_ms = millis();
    lv_screen_load(ui_Screen1);
    display_on();
}

void ui_totp_update(uint8_t idx, const char *name, uint32_t code, uint8_t secs_remaining) {
    if (!ui_Screen1 || lv_screen_active() != ui_Screen1) return;
    if (idx != s_current_idx) return;

    lv_label_set_text(ui_Account, name);
    if (code != UINT32_MAX) {
        lv_label_set_text_fmt(ui_Code, "%06lu", (unsigned long)code);
    } else {
        lv_label_set_text(ui_Code, "------");
    }
    update_arc(secs_remaining);
    update_datetime();
}

void ui_totp_next(void) {
    if (s_account_count > 0 && s_current_idx < s_account_count - 1) {
        totp_goto(s_current_idx + 1, true);
    }
}

void ui_totp_prev(void) {
    if (s_current_idx > 0) {
        totp_goto(s_current_idx - 1, true);
    }
}

void ui_totp_toggle_hide(void) {
    s_code_hidden = !s_code_hidden;
    apply_code_hidden();
}

// Brief "WiFi synced" indicator on the TOTP screen — a small green WiFi glyph in
// the top-right corner that auto-hides after a few seconds. Created lazily as a
// direct child of ui_Screen1 (persists across tile rebuilds). Passive: shown only
// after a successful background NTP sync, never a "connecting" state.
static lv_obj_t *s_sync_icon = nullptr;
static void sync_icon_hide_cb(lv_timer_t *t) {
    if (s_sync_icon) lv_obj_add_flag(s_sync_icon, LV_OBJ_FLAG_HIDDEN);
    lv_timer_delete(t);   // one-shot
}
void ui_totp_set_synced(void) {
    if (!ui_Screen1) return;
    if (!s_sync_icon) {
        s_sync_icon = lv_label_create(ui_Screen1);
        lv_obj_set_style_text_font(s_sync_icon, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_sync_icon, lv_color_hex(0x4CAF50), 0);  // green
        lv_label_set_text(s_sync_icon, LV_SYMBOL_WIFI);
        lv_obj_align(s_sync_icon, LV_ALIGN_TOP_RIGHT, -18, 10);
    }
    lv_obj_remove_flag(s_sync_icon, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(sync_icon_hide_cb, 4000, nullptr);
}

// ── Charging modal (plays once over the TOTP screen on plug-in) ────────────────

static lv_obj_t  *s_charge_overlay = nullptr;
static lv_obj_t  *s_charge_lottie  = nullptr;
static uint8_t   *s_charge_buf     = nullptr;   // PSRAM render buffer (lazy, reused)
static lv_timer_t *s_charge_timer  = nullptr;

static void charge_modal_dismiss(lv_timer_t *t) {
    if (s_charge_lottie)  { lv_obj_delete(s_charge_lottie);  s_charge_lottie  = nullptr; }
    if (s_charge_overlay) { lv_obj_delete(s_charge_overlay); s_charge_overlay = nullptr; }
    s_charge_timer = nullptr;
    lv_timer_delete(t);
}

void ui_totp_show_charging(int pct) {
    if (!ui_Screen1 || lv_screen_active() != ui_Screen1) return;  // TOTP screen only
    if (s_charge_overlay) return;                                  // already showing

    // TOTP has no Lottie of its own, but a tick-result canvas can linger after a
    // verify. Free any before creating ours — ThorVG = one SwCanvas at a time.
    if (s_lottie_result)  { lv_obj_delete(s_lottie_result);  s_lottie_result  = nullptr; }
    if (s_lottie_fp_idle) { lv_obj_delete(s_lottie_fp_idle); s_lottie_fp_idle = nullptr; }

    if (!s_charge_buf)
        s_charge_buf = (uint8_t *)heap_caps_malloc(LOTTIE_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_charge_buf) return;

    // Full-screen dark overlay over the TOTP content (Apple-style takeover).
    s_charge_overlay = lv_obj_create(ui_Screen1);
    lv_obj_remove_flag(s_charge_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_charge_overlay, 240, 240);
    lv_obj_center(s_charge_overlay);
    lv_obj_set_style_bg_color(s_charge_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_charge_overlay, LV_OPA_80, 0);   // dim, not full takeover — TOTP shows faintly behind
    lv_obj_set_style_border_width(s_charge_overlay, 0, 0);
    lv_obj_set_style_radius(s_charge_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_charge_overlay, 0, 0);
    lv_obj_move_foreground(s_charge_overlay);

    memset(s_charge_buf, 0, LOTTIE_BUF_BYTES);
    s_charge_lottie = lv_lottie_create(s_charge_overlay);
    lv_lottie_set_src_data(s_charge_lottie, battery_charging_anim_data, battery_charging_anim_size);
    lv_lottie_set_buffer(s_charge_lottie, FP_LOTTIE_SIZE, FP_LOTTIE_SIZE, s_charge_buf);
    lv_obj_align(s_charge_lottie, LV_ALIGN_CENTER, 5, -12);   // nudge right — artwork sits left of its frame centre

    // Play exactly once (lv_lottie defaults to looping) at the true duration.
    lv_lottie_t *ld = (lv_lottie_t *)s_charge_lottie;
    uint32_t anim_ms = 1600;
    if (ld->anim) {
        float dur_s = 0;
        tvg_animation_get_duration(ld->tvg_anim, &dur_s);
        if (dur_s > 0) ld->anim->duration = (int32_t)(dur_s * 1000.0f);
        ld->anim->repeat_cnt = 1;
        anim_ms = (uint32_t)ld->anim->duration;
    }

    // Percentage under the icon.
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    lv_obj_t *lbl = lv_label_create(s_charge_overlay);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x4CAF50), 0);   // green
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 56);
    lv_label_set_text_fmt(lbl, "%d%%", pct);

    // Dismiss after the visible action finishes (the bolt fills in ~1.1 s) plus a
    // brief hold; cap so a long tail never lingers. Then the overlay is removed
    // and the TOTP screen (still loaded underneath) reappears.
    uint32_t show_ms = (anim_ms < 1600 ? anim_ms : 1600) + 400;
    // The callback deletes itself, so a single fire — no repeat-count needed.
    s_charge_timer = lv_timer_create(charge_modal_dismiss, show_ms, NULL);

    s_last_activity_ms = millis();   // don't let the idle timer sleep mid-animation
}

// ── BLE provision screen ──────────────────────────────────────────────────────

void ui_show_ble_provision(void) {
    // ThorVG allows only ONE active SwCanvas — a second one deadlocks the
    // renderer (same root cause as the idle→result and BLE→FP guards). The
    // reprovision fingerprint gate shows the FP screen BEFORE this one, so the
    // FP Lotties may still be alive here: delete them before creating ours.
    if (s_lottie_fp_idle) {
        lv_obj_delete(s_lottie_fp_idle);
        s_lottie_fp_idle = nullptr;
    }
    if (s_lottie_result) {
        lv_obj_delete(s_lottie_result);
        s_lottie_result = nullptr;
    }

    if (!s_screen_ble) {
        s_screen_ble = make_screen_with_bg();

        // Lottie animation — rendered at 120×120 into a PSRAM buffer.
        if (!s_lottie_buf) {
            s_lottie_buf = (uint8_t*)heap_caps_malloc(LOTTIE_BUF_BYTES, MALLOC_CAP_SPIRAM);
        }

        if (s_lottie_buf) {
            memset(s_lottie_buf, 0, LOTTIE_BUF_BYTES);
            s_lottie_ble = lv_lottie_create(s_screen_ble);
            lv_lottie_set_src_data(s_lottie_ble, ble_anim_data, ble_anim_size);
            lv_lottie_set_buffer(s_lottie_ble, BLE_LOTTIE_SIZE, BLE_LOTTIE_SIZE, s_lottie_buf);
            lv_obj_align(s_lottie_ble, LV_ALIGN_CENTER, 0, -10);
        } else {
            // PSRAM alloc failed — fall back to text.
            lv_obj_t *icon = lv_label_create(s_screen_ble);
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
            lv_obj_set_style_text_color(icon, lv_color_hex(0x2196F3), 0);
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, -10);
            lv_label_set_text(icon, "BLE");
        }

        s_lbl_ble_status = lv_label_create(s_screen_ble);
        lv_obj_set_style_text_font(s_lbl_ble_status, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(s_lbl_ble_status, lv_color_white(), 0);
        lv_obj_align(s_lbl_ble_status, LV_ALIGN_CENTER, 0, 75);
        lv_label_set_text(s_lbl_ble_status, "Waiting...");
    }

    lv_screen_load(s_screen_ble);
    display_on();
}

void ui_ble_set_status(const char *msg) {
    if (s_lbl_ble_status) lv_label_set_text(s_lbl_ble_status, msg);
}

// ── Fingerprint screen ────────────────────────────────────────────────────────

// Reset arc to the idle blue-spinning state.
static void fp_arc_reset(void) {
    if (s_arc_fp) lv_obj_set_style_arc_color(s_arc_fp, lv_color_hex(0x2196F3), LV_PART_INDICATOR);
}

// Show/hide the idle indicator (fp_anim Lottie, or arc fallback if it didn't load).
static void fp_show_waiting(bool show) {
    if (s_lottie_fp_idle) {
        if (show) lv_obj_remove_flag(s_lottie_fp_idle, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(s_lottie_fp_idle,    LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_t *items[] = { s_ring_fp, s_arc_fp, s_icon_fp };
        for (lv_obj_t *o : items) {
            if (!o) continue;
            if (show) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
            else      lv_obj_add_flag(o,    LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Delete the idle indicator and create the result Lottie.
// We DELETE (not hide) the idle Lottie so ThorVG fully tears down its canvas.
// Hiding keeps the lv_anim_t ticking; after the first animation loop ThorVG enters
// a "seek-to-frame-0" reset and creating a second canvas mid-reset deadlocks it.
// Returns the recommended transition timer period in ms (animation duration + 200 ms).
static uint32_t fp_show_result(const void *data, size_t size) {
    // Cancel any pending restore/transition timer from a previous result.
    if (s_fp_result_timer) {
        lv_timer_delete(s_fp_result_timer);
        s_fp_result_timer = nullptr;
    }
    if (s_lottie_fp_idle) {
        lv_obj_delete(s_lottie_fp_idle);   // stops animation + frees ThorVG canvas
        s_lottie_fp_idle = nullptr;         // buffer (s_fp_idle_buf) is still alive
    } else {
        lv_obj_t *items[] = { s_ring_fp, s_arc_fp, s_icon_fp };
        for (lv_obj_t *o : items) {
            if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_lottie_result) {
        lv_obj_delete(s_lottie_result);
        s_lottie_result = nullptr;
    }
    if (!s_fp_result_buf)
        s_fp_result_buf = (uint8_t*)heap_caps_malloc(LOTTIE_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fp_result_buf || !s_screen_fp) return 1800;
    memset(s_fp_result_buf, 0, LOTTIE_BUF_BYTES);
    s_lottie_result = lv_lottie_create(s_screen_fp);
    lv_lottie_set_src_data(s_lottie_result, data, size);
    lv_lottie_set_buffer(s_lottie_result, FP_LOTTIE_SIZE, FP_LOTTIE_SIZE, s_fp_result_buf);
    lv_obj_align(s_lottie_result, LV_ALIGN_CENTER, 0, -20);

    // lv_lottie_set_src_data hardcodes 60 fps: duration = frames * 1000/60.
    // Use ThorVG's actual duration to play at the correct speed, exactly once.
    lv_lottie_t *ld = (lv_lottie_t *)s_lottie_result;
    float dur_s = 0;
    tvg_animation_get_duration(ld->tvg_anim, &dur_s);
    uint32_t anim_ms = (dur_s > 0) ? (uint32_t)(dur_s * 1000.0f) : (uint32_t)ld->anim->duration;
    if (ld->anim) {
        ld->anim->duration   = (int32_t)anim_ms;
        ld->anim->repeat_cnt = 1;
    }
    uint32_t delay = anim_ms + 200;
    return (delay < 1500) ? 1500 : delay;
}

// Return to idle waiting state.
static void fp_restore_idle(void) {
    // Kill BLE Lottie first — two ThorVG SwCanvases active simultaneously deadlock
    // the renderer. The enrollment-complete timer recreates s_lottie_ble when it
    // returns to the BLE screen; ensure_fp_screen() skips the deletion on re-entry
    // because s_screen_fp already exists. Guard here so it's always cleaned up.
    if (s_lottie_ble) {
        lv_obj_delete(s_lottie_ble);
        s_lottie_ble = nullptr;
    }
    if (s_lottie_result) {
        lv_obj_delete(s_lottie_result);
        s_lottie_result = nullptr;
    }
    // Recreate idle Lottie from the still-allocated buffer (fp_show_result deleted the widget).
    if (!s_lottie_fp_idle && s_fp_idle_buf && s_screen_fp) {
        memset(s_fp_idle_buf, 0, LOTTIE_BUF_BYTES);
        s_lottie_fp_idle = lv_lottie_create(s_screen_fp);
        lv_lottie_set_src_data(s_lottie_fp_idle, fp_anim_data, fp_anim_size);
        lv_lottie_set_buffer(s_lottie_fp_idle, FP_LOTTIE_SIZE, FP_LOTTIE_SIZE, s_fp_idle_buf);
        lv_obj_align(s_lottie_fp_idle, LV_ALIGN_CENTER, 0, -20);
        lv_anim_t *a = lv_lottie_get_anim(s_lottie_fp_idle);
        if (!a || a->end_value == 0) {
            lv_obj_delete(s_lottie_fp_idle);
            s_lottie_fp_idle = nullptr;
        }
    }
    fp_arc_reset();
    fp_show_waiting(true);
}

// Arc spinner callback
static void fp_arc_anim_cb(void *obj, int32_t angle) {
    lv_arc_set_angles((lv_obj_t *)obj, (uint16_t)angle, (uint16_t)((angle + 80) % 360));
}

// Pulse animation on the FP icon label (opacity 100%↔40%, 800 ms)
static void fp_pulse_anim_cb(void *obj, int32_t v) {
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void ensure_fp_screen(void) {
    if (s_screen_fp) return;
    s_screen_fp = make_screen_with_bg();

    // ── Idle indicator: spinning arc + "FP" label ─────────────────────────────
    s_ring_fp = lv_arc_create(s_screen_fp);
    lv_obj_set_size(s_ring_fp, 120, 120);
    lv_obj_align(s_ring_fp, LV_ALIGN_CENTER, 0, -20);
    lv_obj_remove_flag(s_ring_fp, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(s_ring_fp, 0, 360);
    lv_arc_set_range(s_ring_fp, 0, 360);
    lv_arc_set_value(s_ring_fp, 0);
    lv_obj_set_style_arc_width(s_ring_fp, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring_fp, lv_color_hex(0x1A3A5C), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring_fp, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ring_fp, LV_OPA_TRANSP, LV_PART_KNOB);

    s_arc_fp = lv_arc_create(s_screen_fp);
    lv_obj_set_size(s_arc_fp, 120, 120);
    lv_obj_align(s_arc_fp, LV_ALIGN_CENTER, 0, -20);
    lv_obj_remove_flag(s_arc_fp, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(s_arc_fp, 0, 0);
    lv_arc_set_angles(s_arc_fp, 0, 80);
    lv_obj_set_style_arc_width(s_arc_fp, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_fp, lv_color_hex(0x2196F3), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_fp, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc_fp, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_arc_opa(s_arc_fp, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_arc_fp);
    lv_anim_set_exec_cb(&a, fp_arc_anim_cb);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    s_icon_fp = lv_label_create(s_screen_fp);
    lv_obj_set_style_text_font(s_icon_fp, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_icon_fp, lv_color_hex(0x2196F3), 0);
    lv_obj_align(s_icon_fp, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(s_icon_fp, "FP");

    lv_anim_t p;
    lv_anim_init(&p);
    lv_anim_set_var(&p, s_icon_fp);
    lv_anim_set_exec_cb(&p, fp_pulse_anim_cb);
    lv_anim_set_values(&p, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_duration(&p, 800);
    lv_anim_set_repeat_count(&p, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_reverse_duration(&p, 800);
    lv_anim_start(&p);

    // ── Idle Lottie — load immediately, hide fallback widgets if it succeeds ────
    // Delete BLE Lottie first: its ThorVG SwCanvas must be destroyed before we
    // create the FP canvas. Two simultaneous SwCanvas instances share ThorVG's
    // internal renderer lock and deadlock on ESP32 (same root cause as idle→result hang).
    if (s_lottie_ble) {
        lv_obj_delete(s_lottie_ble);
        s_lottie_ble = nullptr;
    }
    // set_src_data BEFORE set_buffer (per official LVGL example).
    // Result Lotties (tick/cross) are created lazily in fp_show_result().
    s_fp_idle_buf = (uint8_t*)heap_caps_malloc(LOTTIE_BUF_BYTES, MALLOC_CAP_SPIRAM);

    if (s_fp_idle_buf) {
        memset(s_fp_idle_buf, 0, LOTTIE_BUF_BYTES);
        s_lottie_fp_idle = lv_lottie_create(s_screen_fp);
        lv_lottie_set_src_data(s_lottie_fp_idle, fp_anim_data, fp_anim_size);
        lv_lottie_set_buffer(s_lottie_fp_idle, FP_LOTTIE_SIZE, FP_LOTTIE_SIZE, s_fp_idle_buf);
        lv_obj_align(s_lottie_fp_idle, LV_ALIGN_CENTER, 0, -20);
        lv_anim_t *a = lv_lottie_get_anim(s_lottie_fp_idle);
        if (a && a->end_value > 0) {
            lv_obj_add_flag(s_ring_fp, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_arc_fp,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_icon_fp, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_delete(s_lottie_fp_idle);
            s_lottie_fp_idle = nullptr;
        }
    }

    // ── Status text ───────────────────────────────────────────────────────────
    s_lbl_fp_status = lv_label_create(s_screen_fp);
    lv_obj_set_style_text_font(s_lbl_fp_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_lbl_fp_status, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_lbl_fp_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_lbl_fp_status, 200);
    lv_obj_align(s_lbl_fp_status, LV_ALIGN_CENTER, 0, 72);
}

// ── Public FP screen API ─────────────────────────────────────────────────────

void ui_show_fp_verify(void) {
    ensure_fp_screen();
    fp_restore_idle();
    lv_label_set_text(s_lbl_fp_status, "Place finger\nto unlock");
    lv_screen_load(s_screen_fp);
    display_on();
}

void ui_show_fp_enroll(void) {
    ensure_fp_screen();
    fp_restore_idle();
    lv_label_set_text(s_lbl_fp_status, "Place finger");
    lv_screen_load(s_screen_fp);
    display_on();
}

void ui_fp_enroll_update(uint8_t captures_done, bool success) {
    if (!s_lbl_fp_status) return;

    if (!success) {
        uint32_t delay = fp_show_result(cross_anim_data, cross_anim_size);
        if (s_arc_fp) lv_obj_set_style_arc_color(s_arc_fp, lv_color_hex(0xF44336), LV_PART_INDICATOR);
        lv_label_set_text(s_lbl_fp_status, "Failed - try again");
        s_fp_result_timer = lv_timer_create([](lv_timer_t *t) {
            s_fp_result_timer = nullptr;
            fp_restore_idle();
            if (s_lbl_fp_status) lv_label_set_text(s_lbl_fp_status, "Place finger");
            lv_timer_delete(t);
        }, delay, NULL);
        lv_timer_set_repeat_count(s_fp_result_timer, 1);
        return;
    }

    uint32_t delay = fp_show_result(tick_anim_data, tick_anim_size);
    if (s_arc_fp) lv_obj_set_style_arc_color(s_arc_fp, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);

    if (captures_done >= 2) {
        lv_label_set_text(s_lbl_fp_status, "Enrolled!");
        s_fp_result_timer = lv_timer_create([](lv_timer_t *t) {
            s_fp_result_timer = nullptr;
            // Delete result Lottie before recreating BLE Lottie — ThorVG can only
            // have one active SwCanvas at a time without deadlocking on ESP32.
            if (s_lottie_result) {
                lv_obj_delete(s_lottie_result);
                s_lottie_result = nullptr;
            }
            if (s_screen_ble && !s_lottie_ble && s_lottie_buf) {
                memset(s_lottie_buf, 0, LOTTIE_BUF_BYTES);
                s_lottie_ble = lv_lottie_create(s_screen_ble);
                lv_lottie_set_src_data(s_lottie_ble, ble_anim_data, ble_anim_size);
                lv_lottie_set_buffer(s_lottie_ble, BLE_LOTTIE_SIZE, BLE_LOTTIE_SIZE, s_lottie_buf);
                lv_obj_align(s_lottie_ble, LV_ALIGN_CENTER, 0, -10);
            }
            if (s_lbl_ble_status) {
                lv_obj_set_style_text_font(s_lbl_ble_status, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_align(s_lbl_ble_status, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_width(s_lbl_ble_status, 200);
                lv_label_set_text(s_lbl_ble_status, "Enroll another finger\nor continue");
            }
            if (s_screen_ble) lv_screen_load(s_screen_ble);
            lv_timer_delete(t);
        }, delay, NULL);
        lv_timer_set_repeat_count(s_fp_result_timer, 1);
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "Scan %d/2", captures_done);
        lv_label_set_text(s_lbl_fp_status, buf);
        s_fp_result_timer = lv_timer_create([](lv_timer_t *t) {
            s_fp_result_timer = nullptr;
            fp_restore_idle();
            if (s_lbl_fp_status) lv_label_set_text(s_lbl_fp_status, "Place finger again");
            lv_timer_delete(t);
        }, delay, NULL);
        lv_timer_set_repeat_count(s_fp_result_timer, 1);
    }
}

void ui_fp_verify_result(bool matched) {
    if (!s_lbl_fp_status) return;

    if (matched) {
        uint32_t delay = fp_show_result(tick_anim_data, tick_anim_size);
        if (s_arc_fp) lv_obj_set_style_arc_color(s_arc_fp, lv_color_hex(0x4CAF50), LV_PART_INDICATOR);
        lv_label_set_text(s_lbl_fp_status, "Verified");
        s_fp_result_timer = lv_timer_create([](lv_timer_t *t) {
            s_fp_result_timer = nullptr;
            ui_show_totp(nvs_get_totp_count());
            lv_timer_delete(t);
        }, delay, NULL);
        lv_timer_set_repeat_count(s_fp_result_timer, 1);
    } else {
        uint32_t delay = fp_show_result(cross_anim_data, cross_anim_size);
        if (s_arc_fp) lv_obj_set_style_arc_color(s_arc_fp, lv_color_hex(0xF44336), LV_PART_INDICATOR);
        lv_label_set_text(s_lbl_fp_status, "Not recognized");
        s_fp_result_timer = lv_timer_create([](lv_timer_t *t) {
            s_fp_result_timer = nullptr;
            fp_restore_idle();
            if (s_lbl_fp_status) lv_label_set_text(s_lbl_fp_status, "Try again");
            lv_timer_delete(t);
        }, delay, NULL);
        lv_timer_set_repeat_count(s_fp_result_timer, 1);
    }
}

bool ui_fp_result_active(void) {
    return s_lottie_result != nullptr;
}

void ui_fp_set_status(const char *msg) {
    if (s_lbl_fp_status) lv_label_set_text(s_lbl_fp_status, msg);
}

void ui_fp_anim_trigger(void) {
    if (!s_lottie_fp_idle) return;
    lv_lottie_t *ld = (lv_lottie_t *)s_lottie_fp_idle;
    if (!ld->anim) return;
    ld->anim->act_time = 0;  // restart from frame 0; repeat_cnt stays INFINITE (widget default)
}

uint32_t ui_fp_anim_duration_ms(void) {
    if (!s_lottie_fp_idle) return 1200;
    lv_lottie_t *ld = (lv_lottie_t *)s_lottie_fp_idle;
    if (!ld->anim || ld->anim->duration == 0) return 1200;
    return (uint32_t)ld->anim->duration;
}

// ── Settings screen (touch) — center-locked picker ─────────────────────────────
// A fixed focus slot sits at the viewport centre; the options scroll under it like
// a smartwatch wheel. Whichever option lands in the focus slot is "selected".
// Scrolling cycles options through the slot (one haptic detent per option, fired
// by main.cpp). Tap an off-centre option to scroll it into the slot; tap the
// option already in the slot to toggle/cycle/activate it. No LVGL input device is
// registered — main.cpp feeds CST816 gestures in via the ui_settings_* API, so
// LVGL never auto-scrolls and we own selection + scroll position.
//
// Centring maths: rows are SET_ROW_H tall on a SET_PITCH grid, and the list has
// SET_PAD_Y top/bottom padding sized so the first and last rows can each reach the
// centre slot. With that padding, the scroll-top offset that centres row i is
// exactly i * SET_PITCH — so centring is scroll_to_y(i * SET_PITCH) and the focused
// row is round(scroll_top / SET_PITCH).

// SET_BATTERY is a non-interactive status row (always first); activating it is a
// no-op. It can still scroll through the focus slot like any other option.
// SET_WIFI is a live ON/OFF toggle; SET_SYNC is an action row whose value shows
// the last-sync age ("3h ago"/"Never"/"Syncing...") and whose tap forces a sync.
// All value-bearing rows must stay contiguous from 0..SET_TIMEOUT (see the
// `i <= SET_TIMEOUT` guard in build_settings_screen).
enum { SET_BATTERY = 0, SET_WIFI, SET_SYNC, SET_TAP, SET_ADAPT, SET_FLIP,
       SET_HAPTIC, SET_TIMEOUT, SET_REPROV, SET_EXIT, SET_COUNT };

static const uint16_t SET_TIMEOUT_PRESETS[] = { 5, 15, 30, 60, 120, 300 };
static constexpr int   SET_TIMEOUT_PRESET_COUNT = 6;

static lv_obj_t *s_screen_set        = nullptr;
static lv_obj_t *s_set_list          = nullptr;   // scrollable row container (viewport)
static lv_obj_t *s_set_focus         = nullptr;   // fixed highlight frame at the centre slot
static lv_obj_t *s_set_rows[SET_COUNT]   = {};
static lv_obj_t *s_set_labels[SET_COUNT] = {};
static lv_obj_t *s_set_values[SET_COUNT] = {};   // null for action rows
static lv_obj_t *s_reprov_fill       = nullptr;   // hold-to-trigger progress fill (Reprovision row)

// Base geometry (SET_ROW_W/H, SET_ROW_GAP, SET_VIEW_W/H) is tunable in config.h.
static constexpr int SET_PITCH   = SET_ROW_H + SET_ROW_GAP;       // centre-to-centre spacing
static constexpr int SET_PAD_Y   = (SET_VIEW_H - SET_ROW_H) / 2;  // lets first/last row centre
static constexpr int SET_SCROLL_MAX = SET_PITCH * (SET_COUNT - 1);

static int       s_set_sel           = SET_WIFI;  // focused (centre) row
static bool      s_set_tap           = false;
static bool      s_set_adapt         = false;
static bool      s_set_flip          = false;
static bool      s_set_haptic        = true;
static bool      s_set_wifi          = false;
static uint16_t  s_set_disp          = TOTP_DISPLAY_SEC_DEFAULT;

static void set_refresh_row(int i) {
    if (i < 0 || i >= SET_COUNT || !s_set_values[i]) return;
    switch (i) {
        case SET_WIFI:   lv_label_set_text(s_set_values[i], s_set_wifi   ? "ON" : "OFF"); break;
        case SET_TAP:    lv_label_set_text(s_set_values[i], s_set_tap    ? "ON" : "OFF"); break;
        case SET_ADAPT:  lv_label_set_text(s_set_values[i], s_set_adapt  ? "ON" : "OFF"); break;
        case SET_FLIP:   lv_label_set_text(s_set_values[i], s_set_flip   ? "ON" : "OFF"); break;
        case SET_HAPTIC: lv_label_set_text(s_set_values[i], s_set_haptic ? "ON" : "OFF"); break;
        // SET_SYNC's value is driven live by ui_settings_set_sync_status(), not here.
        case SET_TIMEOUT: {
            char b[12];
            if (s_set_disp < 60) snprintf(b, sizeof(b), "%us", s_set_disp);
            else                 snprintf(b, sizeof(b), "%um", s_set_disp / 60);
            lv_label_set_text(s_set_values[i], b);
            break;
        }
        default: break;
    }
}

// Re-colour rows for the current focus. The blue focus frame is fixed at the
// centre slot, so the focused row reads as black-on-blue; off-centre options dim.
// The battery value keeps its semantic colour (owned by ui_settings_set_battery),
// so only its label is re-coloured here.
static void set_update_highlight(void) {
    for (int i = 0; i < SET_COUNT; i++) {
        if (!s_set_rows[i]) continue;
        bool sel = (i == s_set_sel);
        lv_color_t lbl = sel ? lv_color_black() : lv_color_hex(0xBDBDBD);
        lv_obj_set_style_text_color(s_set_labels[i], lbl, 0);
        // Battery + Sync own their value colour (status/health), so don't dim them.
        if (s_set_values[i] && i != SET_BATTERY && i != SET_SYNC)
            lv_obj_set_style_text_color(s_set_values[i],
                sel ? lv_color_black() : lv_color_hex(0x757575), 0);
    }
}

static void build_settings_screen(void) {
    if (s_screen_set) return;
    s_screen_set = make_screen_with_bg();

    lv_obj_t *title = lv_label_create(s_screen_set);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);
    lv_label_set_text(title, "Settings");

    // Fixed focus frame at the centre slot — options scroll under it. Created
    // before the list so it sits behind the (transparent) list and its rows.
    lv_obj_t *focus = lv_obj_create(s_screen_set);
    s_set_focus = focus;
    lv_obj_remove_flag(focus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(focus, SET_ROW_W, SET_ROW_H);
    lv_obj_align(focus, LV_ALIGN_CENTER, 0, 18);   // matches the list centre below
    lv_obj_set_style_radius(focus, 8, 0);
    lv_obj_set_style_border_width(focus, 0, 0);
    lv_obj_set_style_bg_color(focus, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_bg_opa(focus, LV_OPA_COVER, 0);

    lv_obj_t *list = lv_obj_create(s_screen_set);
    s_set_list = list;
    lv_obj_set_size(list, SET_VIEW_W, SET_VIEW_H);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_top(list, SET_PAD_Y, 0);    // lets the first row reach the centre
    lv_obj_set_style_pad_bottom(list, SET_PAD_Y, 0); // …and the last row
    lv_obj_set_style_pad_row(list, SET_ROW_GAP, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    static const char *names[SET_COUNT] = {
        "Battery", "WiFi", "Sync", "Double-tap sleep", "Adaptive timeout",
        "Auto-rotate", "Haptics", "Screen timeout", "Reprovision", "Exit",
    };
    for (int i = 0; i < SET_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, SET_ROW_W, SET_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);   // focus frame provides the highlight
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_style_clip_corner(row, true, 0);   // keep the hold-fill inside the rounded corners
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_set_rows[i] = row;

        // Hold-to-trigger fill — created first so it sits behind the label.
        // Spans from the row's left edge (content origin is +12 due to padding).
        if (i == SET_REPROV) {
            lv_obj_t *fill = lv_obj_create(row);
            lv_obj_remove_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_border_width(fill, 0, 0);
            lv_obj_set_style_radius(fill, 0, 0);
            lv_obj_set_style_bg_color(fill, lv_color_hex(0xFF8800), 0);  // amber = commit
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
            lv_obj_set_pos(fill, -12, 0);
            lv_obj_set_size(fill, 0, SET_ROW_H);
            lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
            s_reprov_fill = fill;
        }

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_text(lbl, names[i]);
        s_set_labels[i] = lbl;

        if (i <= SET_TIMEOUT) {
            lv_obj_t *val = lv_label_create(row);
            lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
            lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_label_set_text(val, "");   // clear LVGL's default "Text" until refreshed
            s_set_values[i] = val;
        }
    }
}

// Scroll the wheel so row `sel` lands in the centre focus slot (see centring maths
// above). Updates the focused selection + highlight. Clamps sel to a real row.
static void set_center_on(int sel, bool anim) {
    if (!s_set_list) return;
    if (sel < 0) sel = 0;
    if (sel >= SET_COUNT) sel = SET_COUNT - 1;
    s_set_sel = sel;
    lv_obj_update_layout(s_set_list);
    lv_obj_scroll_to_y(s_set_list, SET_PITCH * sel, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    set_update_highlight();
}

void ui_show_settings(bool tap_sleep, bool adaptive, bool orient_flip,
                      bool haptics, uint16_t disp_sec, bool wifi_enabled) {
    build_settings_screen();
    s_set_tap    = tap_sleep;
    s_set_adapt  = adaptive;
    s_set_flip   = orient_flip;
    s_set_haptic = haptics;
    s_set_wifi   = wifi_enabled;
    s_set_disp   = disp_sec;
    for (int i = 0; i < SET_COUNT; i++) set_refresh_row(i);
    ui_settings_set_hold_progress(0);   // clear any leftover hold fill
    s_last_activity_ms = millis();
    lv_screen_load(s_screen_set);
    set_center_on(SET_WIFI, false);     // open focused on WiFi (surfaces the new feature)
    display_on();
}

int ui_settings_nav(int dir) {
    if (!s_screen_set) return s_set_sel;
    int sel = s_set_sel + (dir > 0 ? 1 : dir < 0 ? -1 : 0);
    set_center_on(sel, true);
    s_last_activity_ms = millis();
    return s_set_sel;
}

UiSettingsAction ui_settings_select(void) {
    if (!s_screen_set) return UI_SETTINGS_NONE;
    s_last_activity_ms = millis();
    switch (s_set_sel) {
        case SET_WIFI:   s_set_wifi   = !s_set_wifi;   set_refresh_row(SET_WIFI);   break;
        case SET_SYNC:   return UI_SETTINGS_SYNC_NOW;   // force a background sync (caller handles)
        case SET_TAP:    s_set_tap    = !s_set_tap;    set_refresh_row(SET_TAP);    break;
        case SET_ADAPT:  s_set_adapt  = !s_set_adapt;  set_refresh_row(SET_ADAPT);  break;
        case SET_FLIP:   s_set_flip   = !s_set_flip;   set_refresh_row(SET_FLIP);   break;
        case SET_HAPTIC: s_set_haptic = !s_set_haptic; set_refresh_row(SET_HAPTIC); break;
        case SET_TIMEOUT: {
            int idx = 0;
            for (int k = 0; k < SET_TIMEOUT_PRESET_COUNT; k++)
                if (SET_TIMEOUT_PRESETS[k] == s_set_disp) { idx = k; break; }
            idx = (idx + 1) % SET_TIMEOUT_PRESET_COUNT;
            s_set_disp = SET_TIMEOUT_PRESETS[idx];
            set_refresh_row(SET_TIMEOUT);
            break;
        }
        case SET_BATTERY: return UI_SETTINGS_IGNORED;  // status only — no action, no feedback
        case SET_REPROV:  return UI_SETTINGS_IGNORED;  // hold-to-trigger only — a tap does nothing
        case SET_EXIT:    return UI_SETTINGS_EXIT;
    }
    return UI_SETTINGS_NONE;
}

// Closest-anchor: the row whose vertical center is nearest screen-y, clamped to
// the visible viewport. Returns -1 if the point or all row centers are off-screen.
static int settings_row_at(int y) {
    if (!s_screen_set || !s_set_list) return -1;

    // Ensure positions reflect any in-flight scroll before measuring.
    lv_obj_update_layout(s_screen_set);

    lv_area_t va;
    lv_obj_get_coords(s_set_list, &va);
    if (y < va.y1 || y > va.y2) return -1;

    int best = -1;
    int best_dist = 1 << 30;
    for (int i = 0; i < SET_COUNT; i++) {
        if (!s_set_rows[i]) continue;
        lv_area_t a;
        lv_obj_get_coords(s_set_rows[i], &a);
        int cy = (a.y1 + a.y2) / 2;
        if (cy < va.y1 || cy > va.y2) continue;   // row center off-screen
        int d = (cy > y) ? (cy - y) : (y - cy);
        if (d < best_dist) { best_dist = d; best = i; }
    }
    return best;
}

int ui_settings_row_at(int x, int y) {
    (void)x;  // rows are full-width — only the Y coordinate disambiguates
    return settings_row_at(y);
}

bool ui_settings_row_is_hold(int row) {
    return row == SET_REPROV;
}

void ui_settings_set_hold_progress(int pct) {
    if (!s_reprov_fill) return;
    if (pct <= 0) {
        lv_obj_add_flag(s_reprov_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_reprov_fill, 0);
        return;
    }
    if (pct > 100) pct = 100;
    lv_obj_remove_flag(s_reprov_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_reprov_fill, SET_ROW_W * pct / 100);
}

UiSettingsAction ui_settings_tap(int x, int y) {
    (void)x;
    int best = settings_row_at(y);
    if (best < 0) return UI_SETTINGS_IGNORED;       // outside the list — ignore, no buzz
    if (best == SET_BATTERY) {                       // status row: bring into view, but silent
        set_center_on(SET_BATTERY, true);
        return UI_SETTINGS_IGNORED;
    }
    if (best == s_set_sel) return ui_settings_select();  // already centred → activate
    set_center_on(best, true);                      // off-centre → scroll it into focus
    return UI_SETTINGS_NONE;
}

void ui_settings_set_battery(int pct, bool charging) {
    if (!s_set_values[SET_BATTERY]) return;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    char buf[16];
    lv_color_t col;
    if (charging) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE " %d%%", pct);
        col = lv_color_hex(0x4CAF50);                 // green
    } else {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        if      (pct < 5)  col = lv_color_hex(0xF44336);   // red
        else if (pct < 20) col = lv_color_hex(0xFF8800);   // amber
        else               col = lv_color_hex(0xE0E0E0);   // normal
    }
    lv_label_set_text(s_set_values[SET_BATTERY], buf);
    lv_obj_set_style_text_color(s_set_values[SET_BATTERY], col, 0);
}

// The focused row implied by the current scroll position: scroll_top / SET_PITCH,
// rounded to the nearest detent and clamped to a real row.
static int settings_centered_from_scroll(void) {
    lv_obj_update_layout(s_set_list);
    int st = lv_obj_get_scroll_top(s_set_list);
    if (st < 0) st = 0;
    int sel = (st + SET_PITCH / 2) / SET_PITCH;
    if (sel < 0) sel = 0;
    if (sel >= SET_COUNT) sel = SET_COUNT - 1;
    return sel;
}

int ui_settings_centered_row(void) {
    if (!s_set_list) return s_set_sel;
    return settings_centered_from_scroll();
}

int ui_settings_scroll_by(int dy) {
    if (!s_set_list) return s_set_sel;
    // lv_obj_scroll_by(+dy) moves content DOWN, i.e. reduces scroll_top. Clamp the
    // resulting scroll_top to [0, SET_SCROLL_MAX] so the first/last option can travel
    // no further than the centre slot, then convert the delta back for scroll_by.
    lv_obj_update_layout(s_set_list);
    int st = lv_obj_get_scroll_top(s_set_list);
    int want = st - dy;
    if (want < 0) want = 0;
    if (want > SET_SCROLL_MAX) want = SET_SCROLL_MAX;
    int applied = st - want;
    if (applied) lv_obj_scroll_by(s_set_list, 0, applied, LV_ANIM_OFF);
    int sel = settings_centered_from_scroll();
    if (sel != s_set_sel) { s_set_sel = sel; set_update_highlight(); }
    s_last_activity_ms = millis();
    return s_set_sel;
}

void ui_settings_snap(void) {
    if (!s_set_list) return;
    set_center_on(settings_centered_from_scroll(), true);
    s_last_activity_ms = millis();
}

void ui_settings_get(bool* tap_sleep, bool* adaptive, bool* orient_flip,
                     bool* haptics, uint16_t* disp_sec, bool* wifi_enabled) {
    if (tap_sleep)    *tap_sleep    = s_set_tap;
    if (adaptive)     *adaptive     = s_set_adapt;
    if (orient_flip)  *orient_flip  = s_set_flip;
    if (haptics)      *haptics      = s_set_haptic;
    if (disp_sec)     *disp_sec     = s_set_disp;
    if (wifi_enabled) *wifi_enabled = s_set_wifi;
}

// Live-update the "Sync now" row's value — doubles as the WiFi status indicator.
// `state`: 0 neutral, 1 ok (green + WiFi glyph), 2 fail (red + WiFi glyph),
// 3 busy (pulsing WiFi glyph). Driven each second by main.cpp while settings open.
static bool s_sync_pulsing = false;

void ui_settings_set_sync_status(const char* text, int state) {
    if (!s_set_values[SET_SYNC] || !text) return;

    // Prefix a WiFi glyph for every WiFi-meaningful state (ok / fail / busy) so the
    // icon itself conveys status; only the plain "Never" neutral state omits it.
    char buf[48];
    if (state == 0)
        snprintf(buf, sizeof(buf), "%s", text);
    else
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", text);
    lv_label_set_text(s_set_values[SET_SYNC], buf);

    bool sel = (SET_SYNC == s_set_sel);
    lv_color_t col;
    switch (state) {
        case 1:  col = lv_color_hex(0x4CAF50); break;             // ok — green
        case 2:  col = lv_color_hex(0xF44336); break;             // fail — red
        case 3:  col = sel ? lv_color_black()                     // busy — black on the
                           : lv_color_hex(0x2196F3); break;       //   blue focus, else blue
        default: col = sel ? lv_color_black() : lv_color_hex(0x757575); break;
    }
    lv_obj_set_style_text_color(s_set_values[SET_SYNC], col, 0);

    // Pulse the glyph while connecting/syncing; solid otherwise. Reuses the
    // opacity-pulse cb from the FP screen. The SET_SYNC label persists across
    // settings opens (lazy build), so the animation target is always valid.
    if (state == 3) {
        if (!s_sync_pulsing) {
            lv_anim_t p;
            lv_anim_init(&p);
            lv_anim_set_var(&p, s_set_values[SET_SYNC]);
            lv_anim_set_exec_cb(&p, fp_pulse_anim_cb);
            lv_anim_set_values(&p, LV_OPA_40, LV_OPA_COVER);
            lv_anim_set_duration(&p, 600);
            lv_anim_set_reverse_duration(&p, 600);
            lv_anim_set_repeat_count(&p, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&p);
            s_sync_pulsing = true;
        }
    } else if (s_sync_pulsing) {
        lv_anim_delete(s_set_values[SET_SYNC], fp_pulse_anim_cb);
        lv_obj_set_style_text_opa(s_set_values[SET_SYNC], LV_OPA_COVER, 0);
        s_sync_pulsing = false;
    }
}

// True when the WiFi row is the focused (centre) row — used so a long-press on it
// opens the credentials page rather than toggling.
bool ui_settings_focused_is_wifi(void) { return s_set_sel == SET_WIFI; }

// ── WiFi credentials page (opened by holding the WiFi settings row) ──────────
static lv_obj_t *s_screen_wifi   = nullptr;
static lv_obj_t *s_wifi_ssid_lbl = nullptr;
static lv_obj_t *s_wifi_pass_lbl = nullptr;

static void build_wifi_info_screen(void) {
    if (s_screen_wifi) return;
    s_screen_wifi = make_screen_with_bg();

    lv_obj_t *title = lv_label_create(s_screen_wifi);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x2196F3), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WiFi");

    lv_obj_t *scap = lv_label_create(s_screen_wifi);
    lv_obj_set_style_text_font(scap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(scap, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(scap, LV_ALIGN_TOP_MID, 0, 78);
    lv_label_set_text(scap, "Network");

    s_wifi_ssid_lbl = lv_label_create(s_screen_wifi);
    lv_obj_set_style_text_font(s_wifi_ssid_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_wifi_ssid_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(s_wifi_ssid_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi_ssid_lbl, 180);
    lv_obj_set_style_text_align(s_wifi_ssid_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_ssid_lbl, LV_ALIGN_TOP_MID, 0, 98);

    lv_obj_t *pcap = lv_label_create(s_screen_wifi);
    lv_obj_set_style_text_font(pcap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pcap, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(pcap, LV_ALIGN_TOP_MID, 0, 138);
    lv_label_set_text(pcap, "Password");

    s_wifi_pass_lbl = lv_label_create(s_screen_wifi);
    lv_obj_set_style_text_font(s_wifi_pass_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_wifi_pass_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(s_wifi_pass_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_wifi_pass_lbl, 180);
    lv_obj_set_style_text_align(s_wifi_pass_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi_pass_lbl, LV_ALIGN_TOP_MID, 0, 158);

    lv_obj_t *hint = lv_label_create(s_screen_wifi);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x616161), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_label_set_text(hint, "Release to return");
}

void ui_show_wifi_info(const char* ssid, const char* pass) {
    build_wifi_info_screen();
    lv_label_set_text(s_wifi_ssid_lbl, (ssid && ssid[0]) ? ssid : "(not set)");
    lv_label_set_text(s_wifi_pass_lbl, (pass && pass[0]) ? pass : "(none)");
    s_last_activity_ms = millis();
    lv_screen_load(s_screen_wifi);
    display_on();
}

bool ui_wifi_info_active(void) {
    return s_screen_wifi && lv_screen_active() == s_screen_wifi;
}

bool ui_settings_active(void) {
    return s_screen_set && lv_screen_active() == s_screen_set;
}

bool ui_totp_active(void) {
    return ui_Screen1 && lv_screen_active() == ui_Screen1;
}
