/**
 * PolyCast5 Simulator - Screen rendering
 *
 * Pure LVGL code extracted from the firmware's lcd_utils.c / lcd_task.c.
 * No ESP-IDF dependencies.
 *
 * To add a new screen:
 *   1. Find the screen's setup/draw code in components/lcd/src/lcd_*.c
 *   2. Copy the LVGL calls (lv_obj_create, lv_label_set_text, lv_style_*, etc.)
 *   3. Replace any ESP-IDF calls (NVS, semaphores) with hardcoded values
 *   4. Add a function here and declare it in screens.h
 */

#include "screens.h"
#include "lvgl.h"

#define DEFAULT_BATTERY_LV "92%"

/* ─── Helpers (from lcd_utils.c) ──────────────────────────────── */

static void format_label(lv_obj_t *label, const char *text, lv_color_t color,
                         const lv_font_t *font, lv_align_t alignment,
                         lv_coord_t x_offset, lv_coord_t y_offset)
{
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, alignment, x_offset, y_offset);
}

static void format_center_button(lv_obj_t *btn_mid, lv_color_t primary, lv_color_t secondary)
{
    lv_obj_set_size(btn_mid, 175, 45);
    lv_obj_align(btn_mid, LV_ALIGN_CENTER, 0, 0);

    lv_color_t darker_primary = lv_color_darken(primary, 40);
    lv_color_t darker_secondary = lv_color_darken(secondary, 20);

    static lv_style_t btn_style;
    lv_style_init(&btn_style);
    lv_style_set_radius(&btn_style, 8);
    lv_style_set_bg_color(&btn_style, darker_primary);
    lv_style_set_bg_grad_color(&btn_style, primary);
    lv_style_set_bg_grad_dir(&btn_style, LV_GRAD_DIR_VER);
    lv_style_set_border_width(&btn_style, 2);
    lv_style_set_border_color(&btn_style, darker_secondary);
    lv_style_set_shadow_spread(&btn_style, 3);
    lv_style_set_shadow_width(&btn_style, 6);
    lv_style_set_shadow_offset_x(&btn_style, 3);
    lv_style_set_shadow_offset_y(&btn_style, 3);
    lv_style_set_shadow_color(&btn_style, lv_color_hex(0x000000));
    lv_obj_add_style(btn_mid, &btn_style, 0);
}

/* ─── Selection / Home screen ─────────────────────────────────── */

#define SCROLLBAR_OFFSET      -36
#define SCROLLBAR_CONT_HEIGHT 106
#define SCROLLBAR_THUMB_HEIGHT 20

void screen_selection(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();

    /* Background */
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Menu options (same order as firmware) */
    static const char *options[] = {
        "GPIO", "Wi-Fi", "Bluetooth", "LoRa",
        "ESP-NOW", "Infrared", "Tools", "Settings"
    };
    int size  = 8;
    int index = 3; /* Starts on LoRa */

    int mid = index;
    int top = (mid - 1 + size) % size;
    int bot = (mid + 1) % size;

    /* Scrollbar track */
    lv_obj_t *scroll_track = lv_obj_create(scr);
    lv_obj_set_size(scroll_track, 4, SCROLLBAR_CONT_HEIGHT);
    lv_obj_align(scroll_track, LV_ALIGN_RIGHT_MID, -12, 0);

    static lv_style_t track_style;
    lv_style_init(&track_style);
    lv_style_set_bg_opa(&track_style, LV_OPA_100);
    lv_style_set_bg_color(&track_style, lv_color_darken(primary, 100));
    lv_style_set_radius(&track_style, 3);
    lv_style_set_border_width(&track_style, 0);
    lv_obj_add_style(scroll_track, &track_style, LV_PART_MAIN);

    /* Scrollbar thumb */
    lv_obj_t *scroll_bar = lv_obj_create(scr);
    lv_obj_set_size(scroll_bar, 4, SCROLLBAR_THUMB_HEIGHT);
    lv_obj_align(scroll_bar, LV_ALIGN_RIGHT_MID, -12, 0);

    static lv_style_t bar_style;
    lv_style_init(&bar_style);
    lv_style_set_bg_opa(&bar_style, LV_OPA_80);
    lv_style_set_bg_color(&bar_style, secondary);
    lv_style_set_radius(&bar_style, 3);
    lv_style_set_border_width(&bar_style, 0);
    lv_obj_add_style(scroll_bar, &bar_style, LV_PART_MAIN);

    int max_y = SCROLLBAR_CONT_HEIGHT - SCROLLBAR_THUMB_HEIGHT;
    double fraction = (double)index / (size - 1);
    int y = (int)(fraction * max_y + 0.5);
    lv_obj_set_y(scroll_bar, y + SCROLLBAR_OFFSET);
    lv_obj_set_y(scroll_track, y + SCROLLBAR_OFFSET + 7);

    /* Center button */
    lv_obj_t *btn_mid = lv_btn_create(scr);
    format_center_button(btn_mid, primary, secondary);

    /* Top label */
    lv_obj_t *lbl_top = lv_label_create(scr);
    format_label(lbl_top, options[top], secondary,
            &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

    /* Middle label (inside button) */
    lv_obj_t *lbl_mid = lv_label_create(btn_mid);
    format_label(lbl_mid, options[mid], secondary,
            &lv_font_montserrat_30, LV_ALIGN_CENTER, 0, 0);

    /* Bottom label */
    lv_obj_t *lbl_bot = lv_label_create(scr);
    format_label(lbl_bot, options[bot], secondary,
            &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -15);

    /* Arrows */
    lv_obj_t *arrow_top = lv_label_create(scr);
    format_label(arrow_top, LV_SYMBOL_UP, secondary,
            &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *arrow_left = lv_label_create(scr);
    format_label(arrow_left, LV_SYMBOL_LEFT, secondary,
            &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *arrow_bot = lv_label_create(scr);
    format_label(arrow_bot, LV_SYMBOL_DOWN, secondary,
            &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* Battery */
    lv_obj_t *lbl_bat_txt = lv_label_create(scr);
    format_label(lbl_bat_txt, DEFAULT_BATTERY_LV, secondary,
            &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -28, 0);

    lv_obj_t *lbl_bat_icon = lv_label_create(scr);
    format_label(lbl_bat_icon, LV_SYMBOL_BATTERY_FULL, secondary,
            &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT, -2, -3);

    // /* Connectivity icons (hidden by default on device, shown here for preview) */
    // lv_obj_t *lbl_bt = lv_label_create(scr);
    // format_label(lbl_bt, LV_SYMBOL_BLUETOOTH, secondary,
    //              &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 2, 1);

    // /* Wi-Fi icon — uncomment to show:
    // lv_obj_t *lbl_wifi = lv_label_create(scr);
    // format_label(lbl_wifi, LV_SYMBOL_WIFI, secondary,
    //              &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, 3, 0);
    // */
}

/* ─── LoRa subpage (placeholder) ──────────────────────────────── */

void screen_lora(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    format_label(title, "LoRa", secondary,
            &lv_font_montserrat_24, LV_ALIGN_TOP_MID, 0, 8);

    /* Placeholder list items — replace with your actual LoRa menu setup code */
    lv_obj_t *item1 = lv_label_create(scr);
    format_label(item1, "Living Room", secondary,
            &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *item2 = lv_label_create(scr);
    format_label(item2, "Bedroom", secondary,
            &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 15);

    /* Back arrow */
    lv_obj_t *back = lv_label_create(scr);
    format_label(back, LV_SYMBOL_LEFT " Back", secondary,
            &lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 5, -5);
}

/* ─── Infrared page ───────────────────────────────────────────── */

void screen_infrared(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Rotated horizontal list (from lcd_ir_setup_page) ── */
    lv_obj_t *main_list = lv_list_create(scr);
    lv_obj_set_size(main_list, 105, 208);

    lv_obj_set_scrollbar_mode(main_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(main_list, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(main_list, LV_DIR_VER);

    /* Rotate 270 deg to make horizontal */
    lv_obj_set_style_transform_pivot_x(main_list, 120, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_pivot_y(main_list, 67, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_angle(main_list, 2700, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Position adjustments */
    lv_obj_set_x(main_list, -105);
    lv_obj_set_y(main_list, -31);

    /* Row spacing */
    lv_obj_set_style_pad_row(main_list, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Button style (unselected) ── */
    static lv_style_t btn_style;
    lv_style_init(&btn_style);
    lv_style_set_radius(&btn_style, 8);
    lv_style_set_bg_color(&btn_style, primary);
    lv_style_set_border_width(&btn_style, 2);
    lv_style_set_border_color(&btn_style, secondary);
    lv_style_set_border_side(&btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&btn_style, 3);
    lv_style_set_pad_bottom(&btn_style, 3);
    lv_style_set_text_font(&btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&btn_style, secondary);
    lv_style_set_text_align(&btn_style, LV_TEXT_ALIGN_CENTER);

    /* ── Selected style ── */
    static lv_style_t sel_style;
    lv_style_init(&sel_style);
    lv_style_set_radius(&sel_style, 8);
    lv_style_set_bg_color(&sel_style, secondary);
    lv_style_set_border_width(&sel_style, 2);
    lv_style_set_border_color(&sel_style, secondary);
    lv_style_set_border_side(&sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&sel_style, 3);
    lv_style_set_pad_bottom(&sel_style, 3);
    lv_style_set_text_font(&sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&sel_style, primary);
    lv_style_set_text_align(&sel_style, LV_TEXT_ALIGN_CENTER);

    /* ── Remote name style (with outline) ── */
    static lv_style_t name_sel_style;
    lv_style_init(&name_sel_style);
    lv_style_set_radius(&name_sel_style, 8);
    lv_style_set_bg_color(&name_sel_style, secondary);
    lv_style_set_outline_width(&name_sel_style, 2);
    lv_style_set_outline_color(&name_sel_style, secondary);
    lv_style_set_outline_pad(&name_sel_style, 1);
    lv_style_set_border_width(&name_sel_style, 2);
    lv_style_set_border_color(&name_sel_style, secondary);
    lv_style_set_border_side(&name_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&name_sel_style, 3);
    lv_style_set_pad_bottom(&name_sel_style, 3);
    lv_style_set_text_font(&name_sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&name_sel_style, primary);
    lv_style_set_text_align(&name_sel_style, LV_TEXT_ALIGN_CENTER);

    /* ── Build menu items (from lcd_ir_build_current_menu) ── */

    /* Fake remote data for preview */
    static const char *signal_names[] = { "Power", "Vol Up", "Vol Down", "Mute" };
    int num_signals = 4;

    /* Remote name button (index 0) — selected by default */
    lv_obj_t *btn0 = lv_list_add_btn(main_list, NULL, "Samsung TV");
    lv_obj_set_size(btn0, 100, 28);
    lv_obj_add_style(btn0, &name_sel_style, 0);
    lv_obj_t *lbl = lv_obj_get_child(btn0, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);

    /* Edit button (index 1) */
    lv_obj_t *btn1 = lv_list_add_btn(main_list, NULL, "Edit");
    lv_obj_set_size(btn1, 100, 28);
    lv_obj_add_style(btn1, &btn_style, 0);
    lbl = lv_obj_get_child(btn1, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);

    /* Add New button (index 2) */
    lv_obj_t *btn2 = lv_list_add_btn(main_list, NULL, "Add New");
    lv_obj_set_size(btn2, 100, 28);
    lv_obj_add_style(btn2, &btn_style, 0);
    lbl = lv_obj_get_child(btn2, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);

    /* Signal buttons */
    for (int i = 0; i < num_signals; i++) {
        lv_obj_t *btn = lv_list_add_btn(main_list, NULL, signal_names[i]);
        lv_obj_set_size(btn, 100, 28);
        lv_obj_add_style(btn, &btn_style, 0);
        lbl = lv_obj_get_child(btn, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    }

    /* Format as flex container */
    lv_obj_t *cont = lv_obj_get_parent(btn0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Scroll to top */
    lv_obj_scroll_to_view(btn0, LV_ANIM_OFF);

    /* ── Persistent UI elements (from lcd_init_selection_labels, stay visible across pages) ── */

    /* Arrows — up/down/left stay from selection, right is shown on IR entry */
    lv_obj_t *arrow_top = lv_label_create(scr);
    format_label(arrow_top, LV_SYMBOL_UP, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *arrow_left = lv_label_create(scr);
    format_label(arrow_left, LV_SYMBOL_LEFT, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *arrow_right = lv_label_create(scr);
    format_label(arrow_right, LV_SYMBOL_RIGHT, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_RIGHT_MID, -4, 0);

    lv_obj_t *arrow_bot = lv_label_create(scr);
    format_label(arrow_bot, LV_SYMBOL_DOWN, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* Battery */
    lv_obj_t *lbl_bat_txt = lv_label_create(scr);
    format_label(lbl_bat_txt, DEFAULT_BATTERY_LV, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -28, 0);

    lv_obj_t *lbl_bat_icon = lv_label_create(scr);
    format_label(lbl_bat_icon, LV_SYMBOL_BATTERY_FULL, secondary,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT, -2, -3);
}

/* ─── Infrared Add Signal page ────────────────────────────────── */

LV_IMAGE_DECLARE(img_save_new_remote);

void screen_infrared_add_signal(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Instruction text */
    lv_obj_t *lbl_ins = lv_label_create(scr);
    format_label(lbl_ins, "Point your device at the\nIR lens and send the signal.", secondary,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 13);

    /* IR lens image */
    lv_obj_t *img = lv_img_create(scr);
    lv_image_set_src(img, &img_save_new_remote);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 25);

    /* Signal pulse length label (empty until signal received on device) */
    lv_obj_t *lbl_sig_len = lv_label_create(scr);
    format_label(lbl_sig_len, "", secondary,
                 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -16);

    /* Left arrow (cancel/back) — only arrow visible on this page */
    lv_obj_t *arrow_left = lv_label_create(scr);
    format_label(arrow_left, LV_SYMBOL_LEFT, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);

    /* Battery */
    lv_obj_t *lbl_bat_txt = lv_label_create(scr);
    format_label(lbl_bat_txt, DEFAULT_BATTERY_LV, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -28, 0);

    lv_obj_t *lbl_bat_icon = lv_label_create(scr);
    format_label(lbl_bat_icon, LV_SYMBOL_BATTERY_FULL, secondary,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT, -2, -3);
}

/* ─── Settings page (placeholder) ─────────────────────────────── */

void screen_settings(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(scr);
    format_label(title, "Settings", secondary,
            &lv_font_montserrat_24, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *item = lv_label_create(scr);
    format_label(item, "Replace with actual settings menu", secondary,
            &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
}
