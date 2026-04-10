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

/* ─── Active menu state (for Up/Down navigation) ────────────── */

typedef struct {
    lv_obj_t   *btns[MENU_MAX_BTNS];
    lv_style_t *btn_style;
    lv_style_t *sel_style;
    int         index;
    int         size;
} active_menu_t;

static active_menu_t active_menu = {0};

void screen_menu_reset(void)
{
    active_menu.size = 0;
}

/**
 * Mirrors the firmware's lcd_*_update_menu() pattern:
 *   1. Wrap index
 *   2. Reset all buttons to unselected style
 *   3. Highlight selected button
 *   4. lv_obj_scroll_to_view with LV_ANIM_ON
 */
void screen_menu_navigate(int direction)
{
    if (active_menu.size <= 0) return;

    active_menu.index += direction;

    /* Wrap */
    if (active_menu.index >= active_menu.size)
        active_menu.index = 0;
    else if (active_menu.index < 0)
        active_menu.index = active_menu.size - 1;

    /* Reset all to unselected */
    for (int i = 0; i < active_menu.size; i++) {
        lv_obj_remove_style(active_menu.btns[i], active_menu.sel_style, 0);
        lv_obj_add_style(active_menu.btns[i], active_menu.btn_style, 0);
    }

    /* Highlight selected */
    lv_obj_remove_style(active_menu.btns[active_menu.index], active_menu.btn_style, 0);
    lv_obj_add_style(active_menu.btns[active_menu.index], active_menu.sel_style, 0);

    /* Scroll to view with animation */
    lv_obj_scroll_to_view(active_menu.btns[active_menu.index], LV_ANIM_ON);
}

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

/**
 * Mirror of lcd_apply_scrollbar_style() from lcd_utils.c — adds a narrow
 * right-side scrollbar (AUTO mode), reserves padding for it, and nudges the
 * list 6px right to re-center after the padding offset.
 */
static void apply_scrollbar_style(lv_obj_t *obj)
{
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_AUTO);

    static lv_style_t main_style;
    static bool main_inited = false;
    if (!main_inited) {
        lv_style_init(&main_style);
        lv_style_set_pad_right(&main_style, 24); /* 4px bar + 8px gap + extra */
        main_inited = true;
    }
    lv_obj_add_style(obj, &main_style, LV_PART_MAIN);
    lv_obj_set_x(obj, lv_obj_get_x(obj) + 6);

    static lv_style_t sb_style;
    static bool sb_inited = false;
    if (!sb_inited) {
        lv_style_init(&sb_style);
        lv_style_set_width(&sb_style, 4);
        lv_style_set_bg_opa(&sb_style, LV_OPA_60);
        lv_style_set_bg_color(&sb_style, USER_SECONDARY_COLOR);
        lv_style_set_radius(&sb_style, 3);
        lv_style_set_pad_left(&sb_style, 12);
        lv_style_set_pad_right(&sb_style, 0);
        sb_inited = true;
    }
    lv_obj_add_style(obj, &sb_style, LV_PART_SCROLLBAR);
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

/* ─── LoRa page ────────────────────────────────────────────────
 * Mirrors lcd_lora_setup_page() in components/lcd/src/lcd_lora.c.
 * Options[0] is always "Add PolyPlug"; options[1..] are user outlets loaded
 * from NVS. Default index is 1 when size > 1 (firmware lines 135-137). */

void screen_lora(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Menu list (from lcd_lora_setup_page) ── */
    lv_obj_t *main_list = lv_list_create(scr);
    lv_obj_set_size(main_list, 210, 106);

    lv_obj_set_style_bg_color(main_list, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    apply_scrollbar_style(main_list);
    lv_obj_set_scroll_dir(main_list, LV_DIR_VER);

    /* ── Button style (unselected) ── */
    static lv_style_t lora_btn_style;
    lv_style_init(&lora_btn_style);
    lv_style_set_radius(&lora_btn_style, 8);
    lv_style_set_bg_color(&lora_btn_style, primary);
    lv_style_set_border_width(&lora_btn_style, 2);
    lv_style_set_border_color(&lora_btn_style, secondary);
    lv_style_set_border_side(&lora_btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&lora_btn_style, 3);
    lv_style_set_pad_bottom(&lora_btn_style, 3);
    lv_style_set_text_font(&lora_btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&lora_btn_style, secondary);
    lv_style_set_text_align(&lora_btn_style, LV_TEXT_ALIGN_CENTER);

    /* ── Selected style ── */
    static lv_style_t lora_sel_style;
    lv_style_init(&lora_sel_style);
    lv_style_set_radius(&lora_sel_style, 8);
    lv_style_set_bg_color(&lora_sel_style, secondary);
    lv_style_set_border_width(&lora_sel_style, 2);
    lv_style_set_border_color(&lora_sel_style, secondary);
    lv_style_set_border_side(&lora_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&lora_sel_style, 3);
    lv_style_set_pad_bottom(&lora_sel_style, 3);
    lv_style_set_text_font(&lora_sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&lora_sel_style, primary);
    lv_style_set_text_align(&lora_sel_style, LV_TEXT_ALIGN_CENTER);

    /* ── Options ── */
    static const char *lora_options[] = {
        "Add PolyPlug", "Living Room", "Kitchen", "Bedroom Lamp", "Garage"
    };
    int num_options = sizeof(lora_options) / sizeof(lora_options[0]);
    int selected = 1; /* firmware default when size > 1 */

    for (int i = 0; i < num_options; i++) {
        lv_obj_t *btn = lv_list_add_btn(main_list, NULL, lora_options[i]);
        lv_obj_set_size(btn, 200, 30);

        if (i == selected) {
            lv_obj_add_style(btn, &lora_sel_style, 0);
        } else {
            lv_obj_add_style(btn, &lora_btn_style, 0);
        }

        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        active_menu.btns[i] = btn;
    }

    /* Format buttons as flex container with spacing */
    lv_obj_t *cont = lv_obj_get_parent(active_menu.btns[0]);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Register for Up/Down navigation */
    active_menu.size      = num_options;
    active_menu.index     = selected;
    active_menu.btn_style = &lora_btn_style;
    active_menu.sel_style = &lora_sel_style;

    /* ── Persistent UI (arrows + battery) ──
     * LoRa page inherits arrow state from selection: top/bot/left visible, right hidden. */
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

/* ─── Bluetooth page ──────────────────────────────────────────── */

void screen_bluetooth(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Menu list (from lcd_bluetooth_setup_page) ── */
    lv_obj_t *main_list = lv_list_create(scr);
    lv_obj_set_size(main_list, 210, 106);

    lv_obj_set_style_bg_color(main_list, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(main_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(main_list, LV_DIR_VER);

    /* ── Button style (unselected) ── */
    static lv_style_t bt_btn_style;
    lv_style_init(&bt_btn_style);
    lv_style_set_radius(&bt_btn_style, 8);
    lv_style_set_bg_color(&bt_btn_style, primary);
    lv_style_set_border_width(&bt_btn_style, 2);
    lv_style_set_border_color(&bt_btn_style, secondary);
    lv_style_set_border_side(&bt_btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&bt_btn_style, 3);
    lv_style_set_pad_bottom(&bt_btn_style, 3);
    lv_style_set_text_font(&bt_btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&bt_btn_style, secondary);
    lv_style_set_text_align(&bt_btn_style, LV_TEXT_ALIGN_CENTER);

    /* ── Selected style ── */
    static lv_style_t bt_sel_style;
    lv_style_init(&bt_sel_style);
    lv_style_set_radius(&bt_sel_style, 8);
    lv_style_set_bg_color(&bt_sel_style, secondary);
    lv_style_set_border_width(&bt_sel_style, 2);
    lv_style_set_border_color(&bt_sel_style, secondary);
    lv_style_set_border_side(&bt_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&bt_sel_style, 3);
    lv_style_set_pad_bottom(&bt_sel_style, 3);
    lv_style_set_text_font(&bt_sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&bt_sel_style, primary);
    lv_style_set_text_align(&bt_sel_style, LV_TEXT_ALIGN_CENTER);

    /* ── Menu options ── */
    static const char *bt_options[] = {
        "Pair Device", "Auto Keyboard", "AI Keyboard"
    };
    int num_options = 3;
    int selected = 1; /* Auto Keyboard — firmware default */

    for (int i = 0; i < num_options; i++) {
        lv_obj_t *btn = lv_list_add_btn(main_list, NULL, bt_options[i]);
        lv_obj_set_size(btn, 200, 30);

        if (i == selected) {
            lv_obj_add_style(btn, &bt_sel_style, 0);
        } else {
            lv_obj_add_style(btn, &bt_btn_style, 0);
        }

        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        active_menu.btns[i] = btn;
    }

    /* Format buttons as flex container with spacing */
    lv_obj_t *cont = lv_obj_get_parent(active_menu.btns[0]);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Register for Up/Down navigation */
    active_menu.size      = num_options;
    active_menu.index     = selected;
    active_menu.btn_style = &bt_btn_style;
    active_menu.sel_style = &bt_sel_style;

    /* ── Persistent UI (arrows + battery) ──
     * On the device, arrow_right is hidden on BLUETOOTH_PAGE — only up/down/left
     * carry over from the selection page. */
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
}

/* ─── Tools page ─────────────────────────────────────────────────
 * Mirrors lcd_tools_setup_page() in components/lcd/src/lcd_tools.c.
 * Option list matches the static tools_menu initializer (8 entries).
 * Default index is 0 ("Coin Flipper"). */

void screen_tools(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Menu list (from lcd_tools_setup_page) ── */
    lv_obj_t *main_list = lv_list_create(scr);
    lv_obj_set_size(main_list, 210, 106);

    lv_obj_set_style_bg_color(main_list, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    apply_scrollbar_style(main_list);
    lv_obj_set_scroll_dir(main_list, LV_DIR_VER);

    /* ── Button style (unselected) ── */
    static lv_style_t tools_btn_style;
    lv_style_init(&tools_btn_style);
    lv_style_set_radius(&tools_btn_style, 8);
    lv_style_set_bg_color(&tools_btn_style, primary);
    lv_style_set_border_width(&tools_btn_style, 2);
    lv_style_set_border_color(&tools_btn_style, secondary);
    lv_style_set_border_side(&tools_btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&tools_btn_style, 3);
    lv_style_set_pad_bottom(&tools_btn_style, 3);
    lv_style_set_text_font(&tools_btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&tools_btn_style, secondary);
    lv_style_set_text_align(&tools_btn_style, LV_TEXT_ALIGN_CENTER);

    /* ── Selected style ── */
    static lv_style_t tools_sel_style;
    lv_style_init(&tools_sel_style);
    lv_style_set_radius(&tools_sel_style, 8);
    lv_style_set_bg_color(&tools_sel_style, secondary);
    lv_style_set_border_width(&tools_sel_style, 2);
    lv_style_set_border_color(&tools_sel_style, secondary);
    lv_style_set_border_side(&tools_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&tools_sel_style, 3);
    lv_style_set_pad_bottom(&tools_sel_style, 3);
    lv_style_set_text_font(&tools_sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&tools_sel_style, primary);
    lv_style_set_text_align(&tools_sel_style, LV_TEXT_ALIGN_CENTER);

    /* ── Options — matches the static tools_menu initializer in lcd_tools.c ── */
    static const char *tools_options[] = {
        "Coin Flipper", "Dice Roller", "Tetris", "Number Generator",
        "Read the Docs", "Bitcoin QR", "Pomodoro Timer", "SRS Planner"
    };
    int num_options = sizeof(tools_options) / sizeof(tools_options[0]);
    int selected = 0; /* firmware default */

    for (int i = 0; i < num_options; i++) {
        lv_obj_t *btn = lv_list_add_btn(main_list, NULL, tools_options[i]);
        lv_obj_set_size(btn, 200, 30);

        if (i == selected) {
            lv_obj_add_style(btn, &tools_sel_style, 0);
        } else {
            lv_obj_add_style(btn, &tools_btn_style, 0);
        }

        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        active_menu.btns[i] = btn;
    }

    /* Format buttons as flex container with spacing */
    lv_obj_t *cont = lv_obj_get_parent(active_menu.btns[0]);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Register for Up/Down navigation */
    active_menu.size      = num_options;
    active_menu.index     = selected;
    active_menu.btn_style = &tools_btn_style;
    active_menu.sel_style = &tools_sel_style;

    /* ── Persistent UI (arrows + battery) ──
     * Tools page inherits arrow state from selection: top/bot/left visible, right hidden. */
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
}

/* ─── Settings page ──────────────────────────────────────────────
 * Mirrors lcd_settings_setup_page() in components/lcd/src/lcd_settings.c.
 * Option list matches the static settings_menu initializer (11 entries).
 * Default index is 0 ("Check for Updates"). */

void screen_settings(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Menu list (from lcd_settings_setup_page) ── */
    lv_obj_t *main_list = lv_list_create(scr);
    lv_obj_set_size(main_list, 210, 106);

    lv_obj_set_style_bg_color(main_list, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    apply_scrollbar_style(main_list);
    lv_obj_set_scroll_dir(main_list, LV_DIR_VER);

    /* ── Button style (unselected) ── */
    static lv_style_t sett_btn_style;
    lv_style_init(&sett_btn_style);
    lv_style_set_radius(&sett_btn_style, 8);
    lv_style_set_bg_color(&sett_btn_style, primary);
    lv_style_set_border_width(&sett_btn_style, 2);
    lv_style_set_border_color(&sett_btn_style, secondary);
    lv_style_set_border_side(&sett_btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&sett_btn_style, 3);
    lv_style_set_pad_bottom(&sett_btn_style, 3);
    lv_style_set_text_font(&sett_btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&sett_btn_style, secondary);
    lv_style_set_text_align(&sett_btn_style, LV_TEXT_ALIGN_CENTER);

    /* ── Selected style ── */
    static lv_style_t sett_sel_style;
    lv_style_init(&sett_sel_style);
    lv_style_set_radius(&sett_sel_style, 8);
    lv_style_set_bg_color(&sett_sel_style, secondary);
    lv_style_set_border_width(&sett_sel_style, 2);
    lv_style_set_border_color(&sett_sel_style, secondary);
    lv_style_set_border_side(&sett_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&sett_sel_style, 3);
    lv_style_set_pad_bottom(&sett_sel_style, 3);
    lv_style_set_text_font(&sett_sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&sett_sel_style, primary);
    lv_style_set_text_align(&sett_sel_style, LV_TEXT_ALIGN_CENTER);

    /* ── Options — matches the static settings_menu initializer in lcd_settings.c ── */
    static const char *sett_options[] = {
        "Check for Updates", "Set Unlock PIN", "Change Colors", "LCD Brightness",
        "Adjust Haptics", "Adjust Sleep Timer", "Adjust RGB LED", "Tips and Tricks",
        "System Info", "Reboot", "Factory Reset"
    };
    int num_options = sizeof(sett_options) / sizeof(sett_options[0]);
    int selected = 0; /* firmware default */

    for (int i = 0; i < num_options; i++) {
        lv_obj_t *btn = lv_list_add_btn(main_list, NULL, sett_options[i]);
        lv_obj_set_size(btn, 200, 30);

        if (i == selected) {
            lv_obj_add_style(btn, &sett_sel_style, 0);
        } else {
            lv_obj_add_style(btn, &sett_btn_style, 0);
        }

        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

        active_menu.btns[i] = btn;
    }

    /* Format buttons as flex container with spacing */
    lv_obj_t *cont = lv_obj_get_parent(active_menu.btns[0]);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Register for Up/Down navigation */
    active_menu.size      = num_options;
    active_menu.index     = selected;
    active_menu.btn_style = &sett_btn_style;
    active_menu.sel_style = &sett_sel_style;

    /* ── Persistent UI (arrows + battery) ──
     * Settings page inherits arrow state from selection: top/bot/left visible, right hidden. */
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
}
