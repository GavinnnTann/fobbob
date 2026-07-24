// Based on SquareLine Studio export — adapted for FobBob TOTP display.
// LVGL v9.5 API; arc corrected to 270° sweep with gap at bottom.

#include "../ui.h"

lv_obj_t * ui_Screen1    = NULL;
lv_obj_t * ui_Panel1     = NULL;
lv_obj_t * ui_Scroll_bar = NULL;
lv_obj_t * ui_Account    = NULL;
lv_obj_t * ui_Code       = NULL;
lv_obj_t * ui_Time       = NULL;
lv_obj_t * ui_FobBob     = NULL;
lv_obj_t * ui_Name       = NULL;

void ui_Screen1_screen_init(void)
{
    ui_Screen1 = lv_obj_create(NULL);
    lv_obj_remove_flag(ui_Screen1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Screen1, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Screen1, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    // ── Panel (full-screen black canvas, arc lives inside) ──────────────────
    ui_Panel1 = lv_obj_create(ui_Screen1);
    lv_obj_set_width(ui_Panel1, 240);
    lv_obj_set_height(ui_Panel1, 240);
    lv_obj_set_align(ui_Panel1, LV_ALIGN_CENTER);
    lv_obj_remove_flag(ui_Panel1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Panel1, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Panel1, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Panel1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // ── Countdown arc — created first so it renders behind labels ───────────
    // 270° sweep: start 135° (≈8 o'clock), end 45° (≈2 o'clock), gap at bottom.
    ui_Scroll_bar = lv_arc_create(ui_Panel1);
    lv_obj_set_width(ui_Scroll_bar, 240);
    lv_obj_set_height(ui_Scroll_bar, 240);
    lv_obj_set_align(ui_Scroll_bar, LV_ALIGN_CENTER);
    lv_obj_remove_flag(ui_Scroll_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(ui_Scroll_bar, 135, 45);
    lv_arc_set_range(ui_Scroll_bar, 0, 30);
    lv_arc_set_value(ui_Scroll_bar, 30);   // full on load

    // Background ring — dim grey
    lv_obj_set_style_arc_width(ui_Scroll_bar, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(ui_Scroll_bar, lv_color_hex(0x222222), LV_PART_MAIN | LV_STATE_DEFAULT);

    // Indicator ring — blue (updated dynamically as time progresses)
    lv_obj_set_style_arc_width(ui_Scroll_bar, 8, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_color(ui_Scroll_bar, lv_color_hex(0x2196F3), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(ui_Scroll_bar, true, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // Hide the drag knob
    lv_obj_set_style_bg_opa(ui_Scroll_bar, LV_OPA_TRANSP, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Scroll_bar, 0, LV_PART_KNOB | LV_STATE_DEFAULT);

    // ── Account name + TOTP code labels ──────────────────────────────────────
    // Created per-tile by the tileview in ui_screens.cpp (totp_build_tiles).
    // ui_Account / ui_Code are repointed to the active tile's labels at runtime.

    // ── Datetime label ───────────────────────────────────────────────────────
    ui_Time = lv_label_create(ui_Panel1);
    lv_obj_set_width(ui_Time, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Time, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Time, 0);
    lv_obj_set_y(ui_Time, 60);
    lv_obj_set_align(ui_Time, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Time, "YYYY-MM-DD HH:mm:ss");
    lv_obj_set_style_text_font(ui_Time, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Time, lv_color_hex(0x888888), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_Time, LV_OBJ_FLAG_HIDDEN);

    // ── Static product label ─────────────────────────────────────────────────
    ui_FobBob = lv_label_create(ui_Panel1);
    lv_obj_set_width(ui_FobBob, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_FobBob, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_FobBob, 0);
    lv_obj_set_y(ui_FobBob, 90);
    lv_obj_set_align(ui_FobBob, LV_ALIGN_CENTER);
    lv_label_set_text(ui_FobBob, "FobBob");
    lv_obj_set_style_text_color(ui_FobBob, lv_color_hex(0x555555), LV_PART_MAIN | LV_STATE_DEFAULT);

    // ── Device name label — sibling of Panel1 so it renders on top ───────────
    ui_Name = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_Name, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Name, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Name, 0);
    lv_obj_set_y(ui_Name, -90);
    lv_obj_set_align(ui_Name, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Name, "FobBob");
    lv_obj_set_style_text_color(ui_Name, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
}

void ui_Screen1_screen_destroy(void)
{
    if(ui_Screen1) lv_obj_delete(ui_Screen1);

    ui_Screen1    = NULL;
    ui_Panel1     = NULL;
    ui_Scroll_bar = NULL;
    ui_Account    = NULL;
    ui_Code       = NULL;
    ui_Time       = NULL;
    ui_FobBob     = NULL;
    ui_Name       = NULL;
}
