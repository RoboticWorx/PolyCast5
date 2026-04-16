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

#include <stdio.h>
#include <stdlib.h>

#include "screens.h"
#include "lvgl.h"
#include "img_ai_orb_1.h"
#include "img_ai_orb_2.h"
#include "img_ai_orb_3.h"

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

/* Fallback scroll target for pages without an active menu (Wi-Fi beacon /
 * data pages). When set, Up/Down scroll this container instead. */
static lv_obj_t *active_scroll      = NULL;
static int       active_scroll_step = 55;

/* Per-page Up/Down action overrides + cleanup hook. */
static void (*active_up_cb)(void)      = NULL;
static void (*active_down_cb)(void)    = NULL;
static void (*active_cleanup_cb)(void) = NULL;

void screen_menu_reset(void)
{
    /* Run the previous screen's cleanup (e.g. delete timers whose targets
     * are about to be destroyed by lv_obj_clean) before resetting state. */
    if (active_cleanup_cb) {
        void (*cb)(void) = active_cleanup_cb;
        active_cleanup_cb = NULL;
        cb();
    }

    active_menu.size   = 0;
    active_scroll      = NULL;
    active_scroll_step = 55;
    active_up_cb       = NULL;
    active_down_cb     = NULL;
}

void screen_set_scroll(lv_obj_t *cont, int step_px)
{
    active_scroll      = cont;
    active_scroll_step = (step_px > 0) ? step_px : 55;
}

void screen_set_nav_handlers(void (*on_up)(void), void (*on_down)(void))
{
    active_up_cb   = on_up;
    active_down_cb = on_down;
}

void screen_set_cleanup(void (*on_cleanup)(void))
{
    active_cleanup_cb = on_cleanup;
}

/**
 * Mirrors the firmware's lcd_*_update_menu() pattern:
 *   1. Wrap index
 *   2. Reset all buttons to unselected style
 *   3. Highlight selected button
 *   4. lv_obj_scroll_to_view with LV_ANIM_ON
 *
 * If no menu is active but a scroll container is registered (chart pages),
 * falls back to vertically scrolling that container.
 */
void screen_menu_navigate(int direction)
{
    /* Custom per-page action first — this is how the AI keyboard toggles
     * the "Hold & talk" recording animation on Down. */
    if (direction > 0 && active_down_cb) { active_down_cb(); return; }
    if (direction < 0 && active_up_cb)   { active_up_cb();   return; }

    if (active_menu.size <= 0) {
        /* Fallback: scroll the registered container (if any).
         * Direction +1 (DOWN key) pushes content up to reveal content below
         * (same sign convention as the firmware's button handlers). */
        if (active_scroll) {
            int dy = (direction > 0) ? -active_scroll_step : active_scroll_step;
            lv_obj_scroll_by_bounded(active_scroll, 0, dy, true);
        }
        return;
    }

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
        "Add PolyPlug", "Bedroom Fan", "Garage Lights", "Living Room", "Bedroom Lamp", "Garage"
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
    apply_scrollbar_style(main_list);
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

    /* ── Menu options — matches lcd_bluetooth_setup_page in lcd_bluetooth.c ── */
    static const char *bt_options[] = {
        "Pair Device", "Auto Keyboard", "AI Keyboard", "Media Controller",
        "Page Scroller", "PowerPoint Clicker", "Camera Clicker",
        "Socials Scroller", "Forget All Devices", "Known Devices"
    };
    int num_options = sizeof(bt_options) / sizeof(bt_options[0]);
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

/* ─── Wi-Fi Beacon scan page ─────────────────────────────────────
 * Mirrors lcd_wifi_beacon_page() in components/lcd/src/lcd_wifi.c.
 * Bar chart of 20 signal samples (0-50 range, red→green gradient by value),
 * RSSI/SNR labels above, SSID / channel / security info scrolls below. */

/* Mirror of beacon_chart_draw_cb() in lcd_wifi.c — colors each bar by value
 * (0 = red, 50 = green) via LV_EVENT_DRAW_TASK_ADDED. */
static void beacon_chart_draw_cb(lv_event_t *e)
{
    lv_draw_task_t     *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);

    if (base->part != LV_PART_ITEMS) {
        return;
    }

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    if (!fill) {
        return;
    }

    lv_obj_t          *chart = lv_event_get_target_obj(e);
    lv_chart_series_t *ser   = lv_event_get_user_data(e);
    int32_t           *y_arr = lv_chart_get_y_array(chart, ser);

    uint32_t pc = lv_chart_get_point_count(chart);
    if (pc == 0) {
        return;
    }

    uint32_t idx = base->id2;
    if (idx >= pc) {
        return;
    }

    uint32_t start   = lv_chart_get_x_start_point(chart, ser);
    uint32_t logical = (start + idx) % pc;
    int32_t  v       = y_arr[logical];
    if (v > 50) {
        v = 50;
    }
    if (v == LV_CHART_POINT_NONE) {
        return;
    }

    uint8_t mix = (uint8_t)((uint32_t)v * 255 / 50);
    fill->color = lv_color_mix(lv_palette_main(LV_PALETTE_GREEN),
                               lv_palette_main(LV_PALETTE_RED), mix);
}

void screen_wifi_beacon(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Scrollable container (from lcd_wifi_beacon_page) ── */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(cont, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(cont, 210, 106);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_ON);

    /* ── Chart ── */
    lv_obj_t *chart = lv_chart_create(cont);
    lv_obj_set_size(chart, 186, 60);
    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(chart, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, 20);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 50);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);

    lv_chart_series_t *series = lv_chart_add_series(chart,
                                                    lv_palette_main(LV_PALETTE_GREEN),
                                                    LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_set_style_width(chart, 8, LV_PART_INDICATOR);
    lv_obj_set_style_pad_column(chart, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chart, lv_palette_main(LV_PALETTE_GREEN), LV_PART_ITEMS);

    /* Per-bar color gradient via draw-task callback */
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(chart, beacon_chart_draw_cb, LV_EVENT_DRAW_TASK_ADDED, series);

    /* 20 varying signal samples (0-50 range) */
    static const int32_t bar_values[20] = {
        12, 28, 45, 33, 18, 40, 22,  8, 35, 48,
        15, 30, 42, 25, 10, 38, 20, 44, 32, 16
    };
    for (uint32_t i = 0; i < 20; i++) {
        lv_chart_set_value_by_id(chart, series, i, bar_values[i]);
    }
    lv_chart_refresh(chart);

    /* ── RSSI / SNR / SCROLL labels ── */
    lv_obj_t *lbl_rssi = lv_label_create(cont);
    format_label(lbl_rssi, "RSSI: -52", secondary,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, 0, -10);

    lv_obj_t *lbl_snr = lv_label_create(cont);
    format_label(lbl_snr, "SNR: 42", secondary,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_RIGHT, -5, -10);

    lv_obj_t *lbl_scroll = lv_label_create(cont);
    format_label(lbl_scroll, "SCROLL", secondary,
                 &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, 15);

    /* ── Info text below chart (scrolls into view) ── */
    lv_obj_t *lbl_info = lv_label_create(cont);
    lv_obj_set_style_text_color(lbl_info, secondary, 0);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_16, 0);
    lv_obj_set_width(lbl_info, 190);
    lv_obj_align_to(lbl_info, chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 25);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_info,
        "SSID:\n - MyHomeWifi_5GHz\n"
        "BSSID:\n - A4:C3:61:7B:9F:42\n"
        "Vendor:\n - Apple Inc.\n"
        "Channel:\n - 6 (2.4 GHz)\n"
        "Type:\n - 802.11ax (Wi-Fi 6)\n - 2.437 GHz\n"
        "Security:\n - WPA2/WPA3 Transition\n - AES (CCMP)\n - PMF: Optional\n"
        " - WPS: No\n"
        "Country Code:\n - US\n"
        "Supported Rates:\n - 1, 2, 5.5, 6, 9, 11,\n   12, 18, 24, 36, 48, 54\n"
        "HT Capabilities:\n - 20/40 MHz, SGI\n"
        "Compatibility Code:\n - 0x0411\n"
        "Beacon Interval:\n - 102 ms\n"
        "DTIM Period:\n - 1\n"
        "Time since reboot:\n - 14520m / 10 days");

    /* Register container for Up/Down scroll (firmware uses 55 px/press). */
    screen_set_scroll(cont, 55);

    /* ── "DATA →" label (bottom-right, outside container) ── */
    lv_obj_t *lbl_data = lv_label_create(scr);
    format_label(lbl_data, "DATA " LV_SYMBOL_RIGHT, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_RIGHT, -3, 0);

    /* ── Persistent UI (arrows + battery) ──
     * Beacon page: all four arrows visible; "DATA →" label sits at the
     * bottom-right corner, separate from the right-mid arrow. */
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

/* ─── Wi-Fi Data Frame scan page ─────────────────────────────────
 * Mirrors lcd_wifi_data_page() in components/lcd/src/lcd_wifi.c.
 * Bar chart of per-client packet counts (sorted desc), color-graded
 * red→green by count/max. MAC list below scrolls into view. */

#define DATA_CHART_MIN_PKTS_SIM 10

/* File-scope max so data_chart_draw_cb can scale bar colors (mirrors
 * lcd_wifi.c's data_chart_max_count). Written once per render. */
static int sim_data_chart_max_count = DATA_CHART_MIN_PKTS_SIM;

/* Mirror of data_chart_draw_cb() in lcd_wifi.c — colors bars red→green
 * based on value / max. */
static void data_chart_draw_cb(lv_event_t *e)
{
    lv_draw_task_t     *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);

    if (base->part != LV_PART_ITEMS) {
        return;
    }

    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    if (!fill) {
        return;
    }

    lv_obj_t          *chart = lv_event_get_target_obj(e);
    lv_chart_series_t *ser   = lv_event_get_user_data(e);
    int32_t           *y_arr = lv_chart_get_y_array(chart, ser);

    uint32_t pc = lv_chart_get_point_count(chart);
    if (pc == 0) {
        return;
    }

    uint32_t idx = base->id2;
    if (idx >= pc) {
        return;
    }

    uint32_t start   = lv_chart_get_x_start_point(chart, ser);
    uint32_t logical = (start + idx) % pc;
    int32_t  v       = y_arr[logical];
    if (v == LV_CHART_POINT_NONE) {
        return;
    }

    int32_t maxv = (sim_data_chart_max_count > 0) ? sim_data_chart_max_count : 1;
    if (v < 0) {
        v = 0;
    } else if (v > maxv) {
        v = maxv;
    }

    uint8_t mix = (uint8_t)(((uint32_t)v * 255u) / (uint32_t)maxv);
    fill->color = lv_color_mix(lv_palette_main(LV_PALETTE_GREEN),
                               lv_palette_main(LV_PALETTE_RED), mix);
}

void screen_wifi_data(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Fake client data — varied OUIs + random tail bytes, sorted by
     *    pkt_count desc (firmware sorts with qsort before rendering). ── */
    typedef struct {
        uint8_t  mac[6];
        uint32_t pkt_count;
    } sim_client_t;

    static const sim_client_t clients[] = {
        { { 0xA4, 0xC3, 0x61, 0x7B, 0x9F, 0x42 }, 320 },
        { { 0x78, 0xBD, 0xBC, 0x14, 0xE0, 0x3C }, 215 },
        { { 0xF0, 0x99, 0xBF, 0x52, 0x8A, 0xD1 }, 168 },
        { { 0x00, 0x1B, 0x21, 0x3F, 0xC4, 0x08 }, 124 },
        { { 0x3C, 0x15, 0xC2, 0xAA, 0x61, 0x9E },  92 },
        { { 0xE0, 0xD4, 0xE8, 0x11, 0x73, 0x2C },  77 },
        { { 0x54, 0xEE, 0x75, 0x29, 0xB0, 0x4F },  58 },
        { { 0x9C, 0xB6, 0xD0, 0x66, 0x1A, 0xE5 },  41 },
        { { 0x74, 0xE2, 0xF5, 0x08, 0x4D, 0x93 },  33 },
        { { 0xB8, 0x27, 0xEB, 0xC1, 0x7E, 0x30 },  27 },
        { { 0x68, 0x37, 0xE9, 0x50, 0x92, 0x2B },  22 },
        { { 0xDC, 0xA6, 0x32, 0x2D, 0x5C, 0x71 },  19 },
        { { 0x02, 0x4A, 0x7F, 0x8B, 0x46, 0xA9 },  14 },
        { { 0x0A, 0x00, 0x27, 0xC9, 0x31, 0x04 },  11 },
        { { 0x00, 0x0C, 0x29, 0x7E, 0xBF, 0x58 },   8 },
        { { 0x2C, 0xF0, 0x5D, 0x41, 0x90, 0x6B },   5 },
        { { 0xD4, 0x3B, 0x04, 0x67, 0xE2, 0x18 },   4 },
        { { 0x8C, 0x85, 0x90, 0xBA, 0x3F, 0xD7 },   3 },
        { { 0x40, 0xA3, 0x6B, 0x15, 0x7C, 0x82 },   2 },
        { { 0x1C, 0x87, 0x2C, 0x09, 0xA5, 0x4E },   1 },
    };
    const uint32_t num_clients = sizeof(clients) / sizeof(clients[0]);

    /* Compute max packet count (clamped to MIN_PKTS) — drives chart Y range
     * and the draw-cb's color gradient denominator. */
    uint32_t max_count = 1;
    for (uint32_t i = 0; i < num_clients; i++) {
        if (clients[i].pkt_count > max_count) {
            max_count = clients[i].pkt_count;
        }
    }
    if (max_count < DATA_CHART_MIN_PKTS_SIM) {
        max_count = DATA_CHART_MIN_PKTS_SIM;
    }
    sim_data_chart_max_count = (int)max_count;

    /* ── Scrollable container ── */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(cont, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(cont, 210, 106);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_ON);

    /* ── Chart ── */
    lv_obj_t *chart = lv_chart_create(cont);
    lv_obj_set_size(chart, 186, 60);
    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(chart, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, num_clients);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, sim_data_chart_max_count);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);

    lv_chart_series_t *series = lv_chart_add_series(chart,
                                                    lv_palette_main(LV_PALETTE_GREEN),
                                                    LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_set_style_width(chart, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_column(chart, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chart, lv_palette_main(LV_PALETTE_GREEN), LV_PART_ITEMS);

    /* Per-bar color gradient */
    lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    lv_obj_add_event_cb(chart, data_chart_draw_cb, LV_EVENT_DRAW_TASK_ADDED, series);

    /* Fill bars with packet counts */
    for (uint32_t i = 0; i < num_clients; i++) {
        lv_chart_set_value_by_id(chart, series, i, (int32_t)clients[i].pkt_count);
    }
    lv_chart_refresh(chart);

    /* ── "<N> users on Ch6@72Mbps" label at top of container ── */
    lv_obj_t *lbl_clients = lv_label_create(cont);
    char clients_buf[48];
    snprintf(clients_buf, sizeof(clients_buf),
             "%u users on Ch6@72Mbps", (unsigned)num_clients);
    format_label(lbl_clients, clients_buf, secondary,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, -10);

    /* ── SCROLL hint (initially below viewport) ── */
    lv_obj_t *lbl_scroll = lv_label_create(cont);
    format_label(lbl_scroll, "SCROLL", secondary,
                 &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, 15);

    /* ── MAC list (scrolls into view below chart) ── */
    lv_obj_t *lbl_info = lv_label_create(cont);
    lv_obj_set_style_text_color(lbl_info, secondary, 0);
    lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_info, 190);
    lv_obj_align_to(lbl_info, chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 25);

    static char info_buf[1024];
    size_t off = 0;
    off += snprintf(info_buf + off, sizeof(info_buf) - off,
                    "Unique users (MACs):\n");
    for (uint32_t i = 0; i < num_clients && off < sizeof(info_buf); i++) {
        const uint8_t *m    = clients[i].mac;
        const char    *unit = (clients[i].pkt_count > 1) ? "pkts" : "pkt";
        off += snprintf(info_buf + off, sizeof(info_buf) - off,
                        "%02X:%02X:%02X:%02X:%02X:%02X: %u%s\n",
                        m[0], m[1], m[2], m[3], m[4], m[5],
                        (unsigned)clients[i].pkt_count, unit);
    }
    lv_label_set_text(lbl_info, info_buf);

    /* Register container for Up/Down scroll (firmware uses 53 px/press). */
    screen_set_scroll(cont, 53);

    /* ── "◄ BEACON" label replaces the left arrow (back to beacon page) ── */
    lv_obj_t *lbl_beacon = lv_label_create(scr);
    format_label(lbl_beacon, LV_SYMBOL_LEFT " BEACON", secondary,
                 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 3, 0);

    /* ── Persistent UI (up/down/left + battery).  Right arrow is hidden on
     *    this page; "◄ BEACON" label sits at bottom-left, separate from the
     *    left-mid arrow used for general back navigation. ── */
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

/* ─── Bluetooth AI Keyboard page ─────────────────────────────────
 * Mirrors lcd_bluetooth_ai_keyboard_page() in components/lcd/src/lcd_bluetooth.c
 * in its AI_KEYB_IDLE ("Hold & talk!") ready state — both Wi-Fi and BLE
 * connected, orb hidden, reasoning toggle footer shown.
 *
 * Simulator hook: pressing Down toggles the recording state, mirroring the
 * firmware's select-button recording behavior — hides the text prompts,
 * shows the orb, and spins/pulses it via an lv_anim rotation + periodic
 * image-source swap to mimic voice pickup. */

/* Handles of widgets the recording animation manipulates. */
static lv_obj_t  *aikb_lbl_ins       = NULL;
static lv_obj_t  *aikb_lbl_reasoning = NULL;
static lv_obj_t  *aikb_lbl_config    = NULL;
static lv_obj_t  *aikb_arrow_bot     = NULL;
static lv_obj_t  *aikb_orb           = NULL;
static lv_timer_t *aikb_pulse_timer  = NULL;
static bool       aikb_recording     = false;

/* Orb burst state machine — cycles frames 1→2→3→2→1 to mimic a voice
 * burst, then sleeps a randomized gap before the next burst.  step 0 =
 * idle gap, 1..5 = positions within the 5-frame sequence. */
static int aikb_pulse_step = 0;
static int aikb_pulse_gap  = 0;

/* Loading dot (ported from lcd_anim_loading_start) — a pulsing circle
 * that slides across a small invisible container, running continuously
 * while the AI keyboard page is active. */
static lv_obj_t *aikb_loading_cont = NULL;
static lv_obj_t *aikb_loading_dot  = NULL;

/* Re-center the orb's rotation pivot for its current image size. The three
 * orb frames are different sizes (45/60/75 px) so the pivot must be
 * recomputed each time the source image changes, otherwise rotation
 * happens around a fixed pixel offset that's only the center of one
 * frame. Re-align to CENTER so the newly-sized image doesn't shift off. */
static void aikb_orb_recenter_pivot(void)
{
    if (!aikb_orb) return;
    lv_obj_update_layout(aikb_orb);
    int w = lv_obj_get_width(aikb_orb);
    int h = lv_obj_get_height(aikb_orb);
    lv_obj_set_style_transform_pivot_x(aikb_orb, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(aikb_orb, h / 2, 0);
    lv_obj_align(aikb_orb, LV_ALIGN_CENTER, 0, 0);
}

/* Pulse timer: cycles the orb through the fixed 1→2→3→2→1 burst
 * sequence to imitate a voice burst, then waits a random gap before the
 * next burst. The deterministic sequence keeps the animation looking
 * correct (grow then shrink) while the randomized gaps make bursts feel
 * natural — short pauses between words / phonemes. */
static void aikb_pulse_cb(lv_timer_t *t)
{
    (void)t;
    if (!aikb_orb) return;

    static const void *const frames[] = {
        &img_ai_orb_1, &img_ai_orb_2, &img_ai_orb_3
    };
    /* Indices into frames[] for the burst sequence: 1, 2, 3, 2, 1. */
    static const uint8_t BURST_SEQ[5] = { 0, 1, 2, 1, 0 };

    if (aikb_pulse_step == 0) {
        /* Between bursts — decrement gap; stay on the smallest frame. */
        if (aikb_pulse_gap > 0) {
            aikb_pulse_gap--;
            return;
        }
        /* Gap elapsed; fall through to play step 0 of a new burst. */
    }

    lv_image_set_src(aikb_orb, frames[BURST_SEQ[aikb_pulse_step]]);
    aikb_orb_recenter_pivot();

    aikb_pulse_step++;
    if (aikb_pulse_step >= 5) {
        /* Burst complete — pick a weighted-random gap so bursts arrive at
         * unpredictable intervals, mimicking words of varying length and
         * natural pauses between phrases. The sequence itself is always
         * 1→2→3→2→1 (rising-then-falling) so the shape stays correct. */
        aikb_pulse_step = 0;
        int r = rand() % 10;
        if (r < 4) {
            /* 40%: near-zero gap — rapid-fire run-on words. */
            aikb_pulse_gap = rand() % 2;          /* 0–1 tick   */
        } else if (r < 8) {
            /* 40%: short gap — normal word spacing. */
            aikb_pulse_gap = 2 + (rand() % 3);    /* 2–4 ticks  */
        } else {
            /* 20%: long gap — pause between phrases / thinking. */
            aikb_pulse_gap = 8 + (rand() % 12);   /* 8–19 ticks */
        }
    }
}

/* lv_anim exec-cb: drives continuous rotation of the orb. */
static void aikb_orb_rotate_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)var, v, 0);
}

/* ── Loading dot animation (ported from lcd_anim_loading_*) ──
 * A small circular dot that pulses in size and slides across a narrow
 * invisible container parked in the bottom-right corner. */

static void aikb_loading_size_cb(void *var, int32_t v)
{
    lv_obj_t *obj    = (lv_obj_t *)var;
    lv_obj_t *parent = lv_obj_get_parent(obj);

    lv_obj_set_size(obj, (lv_coord_t)v, (lv_coord_t)v);

    /* Keep the dot vertically centered inside its container as it grows. */
    if (parent) {
        lv_coord_t h = lv_obj_get_height(parent);
        lv_obj_set_y(obj, (h - (lv_coord_t)v) / 2);

        lv_coord_t max_x = lv_obj_get_width(parent) - lv_obj_get_width(obj);
        if (max_x < 0) max_x = 0;
        lv_coord_t x = lv_obj_get_x(obj);
        if (x > max_x)     lv_obj_set_x(obj, max_x);
        else if (x < 0)    lv_obj_set_x(obj, 0);
    }
}

static void aikb_loading_x_cb(void *var, int32_t v)
{
    lv_obj_t *obj    = (lv_obj_t *)var;
    lv_obj_t *parent = lv_obj_get_parent(obj);

    if (parent) {
        lv_coord_t max_x = lv_obj_get_width(parent) - lv_obj_get_width(obj);
        if (max_x < 0) max_x = 0;
        if (v < 0)       v = 0;
        else if (v > max_x) v = max_x;
    }

    lv_obj_set_x(obj, (lv_coord_t)v);
}

static void aikb_loading_start(lv_obj_t *scr, lv_color_t color)
{
    const lv_coord_t min_sz = 6;
    const lv_coord_t max_sz = 24;

    /* Invisible container in the bottom-right corner. */
    aikb_loading_cont = lv_obj_create(scr);
    lv_obj_set_size(aikb_loading_cont, max_sz + 1, max_sz + 1);
    lv_obj_set_style_bg_opa(aikb_loading_cont, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(aikb_loading_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(aikb_loading_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(aikb_loading_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(aikb_loading_cont, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    aikb_loading_dot = lv_obj_create(aikb_loading_cont);
    lv_obj_set_style_radius(aikb_loading_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(aikb_loading_dot, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(aikb_loading_dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(aikb_loading_dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(aikb_loading_dot, min_sz, min_sz);
    lv_obj_align(aikb_loading_dot, LV_ALIGN_LEFT_MID, 0, 0);

    /* Pulse (size) */
    lv_anim_t a_size;
    lv_anim_init(&a_size);
    lv_anim_set_var(&a_size, aikb_loading_dot);
    lv_anim_set_exec_cb(&a_size, aikb_loading_size_cb);
    lv_anim_set_values(&a_size, min_sz, max_sz);
    lv_anim_set_duration(&a_size, 900);
    lv_anim_set_playback_delay(&a_size, 80);
    lv_anim_set_playback_duration(&a_size, 280);
    lv_anim_set_repeat_delay(&a_size, 250);
    lv_anim_set_repeat_count(&a_size, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_size, lv_anim_path_ease_in_out);
    lv_anim_start(&a_size);

    /* Travel (x) */
    lv_anim_t a_x;
    lv_anim_init(&a_x);
    lv_anim_set_var(&a_x, aikb_loading_dot);
    lv_anim_set_exec_cb(&a_x, aikb_loading_x_cb);
    lv_anim_set_values(&a_x, 0, lv_obj_get_width(aikb_loading_cont));
    lv_anim_set_duration(&a_x, 1100);
    lv_anim_set_repeat_count(&a_x, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_x, lv_anim_path_ease_in_out);
    lv_anim_start(&a_x);
}

static void aikb_loading_stop(void)
{
    if (aikb_loading_dot) {
        lv_anim_delete(aikb_loading_dot, aikb_loading_size_cb);
        lv_anim_delete(aikb_loading_dot, aikb_loading_x_cb);
    }
    aikb_loading_dot  = NULL;
    aikb_loading_cont = NULL;
}

static void aikb_start_recording(void)
{
    if (!aikb_orb || aikb_recording) return;

    /* Hide text + down arrow; show orb.  Settings gear stays visible
     * throughout (gives a persistent right-side affordance while talking). */
    lv_obj_add_flag(aikb_lbl_ins, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(aikb_lbl_reasoning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(aikb_arrow_bot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(aikb_orb, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(aikb_orb, &img_ai_orb_1);
    aikb_orb_recenter_pivot();

    /* Continuous rotation: 2 s per revolution, looping forever. */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, aikb_orb);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_duration(&a, 2000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, aikb_orb_rotate_cb);
    lv_anim_start(&a);

    /* Start burst state machine at the beginning of sequence with no gap
     * so the first burst fires on the next timer tick. */
    aikb_pulse_step = 0;
    aikb_pulse_gap  = 0;
    if (!aikb_pulse_timer) {
        /* 30 ms/tick → a 5-frame burst plays in ~150 ms, snappy enough to
         * read as an individual word.  Weighted-random gaps then cluster
         * bursts into run-ons, normal word-spaced groups, and occasional
         * phrase pauses — so the orb looks like it's catching random
         * words rather than a metronome pulse. */
        aikb_pulse_timer = lv_timer_create(aikb_pulse_cb, 30, NULL);
    }

    aikb_recording = true;
}

static void aikb_stop_recording(void)
{
    if (!aikb_orb || !aikb_recording) return;

    /* Stop spin + pulse. */
    lv_anim_delete(aikb_orb, aikb_orb_rotate_cb);
    if (aikb_pulse_timer) {
        lv_timer_delete(aikb_pulse_timer);
        aikb_pulse_timer = NULL;
    }

    /* Reset orb appearance then hide. */
    lv_image_set_src(aikb_orb, &img_ai_orb_1);
    lv_obj_set_style_transform_rotation(aikb_orb, 0, 0);
    lv_obj_add_flag(aikb_orb, LV_OBJ_FLAG_HIDDEN);

    /* Restore text + down arrow (settings gear was never hidden). */
    lv_obj_remove_flag(aikb_lbl_ins, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(aikb_lbl_reasoning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(aikb_arrow_bot, LV_OBJ_FLAG_HIDDEN);

    aikb_recording = false;
}

/* Down-key handler — toggle recording (click once to start, click again
 * to stop). */
static void aikb_on_down(void)
{
    if (aikb_recording) {
        aikb_stop_recording();
    } else {
        aikb_start_recording();
    }
}

/* Runs from screen_menu_reset() before lv_obj_clean() destroys our widgets.
 * Tears down the pulse timer and any running rotation animation whose var
 * pointer is about to become stale. */
static void aikb_cleanup(void)
{
    if (aikb_pulse_timer) {
        lv_timer_delete(aikb_pulse_timer);
        aikb_pulse_timer = NULL;
    }
    if (aikb_orb) {
        lv_anim_delete(aikb_orb, NULL);
    }
    /* Loading dot lives for the page's lifetime — cancel its anims before
     * lv_obj_clean() destroys the objects. */
    aikb_loading_stop();
    aikb_lbl_ins = aikb_lbl_reasoning = aikb_lbl_config = NULL;
    aikb_arrow_bot = aikb_orb = NULL;
    aikb_pulse_step = 0;
    aikb_pulse_gap  = 0;
    aikb_recording  = false;
}

void screen_ai_keyboard(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Main "Hold & talk!" prompt (idle/ready state) ── */
    aikb_lbl_ins = lv_label_create(scr);
    format_label(aikb_lbl_ins, "Hold & talk!", secondary,
                 &lv_font_montserrat_22, LV_ALIGN_CENTER, 0, 0);

    /* ── Reasoning-mode indicator (bottom-mid, toggled by Down button) ── */
    aikb_lbl_reasoning = lv_label_create(scr);
    format_label(aikb_lbl_reasoning, "Use: non-reasoning", secondary,
                 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -14);

    /* ── Settings gear (right-mid; replaces the right arrow on this page) ── */
    aikb_lbl_config = lv_label_create(scr);
    format_label(aikb_lbl_config, LV_SYMBOL_SETTINGS, secondary,
                 &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -16, 0);

    /* ── AI orb image (hidden until recording starts) ── */
    aikb_orb = lv_image_create(scr);
    lv_image_set_src(aikb_orb, &img_ai_orb_1);
    lv_obj_align(aikb_orb, LV_ALIGN_CENTER, 0, 0);
    /* Give rotation some headroom so diagonals aren't clipped. */
    lv_obj_set_style_transform_width(aikb_orb, 8, 0);
    lv_obj_set_style_transform_height(aikb_orb, 8, 0);
    /* Pivot is re-computed each pulse frame (images are 45/60/75 px, so a
     * fixed pivot would only be centered for one of them). */
    aikb_orb_recenter_pivot();
    lv_obj_add_flag(aikb_orb, LV_OBJ_FLAG_HIDDEN);

    /* ── Persistent Wi-Fi + BT connected icons (top-left, stacked) ──
     * Offsets match lcd_update_icons() in lcd_utils.c for the wifi+bt-both
     * state: Wi-Fi at (3, 0), Bluetooth at (5, 22). */
    lv_obj_t *lbl_wifi_icon = lv_label_create(scr);
    format_label(lbl_wifi_icon, LV_SYMBOL_WIFI, secondary,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, 3, 0);

    lv_obj_t *lbl_bt_icon = lv_label_create(scr);
    format_label(lbl_bt_icon, LV_SYMBOL_BLUETOOTH, secondary,
                 &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 5, 22);

    /* ── Persistent UI (arrows + battery).  The top arrow is intentionally
     *    omitted on this page; left/right/down are all visible in ready
     *    state.  Down toggles recording — while recording, down/settings
     *    get hidden but left & right remain visible. ── */
    lv_obj_t *arrow_left = lv_label_create(scr);
    format_label(arrow_left, LV_SYMBOL_LEFT, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *arrow_right = lv_label_create(scr);
    format_label(arrow_right, LV_SYMBOL_RIGHT, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_RIGHT_MID, -4, 0);

    aikb_arrow_bot = lv_label_create(scr);
    format_label(aikb_arrow_bot, LV_SYMBOL_DOWN, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* Battery */
    lv_obj_t *lbl_bat_txt = lv_label_create(scr);
    format_label(lbl_bat_txt, DEFAULT_BATTERY_LV, secondary,
                 &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -28, 0);

    lv_obj_t *lbl_bat_icon = lv_label_create(scr);
    format_label(lbl_bat_icon, LV_SYMBOL_BATTERY_FULL, secondary,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT, -2, -3);

    /* Persistent loading-dot animation at bottom-right of the page. */
    aikb_loading_start(scr, secondary);

    /* Hook Down into the recording toggle; cleanup tears down timer/anim
     * before the next screen is loaded. */
    aikb_recording  = false;
    aikb_pulse_step = 0;
    aikb_pulse_gap  = 0;
    screen_set_nav_handlers(NULL, aikb_on_down);
    screen_set_cleanup(aikb_cleanup);
}

/* ─── GPIO submenu page ──────────────────────────────────────────
 * Mirrors lcd_gpio_setup_page() in components/lcd/src/lcd_gpio.c.
 * Three-option vertical list; default selection is index 0 ("How It Works")
 * so the first thing a new user reads is the explainer page. */

void screen_gpio(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Menu list ── */
    lv_obj_t *main_list = lv_list_create(scr);
    lv_obj_set_size(main_list, 210, 106);
    lv_obj_set_style_bg_color(main_list, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    apply_scrollbar_style(main_list);
    lv_obj_set_scroll_dir(main_list, LV_DIR_VER);

    /* ── Button style (unselected) ── */
    static lv_style_t gpio_btn_style;
    lv_style_init(&gpio_btn_style);
    lv_style_set_radius(&gpio_btn_style, 8);
    lv_style_set_bg_color(&gpio_btn_style, primary);
    lv_style_set_border_width(&gpio_btn_style, 2);
    lv_style_set_border_color(&gpio_btn_style, secondary);
    lv_style_set_border_side(&gpio_btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&gpio_btn_style, 3);
    lv_style_set_pad_bottom(&gpio_btn_style, 3);
    lv_style_set_text_font(&gpio_btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&gpio_btn_style, secondary);
    lv_style_set_text_align(&gpio_btn_style, LV_TEXT_ALIGN_CENTER);

    /* ── Selected style ── */
    static lv_style_t gpio_sel_style;
    lv_style_init(&gpio_sel_style);
    lv_style_set_radius(&gpio_sel_style, 8);
    lv_style_set_bg_color(&gpio_sel_style, secondary);
    lv_style_set_border_width(&gpio_sel_style, 2);
    lv_style_set_border_color(&gpio_sel_style, secondary);
    lv_style_set_border_side(&gpio_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_pad_top(&gpio_sel_style, 3);
    lv_style_set_pad_bottom(&gpio_sel_style, 3);
    lv_style_set_text_font(&gpio_sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&gpio_sel_style, primary);
    lv_style_set_text_align(&gpio_sel_style, LV_TEXT_ALIGN_CENTER);

    /* ── Options — matches the static initializer in lcd_gpio.c. ── */
    static const char *gpio_options[] = {
        "How It Works", "Terminal", "I2C Scanner"
    };
    int num_options = sizeof(gpio_options) / sizeof(gpio_options[0]);
    int selected = 0;

    for (int i = 0; i < num_options; i++) {
        lv_obj_t *btn = lv_list_add_btn(main_list, NULL, gpio_options[i]);
        lv_obj_set_size(btn, 200, 30);

        if (i == selected) {
            lv_obj_add_style(btn, &gpio_sel_style, 0);
        } else {
            lv_obj_add_style(btn, &gpio_btn_style, 0);
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
    active_menu.btn_style = &gpio_btn_style;
    active_menu.sel_style = &gpio_sel_style;

    /* ── Persistent UI (arrows + battery).  GPIO page inherits arrow state
     *    from selection: up/down/left visible, right hidden. ── */
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

/* ─── GPIO I2C Terminal page ─────────────────────────────────────
 * Mirrors lcd_gpio_terminal_page() in components/lcd/src/lcd_gpio.c.
 * Shows the scrollable terminal container with title, instructions, and a
 * single successful send→receive round-trip (Command 42 → "Hello ESP32!"). */

void screen_gpio_terminal(void)
{
    lv_color_t primary   = USER_PRIMARY_COLOR;
    lv_color_t secondary = USER_SECONDARY_COLOR;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, primary, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ── Scrollable terminal container (rounded, bordered, shadowed) ── */
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 210, 106);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, primary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(cont, secondary, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* ── Title ── */
    lv_obj_t *title_lbl = lv_label_create(cont);
    lv_label_set_text(title_lbl, "I2C Terminal");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title_lbl, secondary, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

    /* ── Log label (scrolls into view as content grows) ── */
    lv_obj_t *log_lbl = lv_label_create(cont);
    lv_label_set_long_mode(log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(log_lbl, lv_pct(100));
    lv_obj_set_style_text_font(log_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(log_lbl, secondary, 0);
    lv_obj_align_to(log_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /* One successful round-trip: up/down adjusted the command to 42, then
     * select sent it to 0x2A and the slave replied with "Hello ESP32!".
     * Format matches the firmware's term_log_append() output exactly. */
    lv_label_set_text(log_lbl,
        // "Use up/down to adjust.\n"
        // "Press select to send.\n"
        // "Command: 42\n"
        // "\n"
        "Sent: 42 (0x2A) to 0x2A\n"
        "Response: Hello world!\n\n");
        // "Sent: 41 (0x2A) to 0x2A\n"
        // "Response: Hello ESP32!\n\n"

    /* Register for Up/Down scroll (the firmware's up/down change the
     * command; here the static page just scrolls so the whole log is
     * reachable in the simulator). */
    screen_set_scroll(cont, 40);

    /* ── Persistent UI (arrows + battery).  Terminal inherits up/down/left
     *    from the GPIO submenu; right is hidden. ── */
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
