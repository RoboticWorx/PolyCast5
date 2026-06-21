#include "polycast5_macros.h"
#include "polycast5_fonts.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#include "core/lv_obj_scroll.h"
#include "font/lv_symbol_def.h"
#include "core/lv_obj_pos.h"
#include "core/lv_obj.h"
#include "misc/lv_style.h"
#include "misc/lv_area.h"
#include "misc/lv_anim.h"
#include "widgets/label/lv_label.h"
#include "widgets/image/lv_image.h"
#include "widgets/bar/lv_bar.h"
#include "draw/lv_image_dsc.h"

#include "tca9535.h"
#include "gpio_task.h"
#include "lis2dh12.h"
#include "mmc5603.h"
#include "lcd_utils.h"
#include "lcd_espnow.h"   // espnow_menu, espnow_entry_mode
#include "espnow_task.h"  // xEspAccelStreamCtrlQueue, xEspAccelStreamQueue
#include "lcd_gpio.h"

#define TAG "LCD_GPIO"

gpio_menu_t gpio_menu = {
    .options = {"How It Works", "Magcelerometer", "Terminal", "I2C Scanner"},
    .size = 4,
    .index = 1,
    .cont = NULL,
};

void lcd_gpio_setup_page(gpio_menu_t *menu)
{
    // Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 210, 106);
    
    // Format
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lcd_apply_scrollbar_style(menu->main_list);
    lv_obj_set_scroll_dir(menu->main_list, LV_DIR_VER);

    // Create button style
    lv_style_init(&menu->btn_style);
    
    lv_style_set_radius(&menu->btn_style, 8);
    lv_style_set_bg_color(&menu->btn_style, user_primary_color);
    
    lv_style_set_border_width(&menu->btn_style, 2);
    lv_style_set_border_color(&menu->btn_style, user_secondary_color);
    lv_style_set_border_side(&menu->btn_style, LV_BORDER_SIDE_FULL);
    
    lv_style_set_pad_top(&menu->btn_style, 3);
    lv_style_set_pad_bottom(&menu->btn_style, 3);
    
    lv_style_set_text_font(&menu->btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->btn_style, user_secondary_color);
    lv_style_set_text_align(&menu->btn_style, LV_TEXT_ALIGN_CENTER);
    
    // Create selected button style
    lv_style_init(&menu->sel_style);
    
    lv_style_set_radius(&menu->sel_style, 8);
    lv_style_set_bg_color(&menu->sel_style, user_secondary_color);
    
    lv_style_set_border_width(&menu->sel_style, 2);
    lv_style_set_border_color(&menu->sel_style, user_secondary_color);
    lv_style_set_border_side(&menu->sel_style, LV_BORDER_SIDE_FULL);
    
    lv_style_set_pad_top(&menu->sel_style, 3);
    lv_style_set_pad_bottom(&menu->sel_style, 3);
    
    lv_style_set_text_font(&menu->sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->sel_style, user_primary_color);
    lv_style_set_text_align(&menu->sel_style, LV_TEXT_ALIGN_CENTER);
    
    
    // Create buttons
    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }
    
    // Create button for each option
    for (int i = 0; i < menu->size; ++i) {

        menu->btns[i] = lv_list_add_btn(menu->main_list, NULL, menu->options[i]);
        lv_obj_set_size(menu->btns[i], 200, 30);

        // Style selected
        if (i == menu->index) {
            lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
        } else {
            lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
        }

        // Create and format text label
        lv_obj_t *lbl = lv_obj_get_child(menu->btns[i], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }

    // Format buttons as container
    menu->cont = lv_obj_get_parent(menu->btns[0]);
    lv_obj_set_flex_flow (menu->cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(menu->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
    
    // Hide for now
    lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_gpio_update_menu(gpio_menu_t *menu)
{
    // Reveal
    lv_obj_remove_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);

    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }

    // Reset every button to unselected
    for (int i = 0; i < menu->size; ++i) {
        lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
        lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);
    
    // Enable scrolling if list gets too long
    lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
}

void lcd_gpio_how_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define HOW_Y_OFFSET 40
    
    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;
    
    if (!init) {
        // Create a scrollable container for the instructions
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
        lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content

        // Title label
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "How It Works:");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instructions label (scrollable if text is long)
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        // Set custom text to explain to the user
        const char *instr_text =
                                "PolyCast5 is expandable with external hardware to allow for infinite control possibilities.\n\n"
                                "This device will communicate with the external hardware via I2C through the 'Terminal' option.\n\n"
                                "In the terminal, simply enter a command for the connected hardware to receive and execute.\n\n"
                                "From there, PolyCast5 will also print any responses from the connected hardware for your convenience. "
                                "(e.g. status, errors, etc.)\n\n"
                                "To get started building your own connectible hardware, please visit:\n\n"
                                "polycast5.com/blogs /tutorials/create-external-hardware";
        
        lv_label_set_text(instr_lbl, instr_text);

        lv_timer_handler();

        init = true;
    }
    
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, HOW_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -HOW_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->left_btn) { // Go back
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
            
        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

static void prompt_accel_espnow_qr(ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    static lv_obj_t *qr_canvas = NULL;
    static uint8_t *qr_buf = NULL; // Canvas backing buffer
    
    // Hide arrows
    lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
    
    // Create and format ins labels
    lv_obj_t *lbl_ask_enc = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ask_enc, "Use wirelessly:", user_secondary_color,
            &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 11, 6);
    
    lv_obj_t *lbl_qr_ok = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_qr_ok, "OK", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);

    lv_obj_t *lbl_qr_back = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_qr_back, "BACK", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_LEFT_MID, 16, -1);
    
    // Create QR canvas
    qr_canvas = lv_canvas_create(ACTIVE_SCR);
    lv_obj_set_size(qr_canvas, 100, 100);
    lv_obj_align(qr_canvas, LV_ALIGN_CENTER, 11, 12);
    
    // Draw the URL as a QR
    const char *url = "https://polycast5.com/blogs/tutorials/control-custom-builds-with-accelerometer";
    int n = lcd_draw_qr(qr_canvas, url, 100, &qr_buf);
    if (n != 0) {
        ESP_LOGE(TAG, "prompt_accel_espnow_qr lcd_draw_qr failed: %d", n);
    }
    
    while (1) {
        lv_timer_handler();
        
        // OK -> pick an ESP-NOW device to stream the readings to
        if (xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {
            // Delete used
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_qr_ok);
            lv_obj_delete(lbl_qr_back);
            lv_obj_delete(qr_canvas);

            // Free QR buffer
            if (qr_buf) {
                free(qr_buf);
                qr_buf = NULL;
            }

            qr_canvas = NULL;

            // List-navigation arrows for the device picker
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // No right arrow

            lcd_clear_pending_inputs = true; // Clear any false inputs

            // Enter the ESP-NOW device picker in "accel streaming" mode
            espnow_entry_mode = ESPNOW_ENTRY_ACCEL; // Set picker flag

            // Go to page
            lv_obj_remove_flag(espnow_menu.main_list, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = ESPNOW_PAGE;
            return;
        }

        // BACK
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {
            // Show arrows
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            
            // Delete used
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_qr_ok);
            lv_obj_delete(lbl_qr_back);
            lv_obj_delete(qr_canvas);
        
            // Free QR buffer
            if (qr_buf) {
                free(qr_buf);
                qr_buf = NULL;
            }
            
            qr_canvas = NULL;
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Go back
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Bubble-level view for accel + ESP-NOW stream page

#define LEVEL_D      88    // Bubble-level circle diameter (px)
#define BALL_D       18    // Moving ball diameter (px)
#define MAX_TILT_DEG 45.0f // Tilt that pushes the ball to the circle edge
#define DEG2RAD      0.017453292f

#define MODE_FLAT    0
#define MODE_HOLDING 1
#define MODE_REMOTE  2 // Upright, then rotated 90 deg to the right
#define MODE_COUNT   3

#define ARROW_SIZE 34      // Square ARGB image holding the arrow (px)
#define ARROW_FILL_SCALE 0.74f // Inner fill inset, leaving a white border rim
#define ARROW_SMOOTH 0.5f // Heading low-pass factor (0..1, higher = snappier / less lag)

// Magnetometer hard-/soft-iron calibration persistence (NVS)
// Stores the four X/Y min/max bounds so the compass works on every boot without a fresh calibration turn
#define MAGCAL_NVS_NS  "magcal"
#define MAGCAL_NVS_KEY "minmax"

typedef struct {
    float x_min, x_max, y_min, y_max;
} magcal_blob_t;

// Static ARGB8888 arrow image, re-rasterised on each page entry to track the accent colour
POLYCAST5_USE_PSRAM_BSS static uint8_t arrow_px[ARROW_SIZE * ARROW_SIZE * 4] __attribute__((aligned(4)));
static lv_image_dsc_t arrow_dsc;

// Hard-/soft-iron compass calibration state: Min/max of the X/Y field: centre = midpoint, radius = half-span
static float mag_cal_x_min = 1e9f, mag_cal_x_max = -1e9f;
static float mag_cal_y_min = 1e9f, mag_cal_y_max = -1e9f;
static bool magcal_loaded = false; // NVS calibration loaded this boot?
static bool cal_complete = false;  // Do we have a usable calibration (from NVS or the cal page)?

// Centre of the calibration ellipse (hard-iron offset), used by both the compass and cal pages
static inline float magcal_center_x(void) { return (mag_cal_x_min + mag_cal_x_max) * 0.5f; }
static inline float magcal_center_y(void) { return (mag_cal_y_min + mag_cal_y_max) * 0.5f; }

// Arrow outline as offsets from the image centre, tip pointing up
static const float arrow_pts[4][2] = {
    {  0.0f, -13.0f }, // Tip
    { 10.0f,  11.0f }, // Right wing
    {  0.0f,   5.0f }, // Centre notch
    {-10.0f,  11.0f }, // Left wing
};

// Current orientation mode
static uint8_t accel_mode = MODE_FLAT;

// Ease one axis of the bubble-level ball from its current offset to a target
static void accel_ball_ease(lv_obj_t *ball, lv_anim_exec_xcb_t cb, int32_t from, int32_t to)
{
    #define ACCEL_EASE_MS 100

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ball);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ACCEL_EASE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Display name for each accelerometer orientation mode
static const char *accel_mode_name(uint8_t m)
{
    static const char *names[] = { "Flat", "Holding", "Remote" };
    return (m < 3) ? names[m] : "";
}

// Signed edge function (>0, <0 or 0 tells which side of segment AB the point P is on)
static inline float arrow_edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// True if (px,py) is inside the concave arrow scaled by k about the image centre
static bool arrow_hit(float px, float py, float k)
{
    const float ctr = ARROW_SIZE / 2.0f;
    const float tx = ctr + arrow_pts[0][0] * k, ty = ctr + arrow_pts[0][1] * k; // tip
    const float rx = ctr + arrow_pts[1][0] * k, ry = ctr + arrow_pts[1][1] * k; // right
    const float nx = ctr + arrow_pts[2][0] * k, ny = ctr + arrow_pts[2][1] * k; // notch
    const float lx = ctr + arrow_pts[3][0] * k, ly = ctr + arrow_pts[3][1] * k; // left

    // Inside if within triangle(tip,right,notch) OR triangle(tip,notch,left).
    // A point is in a triangle when its three edge functions all share one sign.
    float a = arrow_edge(tx, ty, rx, ry, px, py);
    float b = arrow_edge(rx, ry, nx, ny, px, py);
    float c = arrow_edge(nx, ny, tx, ty, px, py);
    bool in1 = (a <= 0 && b <= 0 && c <= 0) || (a >= 0 && b >= 0 && c >= 0);

    a = arrow_edge(tx, ty, nx, ny, px, py);
    b = arrow_edge(nx, ny, lx, ly, px, py);
    c = arrow_edge(lx, ly, tx, ty, px, py);
    bool in2 = (a <= 0 && b <= 0 && c <= 0) || (a >= 0 && b >= 0 && c >= 0);

    return in1 || in2;
}

// Rasterise the arrow into arrow_px (accent fill + white border, transparent elsewhere)
// and point the static descriptor at it
static void arrow_rasterize(lv_color_t fill)
{
    const uint32_t white  = 0xFFFFFFFFu;
    const uint32_t accent = ((uint32_t)0xFF << 24) | ((uint32_t)fill.red << 16) |
                            ((uint32_t)fill.green << 8) | (uint32_t)fill.blue;
    uint32_t *px = (uint32_t *)arrow_px;

    for (int y = 0; y < ARROW_SIZE; y++) {
        for (int x = 0; x < ARROW_SIZE; x++) {
            const float fx = x + 0.5f, fy = y + 0.5f; // sample pixel centres
            uint32_t v = 0; // transparent
            if (arrow_hit(fx, fy, ARROW_FILL_SCALE)) {
                v = accent;
            } else if (arrow_hit(fx, fy, 1.0f)) {
                v = white; // border rim
            }
            px[y * ARROW_SIZE + x] = v;
        }
    }

    arrow_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    arrow_dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    arrow_dsc.header.w      = ARROW_SIZE;
    arrow_dsc.header.h      = ARROW_SIZE;
    arrow_dsc.header.stride = ARROW_SIZE * 4;
    arrow_dsc.data_size     = sizeof(arrow_px);
    arrow_dsc.data          = arrow_px;
}

// Build the bubble-level view: circle + crosshair + moving indicator, plus the mode name and X/Y readout on the right
// When use_arrow is set, the moving indicator is a rotating direction arrow (accel page); otherwise the classic ball
static void accel_build_bubble(ui_menu_t *ui_menu, lv_obj_t *cont, bool use_arrow, lv_obj_t **out_ball, lv_obj_t **out_mode_lbl, lv_obj_t **out_val_lbl)
{
    #define X_OFFSET 15 // Move all to the right a bit

    // Bubble-level circle (left side)
    lv_obj_t *level_bg = lv_obj_create(cont);
    lv_obj_set_size(level_bg, LEVEL_D, LEVEL_D);
    lv_obj_align(level_bg, LV_ALIGN_LEFT_MID, X_OFFSET, 0);
    lv_obj_set_style_radius(level_bg, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(level_bg, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(level_bg, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(level_bg, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(level_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(level_bg, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Faint crosshair through the centre
    lv_obj_t *h_line = lv_obj_create(level_bg);
    lv_obj_set_size(h_line, LEVEL_D - 8, 1);
    lv_obj_align(h_line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(h_line, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(h_line, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(h_line, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(h_line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *v_line = lv_obj_create(level_bg);
    lv_obj_set_size(v_line, 1, LEVEL_D - 8);
    lv_obj_align(v_line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(v_line, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(v_line, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(v_line, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(v_line, LV_OBJ_FLAG_SCROLLABLE);

    // Moving indicator (created last so it draws on top of the crosshair):
    // a rotating direction arrow, or the classic bubble ball
    lv_obj_t *ball;
    if (use_arrow) {
        arrow_rasterize(user_secondary_color); // Build the arrow in the current accent colour
        ball = lv_image_create(level_bg);
        lv_image_set_src(ball, &arrow_dsc);
        lv_obj_set_size(ball, ARROW_SIZE, ARROW_SIZE);
        lv_image_set_pivot(ball, ARROW_SIZE / 2, ARROW_SIZE / 2); // Rotate about its centre
        lv_image_set_antialias(ball, true);
        lv_obj_align(ball, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    } else { // Classic ball (stream page)
        ball = lv_obj_create(level_bg);
        lv_obj_set_size(ball, BALL_D, BALL_D);
        lv_obj_align(ball, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ball, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ball, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ball, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(ball, LV_OBJ_FLAG_SCROLLABLE);
    }

    // Right-side panel: mode name on top, X/Y readout centred below
    lv_obj_t *right_panel = lv_obj_create(cont);
    lv_obj_set_size(right_panel, 110, lv_pct(100));
    lv_obj_align(right_panel, LV_ALIGN_RIGHT_MID, X_OFFSET, 0);
    lv_obj_set_style_bg_opa(right_panel, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(right_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(right_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(right_panel, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Current mode name (above the readout)
    lv_obj_t *mode_lbl = lv_label_create(right_panel);
    lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(mode_lbl, user_secondary_color, 0);
    lv_obj_set_style_text_align(mode_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));

    // Sending header shown when streaming via ESP-NOW
    if (ui_menu->page == GPIO_ACCEL_STREAM_PAGE) {
        lv_obj_t *sending_lbl = lv_label_create(right_panel);
        lv_obj_set_style_text_font(sending_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(sending_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_align(sending_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(sending_lbl, "Sending:");
    }

    // X / Y degree readout
    lv_obj_t *val_lbl = lv_label_create(right_panel);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(val_lbl, user_secondary_color, 0);
    lv_obj_set_style_text_align(val_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(val_lbl, "X: 0\xC2\xB0\nY: 0\xC2\xB0");

    *out_ball = ball;
    *out_mode_lbl = mode_lbl;
    *out_val_lbl = val_lbl;
}

// Update the X/Y readout from a reading and ease the indicator toward the tilt
// indicator_d is the indicator's diameter in px; travel is clamped to it so a larger indicator still stays inside the circle.
static void accel_apply_reading(const accel_deg_t *a, lv_obj_t *ball, float indicator_d, lv_obj_t *val_lbl, float *out_x, float *out_y)
{
    const float max_travel = (LEVEL_D / 2.0f) - (indicator_d / 2.0f) - 2.0f;

    float dx, dy; // Ball offset from centre (px)
    float read_x, read_y; // X/Y readout in deg, 0 at the current mode's centred pose
    if (accel_mode == MODE_FLAT) {
        dx = (-(a->roll) / MAX_TILT_DEG) * max_travel; // Horizontal inverted
        dy = (a->pitch / MAX_TILT_DEG) * max_travel;
        read_x = -a->roll;
        read_y = -a->pitch;
    } else {
        // Holding/Remote are used with the screen vertical, where atan2 pitch/roll gimbal-lock
        // Reconstruct the corrected gravity unit vector and recentre
        float p = a->pitch * DEG2RAD;
        float r = a->roll  * DEG2RAD;
        float gx = -sinf(p);          // Board +X gravity component
        float gy = cosf(p) * sinf(r); // Board +Y gravity component
        float gz = cosf(p) * cosf(r); // Board +Z gravity component
        const float scale = max_travel / sinf(MAX_TILT_DEG * DEG2RAD);

        if (accel_mode == MODE_HOLDING) {
            // Held upright, board +X points up (centred when gx~1, gy/gz~0)
            dx = -gy * scale; // Lean left/right
            dy = -gz * scale; // Tilt toward/away (screen normal)

            // Recompute text readings
            read_x = -asinf(gy) / DEG2RAD; // Deviation from upright, 0 when centred
            read_y =  asinf(gz) / DEG2RAD;
        } else { // MODE_REMOTE
            // Upright then rotated 90 deg right, board +Y points up (centred when gy~1)
            dx = -gz * scale; // Tilt toward/away (screen normal)
            dy = -gx * scale; // Lean up/down

            // Recompute text readings
            read_x = -asinf(gz) / DEG2RAD; // Deviation from remote pose, 0 when centred
            read_y =  asinf(gx) / DEG2RAD;
        }
    }

    // Keep the indicator inside the circle
    float mag = sqrtf(dx * dx + dy * dy);
    if (mag > max_travel) {
        dx *= max_travel / mag;
        dy *= max_travel / mag;
    }

    // Smoothly ease the indicator from its current offset to the new target
    accel_ball_ease(ball, (lv_anim_exec_xcb_t)lv_obj_set_x, lv_obj_get_style_x(ball, LV_PART_MAIN), (int32_t)dx);
    accel_ball_ease(ball, (lv_anim_exec_xcb_t)lv_obj_set_y, lv_obj_get_style_y(ball, LV_PART_MAIN), (int32_t)dy);

    // X and Y readout: \xC2\xB0 is the degree symbol in UTF-8
    char buf[48];
    snprintf(buf, sizeof(buf), "X: %+.0f\xC2\xB0\n" "Y: %+.0f\xC2\xB0", (double)read_x, (double)read_y);
    lv_label_set_text(val_lbl, buf);

    // Hand the displayed values back so the stream can send exactly what's shown
    if (out_x) *out_x = read_x;
    if (out_y) *out_y = read_y;
}

static void magcal_nvs_save(float x_min, float x_max, float y_min, float y_max)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(MAGCAL_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "magcal_nvs_save nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    // Write the four bounds as a blob and commit
    magcal_blob_t blob = { x_min, x_max, y_min, y_max };
    err = nvs_set_blob(h, MAGCAL_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    } else {
        ESP_LOGE(TAG, "magcal_nvs_save set_blob failed: %s", esp_err_to_name(err));
    }

    // Close NVS
    nvs_close(h);
}

// Fill the four bounds from NVS and report whether a valid calibration was loaded
static bool magcal_nvs_load(float *x_min, float *x_max, float *y_min, float *y_max)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(MAGCAL_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "magcal_nvs_load nvs_open failed: %s", esp_err_to_name(err));
#endif
        return false;
    }

    // Read the four bounds as a blob
    magcal_blob_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, MAGCAL_NVS_KEY, &blob, &sz);
    nvs_close(h); // Close NVS
    if (err != ESP_OK || sz != sizeof(blob)) {
        return false; // Not stored yet
    }

    // Reject corrupt/implausible data so a bad blob can't break the compass
    float xr = (blob.x_max - blob.x_min) * 0.5f;
    float yr = (blob.y_max - blob.y_min) * 0.5f;
    if (xr <= 0.0f || yr <= 0.0f || xr > 500.0f || yr > 500.0f) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "magcal_nvs_load rejected implausible blob: x_min=%.1f x_max=%.1f y_min=%.1f y_max=%.1f",
                (double)blob.x_min, (double)blob.x_max, (double)blob.y_min, (double)blob.y_max);
#endif
        return false;
    }

    *x_min = blob.x_min; *x_max = blob.x_max;
    *y_min = blob.y_min; *y_max = blob.y_max;
    return true;
}

void lcd_gpio_magcel_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define ACCEL_REFRESH_MS 25 // Ask gpio_task for fresh accel + mag samples at ~40 Hz

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *ball = NULL;
    static lv_obj_t *mode_lbl = NULL;
    static lv_obj_t *val_lbl = NULL;
    static TickType_t last_refresh = 0;
    static float arrow_heading = 0.0f; // Smoothed absolute heading
    static float heading_ref = 0.0f;   // Heading captured at entry ("straight ahead" = up)
    static bool heading_init = false;  // Has heading_ref been captured this visit?
    static float arrow_drawn = -1.0f;  // Last relative angle actually rendered (-1 = none yet)
    static float disp_x = 0.0f, disp_y = 0.0f; // Latest tilt readout, shown alongside Z = heading

    if (!init) {
        // Seed the calibration from NVS once per boot and trust a valid stored calibration
        if (!magcal_loaded) {
            magcal_loaded = true;
            if (magcal_nvs_load(&mag_cal_x_min, &mag_cal_x_max, &mag_cal_y_min, &mag_cal_y_max)) {
                cal_complete = true;
            }
        }

        // The compass can only read a heading once calibrated; on first boot there is none
        // Send the user straight to the calibration page rather than show a dead arrow
        if (!cal_complete) {
            ui_menu->page = GPIO_MAGCAL_PAGE;
            return;
        }

        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Show right arrow

        // Outer container
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Bubble view with the rotating direction arrow
        accel_build_bubble(ui_menu, cont, true, &ball, &mode_lbl, &val_lbl);

        // Drop any stale readings left in the queues from a previous visit
        xQueueReset(xAccelReadingsQueue);
        xQueueReset(xMagReadingsQueue);

        last_refresh = 0; // Force an immediate trigger on the first frame
        arrow_heading = 0.0f;
        heading_ref = 0.0f;
        heading_init = false; // Re-capture the "straight ahead" reference on entry
        arrow_drawn = -1.0f;
        disp_x = disp_y = 0.0f;
        init = true;
    }

    // Periodically ask gpio_task for a fresh accel (tilt) + mag (heading) sample
    if (xTaskGetTickCount() - last_refresh >= pdMS_TO_TICKS(ACCEL_REFRESH_MS)) {
        last_refresh = xTaskGetTickCount();
        xSemaphoreGive(xReadAccelSemaphore); // Req accel
        xSemaphoreGive(xReadMagSemaphore); // Req mag
    }

    // Update arrow + text whenever reading received
    accel_deg_t accel;
    if (xQueueReceive(xAccelReadingsQueue, &accel, 0) == pdTRUE) {
        // Writes the X/Y label and hands back the displayed tilt values for the Z line below
        accel_apply_reading(&accel, ball, ARROW_SIZE, val_lbl, &disp_x, &disp_y);
    }

    // Compass heading from the magnetometer using the stored hard-/soft-iron calibration
    mmc5603_reading_t mag;
    if (ball && xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE) {
        // Recover the calibration shape
        float x_half = (mag_cal_x_max - mag_cal_x_min) * 0.5f; // X radius from the stored calibration
        float y_half = (mag_cal_y_max - mag_cal_y_min) * 0.5f; // Y radius

        if (x_half > 0.0f && y_half > 0.0f) { // Always true once calibrated; guards a bad blob
#ifdef POLYCAST5_DEBUG_MAGNETO
            static TickType_t last_cal_log = 0;
            if (xTaskGetTickCount() - last_cal_log >= pdMS_TO_TICKS(500)) {
                last_cal_log = xTaskGetTickCount();
                ESP_LOGI(TAG, "MAGCAL centre=(%.1f, %.1f) radius=(%.1f, %.1f)",
                         (double)magcal_center_x(), (double)magcal_center_y(),
                         (double)x_half, (double)y_half);
            }
#endif
            // Hard-iron: subtract the centre. Soft-iron: normalise each axis to its half-span.
            float cx = (mag.x - magcal_center_x()) / x_half;
            float cy = (mag.y - magcal_center_y()) / y_half;

            // atan2 of the centred unit circle -> heading in degrees, +y convention
            float raw_heading = atan2f(cy, cx) / DEG2RAD; // rad -> deg
            if (raw_heading < 0.0f) raw_heading += 360.0f;

            if (!heading_init) {
                // First sample this visit: take it as "straight ahead" so the arrow starts up
                arrow_heading = raw_heading;
                heading_ref = raw_heading;
                heading_init = true;
            } else {
                // Low-pass over the shortest angular path (handles the 360->0 wrap)
                float d = raw_heading - arrow_heading;
                while (d > 180.0f) d -= 360.0f;
                while (d < -180.0f) d += 360.0f;

                // Eases a fraction toward it each frame
                arrow_heading += d * ARROW_SMOOTH;
                if (arrow_heading < 0.0f) arrow_heading += 360.0f;
                else if (arrow_heading >= 360.0f) arrow_heading -= 360.0f;
            }
        }

        // Show the turn relative to the entry orientation: 0 = straight ahead = arrow up
        float rel = arrow_heading - heading_ref;
        while (rel < 0.0f) rel += 360.0f;
        while (rel >= 360.0f) rel -= 360.0f;

        // Readout: X/Y are the tilt (from the accel), Z is the compass heading (arrow angle)
        // Overwrites the X/Y-only text accel_apply_reading wrote above
        char buf[64];
        snprintf(buf, sizeof(buf), "X: %+.0f\xC2\xB0\n" "Y: %+.0f\xC2\xB0\n" "Z: %.0f\xC2\xB0",
                (double)disp_x, (double)disp_y, (double)rel);
        lv_label_set_text(val_lbl, buf);

        // Actually spin the arrow on screen
        float dd = rel - arrow_drawn;
        while (dd > 180.0f) dd -= 360.0f;
        while (dd < -180.0f) dd += 360.0f;
        if (arrow_drawn < 0.0f || fabsf(dd) >= 1.0f) {
            lv_image_set_rotation(ball, (int32_t)lroundf(rel * 10.0f) % 3600);
            arrow_drawn = rel;
        }
    }

    /* User input */
    if (ui_btns->up_btn == 1) { // Next mode (wraps)
        accel_mode = (accel_mode + 1) % MODE_COUNT;
        lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));
    } else if (ui_btns->down_btn == 1) { // Open the compass calibration page
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = NULL;
        init = false;

        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Hide right
        ui_menu->page = GPIO_MAGCAL_PAGE;
    } else if (ui_btns->select_btn) { // Refresh
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = NULL;
        init = false;

        lv_timer_handler(); // Refresh screen
    } else if (ui_btns->left_btn) { // Go back
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = NULL;
        init = false;

        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Hide right

        // Back to GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->right_btn) { // Use accel with ESP-NOW
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = NULL;
        init = false;

        // Show tutorial QR to proceed
        prompt_accel_espnow_qr(ui_menu, gpio_menu);
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_gpio_magcal_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define MAGCAL_REFRESH_MS 25 // Pull fresh mag samples at ~40 Hz while calibrating
    #define MAGCAL_SECTORS_REQ 12 // Wedges (of 12) that count as a full turn

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;
    static lv_obj_t *status_lbl = NULL;
    static lv_obj_t *arrow_img = NULL;   // Live direction arrow (shown while calibrating)
    static lv_obj_t *prog_bar = NULL;    // Coverage progress bar (shown while calibrating)
    static bool calibrating = false;
    static uint16_t visited_sectors = 0; // Bitmask of the 12x30deg heading wedges seen this run
    static TickType_t last_refresh = 0;

    // Snapshot of the calibration taken when a run starts, restored if the user backs out
    static float bk_x_min, bk_x_max, bk_y_min, bk_y_max;
    static bool bk_cal_complete;

    if (!init) {
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Title
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "Compass Calibration");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instruction (centre); text changes once a run starts
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100));
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_align(instr_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(instr_lbl, "Lay flat away from metal, then turn a full circle.");
        lv_obj_align(instr_lbl, LV_ALIGN_CENTER, 0, 0);

        // Status / progress line at the bottom
        status_lbl = lv_label_create(cont);
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(status_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(status_lbl, "SELECT: start   LEFT: back");
        lv_obj_align(status_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

        // Live direction arrow (hidden until a run starts), built in the current accent colour
        arrow_rasterize(user_secondary_color);
        arrow_img = lv_image_create(cont);
        lv_image_set_src(arrow_img, &arrow_dsc);
        lv_obj_set_size(arrow_img, ARROW_SIZE, ARROW_SIZE);
        lv_image_set_pivot(arrow_img, ARROW_SIZE / 2, ARROW_SIZE / 2);
        lv_image_set_antialias(arrow_img, true);
        lv_obj_align(arrow_img, LV_ALIGN_CENTER, 0, -8);
        lv_obj_add_flag(arrow_img, LV_OBJ_FLAG_HIDDEN);

        // OTA-style coverage progress bar (hidden until a run starts)
        prog_bar = lv_bar_create(cont);
        lv_bar_set_range(prog_bar, 0, 100);
        lv_bar_set_value(prog_bar, 0, LV_ANIM_OFF);
        lv_obj_set_size(prog_bar, 150, 12);
        lv_obj_align(prog_bar, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_set_style_border_width(prog_bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(prog_bar, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(prog_bar, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(prog_bar, lv_color_darken(user_primary_color, 100), LV_PART_MAIN);
        lv_obj_set_style_bg_color(prog_bar, user_secondary_color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(prog_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(prog_bar, 6, LV_PART_MAIN | LV_PART_INDICATOR);
        lv_obj_add_flag(prog_bar, LV_OBJ_FLAG_HIDDEN);

        calibrating = false;
        last_refresh = 0;
        init = true;
    }

    // While a run is active, keep pulling samples, grow the bounds, and show coverage
    if (calibrating) {
        if (xTaskGetTickCount() - last_refresh >= pdMS_TO_TICKS(MAGCAL_REFRESH_MS)) {
            last_refresh = xTaskGetTickCount();
            xSemaphoreGive(xReadMagSemaphore);
        }

        // When a reading is received
        mmc5603_reading_t mag;
        if (xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE) {
            // Update calibration bounds
            if (mag.x < mag_cal_x_min) mag_cal_x_min = mag.x;
            if (mag.x > mag_cal_x_max) mag_cal_x_max = mag.x;
            if (mag.y < mag_cal_y_min) mag_cal_y_min = mag.y;
            if (mag.y > mag_cal_y_max) mag_cal_y_max = mag.y;

            // Live direction from the hard-iron-centred field
            // Only act when the vector is clearly off-centre

            // Field vector relative to the circle's center
            float dx = mag.x - magcal_center_x();
            float dy = mag.y - magcal_center_y();

            // Avoid dead zone near the center
            if (dx * dx + dy * dy > 9.0f) { // > ~3 uT from centre -> valid
                // Angle of that vector in degrees [0,360)
                float heading = atan2f(dy, dx) / DEG2RAD;
                if (heading < 0.0f) heading += 360.0f;
                lv_image_set_rotation(arrow_img, (int32_t)lroundf(heading * 10.0f) % 3600); // Rotate arrow img

                // Split the circle into 12 wedges of 30 degrees and tally which ones the user has covered
                int sector = (int)(heading / 30.0f);
                if (sector >= 0 && sector < 12) {
                    visited_sectors |= (uint16_t)(1u << sector);
                }
            }

            // Counts the set bits = how many distinct wedges have been hit (0–12)
            int covered = __builtin_popcount(visited_sectors);

            // Map and show percentage of wedges covered
            int pct = covered * 100 / MAGCAL_SECTORS_REQ;
            if (pct > 100) pct = 100;
            lv_bar_set_value(prog_bar, pct, LV_ANIM_ON);
            if (covered >= MAGCAL_SECTORS_REQ) {
                lv_label_set_text(status_lbl, "Ready - SELECT to finish");
            } else {
                char buf[40];
                snprintf(buf, sizeof(buf), "Keep turning... %d%%", pct);
                lv_label_set_text(status_lbl, buf);
            }
        }
    }

    /* User input */
    if (ui_btns->select_btn) {
        if (!calibrating) {
            // Start: snapshot the current calibration, then clear it and begin collecting
            bk_x_min = mag_cal_x_min; bk_x_max = mag_cal_x_max;
            bk_y_min = mag_cal_y_min; bk_y_max = mag_cal_y_max;
            bk_cal_complete = cal_complete;

            // Min max start values
            mag_cal_x_min = mag_cal_y_min = 1e9f; // Biggest possible float
            mag_cal_x_max = mag_cal_y_max = -1e9f; // Smallest possible float
            cal_complete = false;
            visited_sectors = 0;
            xQueueReset(xMagReadingsQueue);

            calibrating = true;
            last_refresh = 0;
            
            lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_HIDDEN); // Free the top for the arrow
            lv_obj_add_flag(instr_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(arrow_img, LV_OBJ_FLAG_HIDDEN); // Reveal the live arrow + bar
            lv_obj_remove_flag(prog_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(prog_bar, 0, LV_ANIM_OFF);
            lv_label_set_text(status_lbl, "Keep turning... 0%");
        } else {
            // Finish: only accept once enough wedges are covered (a real full turn)
            if (__builtin_popcount(visited_sectors) >= MAGCAL_SECTORS_REQ) {
                cal_complete = true;
                magcal_nvs_save(mag_cal_x_min, mag_cal_x_max, mag_cal_y_min, mag_cal_y_max); // Save

                // Clean up
                lv_obj_delete(cont);
                cont = NULL; title_lbl = instr_lbl = status_lbl = arrow_img = prog_bar = NULL;
                calibrating = false; init = false;
                ui_menu->page = GPIO_ACCEL_PAGE; // Back to the compass
            } else {
                lv_label_set_text(status_lbl, "Not enough - keep turning");
            }
        }
    } else if (ui_btns->left_btn) { // Cancel / back out
        if (calibrating) { // Restore the calibration the run replaced
            mag_cal_x_min = bk_x_min; mag_cal_x_max = bk_x_max;
            mag_cal_y_min = bk_y_min; mag_cal_y_max = bk_y_max;
            cal_complete = bk_cal_complete;
        }

        // Clean up
        lv_obj_delete(cont);
        cont = NULL; title_lbl = instr_lbl = status_lbl = arrow_img = prog_bar = NULL;
        calibrating = false; init = false;

        if (cal_complete) { // Have a calibration -> back to the compass
            ui_menu->page = GPIO_ACCEL_PAGE;

        // No calibration yet (e.g. first-boot forced cal) -> GPIO menu, not a dead compass
        } else {
            lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = GPIO_PAGE;
        }
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        if (calibrating) {
            mag_cal_x_min = bk_x_min; mag_cal_x_max = bk_x_max;
            mag_cal_y_min = bk_y_min; mag_cal_y_max = bk_y_max;
            cal_complete = bk_cal_complete;
        }

        // Clean up
        lv_obj_delete(cont);
        cont = NULL; title_lbl = instr_lbl = status_lbl = arrow_img = prog_bar = NULL;
        calibrating = false; init = false;
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_gpio_magcel_stream_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define STREAM_REFRESH_MS 25 // Trigger + transmit a sample at ~40 Hz

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *ball = NULL;
    static lv_obj_t *mode_lbl = NULL;
    static lv_obj_t *val_lbl = NULL;
    static TickType_t last_refresh = 0;

    if (!init) {
        int idx = espnow_menu.index;

        // Encryption is on for this peer if it has a non-zero LMK stored
        uint8_t zero_lmk[LMK_LEN] = {0};
        bool enc = memcmp(espnow_menu.lmk[idx], zero_lmk, LMK_LEN) != 0;

        // Ask the ESP-NOW task to open a streaming session to this peer
        espnow_accel_ctrl_t ctrl = {
            .start = true,
            .enc = enc
        };
        memcpy(ctrl.mac_selected, espnow_menu.rx_mac[idx], ESPNOW_MAC_SIZE);
        if (enc) {
            memcpy(ctrl.lmk, espnow_menu.lmk[idx], LMK_LEN);
        }

        lv_timer_handler(); // Let the UI update before starting the session

        // Bring the radio + peer up
        xQueueSend(xEspAccelStreamCtrlQueue, &ctrl, portMAX_DELAY);

        // Up/down change the view mode, left goes back; no right action here
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

        // Outer container
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Same bubble view as the accel page, but with the classic ball
        accel_build_bubble(ui_menu, cont, false, &ball, &mode_lbl, &val_lbl);

        // Drop any stale reading from a previous visit
        xQueueReset(xAccelReadingsQueue);

        last_refresh = 0; // Force an immediate trigger on the first frame
        init = true;
    }

    // Periodically request a fresh accel sample from gpio_task
    if (xTaskGetTickCount() - last_refresh >= pdMS_TO_TICKS(STREAM_REFRESH_MS)) {
        last_refresh = xTaskGetTickCount();
        xSemaphoreGive(xReadAccelSemaphore);
    }

    // Move the ball + update text, and forward each new sample to the ESP-NOW task
    accel_deg_t accel;
    if (xQueueReceive(xAccelReadingsQueue, &accel, 0) == pdTRUE) {
        float disp_x, disp_y;
        accel_apply_reading(&accel, ball, BALL_D, val_lbl, &disp_x, &disp_y);

        // Stream exactly what the LCD shows (mode-aware, recentred X/Y)
        espnow_accel_t sample = {
            .x = disp_x,
            .y = disp_y
        };
        xQueueOverwrite(xEspAccelStreamQueue, &sample); // Latest value wins
    }

    /* User input */
    if (ui_btns->up_btn == 1) { // Next view mode (wraps)
        accel_mode = (accel_mode + 1) % MODE_COUNT;
        lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));
    } else if (ui_btns->down_btn == 1) { // Previous view mode (wraps)
        accel_mode = (accel_mode + MODE_COUNT - 1) % MODE_COUNT;
        lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));
    } else if (ui_btns->left_btn) { // Stop streaming, back to GPIO menu
        espnow_accel_ctrl_t stop = {
            .start = false
        };
        xQueueSend(xEspAccelStreamCtrlQueue, &stop, portMAX_DELAY);

        lv_anim_delete(ball, NULL); // Stop ball anims before freeing the object
        lv_obj_delete(cont);
        cont = NULL;
        ball = mode_lbl = val_lbl = NULL;
        init = false;

        espnow_entry_mode = ESPNOW_ENTRY_NORMAL;

        // Go to GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        espnow_accel_ctrl_t stop = {
            .start = false
        };
        xQueueSend(xEspAccelStreamCtrlQueue, &stop, portMAX_DELAY);

        lv_anim_delete(ball, NULL); // Stop ball anims before freeing the object
        lv_obj_delete(cont);
        cont = NULL;
        ball = mode_lbl = val_lbl = NULL;
        init = false;

        espnow_entry_mode = ESPNOW_ENTRY_NORMAL;
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

// Helper function to perform the I2C scan and return found addresses
static void i2c_scan(uint8_t found_addrs[], int max, int *found_count)
{
    *found_count = 0;

    // Wait for then lock I2C bus
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take I2C mutex");
        return;
    }

    // For all standard I2C addresses
    for (uint8_t addr = 0x03; addr < 0x78; ++addr) {
        // Probe the address on the shared bus handle
        esp_err_t ret = i2c_master_probe(i2c_bus_handle, addr, 50);

        // If slave at addr ACKed the address byte
        if (ret == ESP_OK) {
            // Stop once the caller's buffer is full
            if (*found_count >= max) {
                ESP_LOGW(TAG, "I2C scan found more than %d devices, some addresses not reported", max);
                break;
            }
            found_addrs[*found_count] = addr;
            (*found_count)++;

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
#endif
        }
    }

    xSemaphoreGive(xI2CBusMutex); // Release I2C bus
}

void lcd_gpio_scanner_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define I2C_MAX_DEVICES 32 // Max I2C devices
    #define I2C_SCROLL_OFFSET 20 // Scroll per button press

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *status_lbl = NULL;
    static lv_obj_t *addrs_lbl = NULL;

    if (!init) {
        // Create a scrollable container for the instructions
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
        lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content

        // Title label
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "I2C Scanner");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Status label
        status_lbl = lv_label_create(cont);
        lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(status_lbl, lv_pct(100));
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(status_lbl, user_secondary_color, 0);
        lv_obj_align_to(status_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text(status_lbl, "Press SELECT to scan.\nNote: 0x20 and 0x19 are internal.");

        // Addresses label: hidden initially
        addrs_lbl = lv_label_create(cont);
        lv_label_set_long_mode(addrs_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(addrs_lbl, lv_pct(100));
        lv_obj_set_style_text_font(addrs_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(addrs_lbl, user_secondary_color, 0);
        lv_obj_align_to(addrs_lbl, status_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, -13);
        lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Hide until scan complete

        init = true;
    }

    /* User input */
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, I2C_SCROLL_OFFSET, LV_ANIM_ON); // Scroll up
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -I2C_SCROLL_OFFSET, LV_ANIM_ON); // Scroll down
    } else if (ui_btns->select_btn == 1) {
        lv_label_set_text(status_lbl, "Scanning...");
        lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN);
        
        lv_timer_handler(); // Force update
        
        // Perform scan
        uint8_t found_addrs[I2C_MAX_DEVICES];
        int found_count = 0;
        i2c_scan(found_addrs, I2C_MAX_DEVICES, &found_count);

        // Build concatenated string of devices found
        if (found_count > 0) { // If any
            lv_label_set_text(status_lbl, "Found devices:");
            lv_obj_remove_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Show

            // Build the comma-separated string
            POLYCAST5_USE_PSRAM_BSS static char i2c_addr_str[256] = {0}; // String buffer: 0-out
            memset(i2c_addr_str, 0, sizeof(i2c_addr_str));
            for (int i = 0; i < found_count; ++i) {
                char buf[5];
                snprintf(buf, sizeof(buf), "0x%02X", found_addrs[i]);
                
                // Append to i2c_addr_str
                strcat(i2c_addr_str, buf);
                
                // If not last, add comma
                if (i < found_count - 1) {
                    strcat(i2c_addr_str, ", ");
                }
            }
            
            // Update text
            lv_label_set_text(addrs_lbl, i2c_addr_str);
        } else { // Else no devices found
            lv_label_set_text(status_lbl, "No devices found.");
            lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Hide
        }

        lv_timer_handler(); // Force update
    } else if (ui_btns->left_btn == 1) { // Exit
        // Clean up
        lv_obj_delete(cont); // Deletes children
        cont = title_lbl = status_lbl = addrs_lbl = NULL;
        init = false;

        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        // Clean up
        lv_obj_delete(cont);
        cont = title_lbl = status_lbl = addrs_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu);  // True = home, false = sleep
    }
}

// Safe log append
static void term_log_append(char *dst, size_t cap, const char *line)
{
    // Exit early if bad
    if (!dst || !line || cap == 0) {
        return;
    }
    
    strlcat(dst, line, cap);
}

void lcd_gpio_terminal_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define TERMINAL_SLAVE_ADDR 0x2A // Assumed external slave address
    #define TERMINAL_MAX_CMD 255 // Max command value (uint8_t)
 
    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *log_lbl = NULL;
    static uint8_t current_cmd = 0; // Current command number (0-255)
    POLYCAST5_USE_PSRAM_BSS static char log_buffer[2048] = {0}; // Buffer for terminal log (append-only)
 
     // Do once
    if (!init) {
        // Create a scrollable container for the instructions
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
        lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content

        // Title label
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "I2C Terminal");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instructions label (scrollable if text is long)
        log_lbl = lv_label_create(cont);
        lv_label_set_long_mode(log_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(log_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(log_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(log_lbl, user_secondary_color, 0);
        lv_obj_align_to(log_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        
        lv_label_set_text(log_lbl, "Use up/down to adjust.\nPress select to send.");

        lv_timer_handler();
 
        init = true;
    }

    /* User input */
    if (ui_btns->up_btn == 1) {
        // Increment cmd with wrap
        current_cmd = (current_cmd < TERMINAL_MAX_CMD) ? current_cmd + 1 : 0;
        
        // Show updated
        char msg[64];
        snprintf(msg, sizeof(msg), "Command: %d\n", current_cmd);
        term_log_append(log_buffer, sizeof(log_buffer), msg);

        lv_label_set_text(log_lbl, log_buffer);
        lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        // Decrement cmd with wrap
        current_cmd = (current_cmd > 0) ? current_cmd - 1 : TERMINAL_MAX_CMD;
        
        // Show updated
        char msg[64];
        snprintf(msg, sizeof(msg), "Command: %d\n", current_cmd);
        term_log_append(log_buffer, sizeof(log_buffer), msg);
        
        lv_label_set_text(log_lbl, log_buffer);

        lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
    } else if (ui_btns->select_btn == 1) {
        #define TERMINAL_MAX_RESPONSE_LEN 126 // Match slave
        #define TERMINAL_FIXED_LEN (1 + TERMINAL_MAX_RESPONSE_LEN) // Fixed read size
    
        // Take mutex for I2C access
        if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            esp_err_t ret = ESP_OK;
            POLYCAST5_USE_PSRAM_BSS static char msg[256]; // Buffer for logging
            memset(msg, 0, sizeof(msg));

            // Add terminal slave device to bus
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = TERMINAL_SLAVE_ADDR,
                .scl_speed_hz = I2C_MASTER_FREQ_HZ,
            };
            i2c_master_dev_handle_t term_dev = NULL;
            ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &term_dev);
            if (ret != ESP_OK) {
                snprintf(msg, sizeof(msg), "Add device failed: %s\n\n", esp_err_to_name(ret));
                term_log_append(log_buffer, sizeof(log_buffer), msg);
                xSemaphoreGive(xI2CBusMutex);
                lv_label_set_text(log_lbl, log_buffer);
                lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
                return;
            }

            // WRITE transaction (send command)
            uint8_t cmd_byte = current_cmd;
            ret = i2c_master_transmit(term_dev, &cmd_byte, 1, 150);

            // Log confirmation
            snprintf(msg, sizeof(msg), "\nSent: %u (0x%02X) to 0x%X\n", current_cmd, current_cmd, TERMINAL_SLAVE_ADDR);
            term_log_append(log_buffer, sizeof(log_buffer), msg);

            if (ret != ESP_OK) {
                snprintf(msg, sizeof(msg), "Write failed: %s\n\n", esp_err_to_name(ret));
                term_log_append(log_buffer, sizeof(log_buffer), msg);

                // Early exit on write error
                i2c_master_bus_rm_device(term_dev);
                xSemaphoreGive(xI2CBusMutex); // Release I2C bus
                lv_label_set_text(log_lbl, log_buffer);
                lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
                return;
            }

            // Delay to allow slave processing time
            vTaskDelay(pdMS_TO_TICKS(50));

            uint8_t response_buf[TERMINAL_FIXED_LEN] = {0};

            // Single READ transaction (exactly TERMINAL_FIXED_LEN bytes)
            ret = i2c_master_receive(term_dev, response_buf, TERMINAL_FIXED_LEN, 150);

            // If read something
            if (ret == ESP_OK) {
                uint8_t response_len = response_buf[0]; // First byte is length (from receiver example)

                // len == 0 -> empty
                if (response_len == 0) {
                    snprintf(msg, sizeof(msg), "Empty response.\n\n");
                } else if (response_len > TERMINAL_MAX_RESPONSE_LEN) { // Response too big
                    snprintf(msg, sizeof(msg), "Invalid response length: %u\n\n", response_len);
                } else { // Else valid response
                    char response_str[TERMINAL_MAX_RESPONSE_LEN + 1] = {0};
                    memcpy(response_str, &response_buf[1], response_len); // Copy exactly len bytes
                    response_str[response_len] = '\0'; // NUL-terminate
                    snprintf(msg, sizeof(msg), "Response: %s\n\n", response_str);
                }
            } else {
                snprintf(msg, sizeof(msg), "Read failed: %s\n\n", esp_err_to_name(ret));
            }

            // Append to log
            term_log_append(log_buffer, sizeof(log_buffer), msg);

            // Remove terminal device from bus
            i2c_master_bus_rm_device(term_dev);
            xSemaphoreGive(xI2CBusMutex); // Release I2C bus

            // Show and scroll to bottom
            lv_label_set_text(log_lbl, log_buffer);
            lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
        } else {
            // Mutex timeout - append error
            term_log_append(log_buffer, sizeof(log_buffer), "\nI2C bus busy - timed out.\n\n");

            // Show and scroll to bottom
            lv_label_set_text(log_lbl, log_buffer);
            lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
        }
    } else if (ui_btns->left_btn == 1) { // Exit
        // Clean up
        lv_obj_delete(cont); // Deletes children
        cont = title_lbl = log_lbl = NULL;
        init = false;
        memset(log_buffer, 0, sizeof(log_buffer)); // Clear log for next entry
        current_cmd = 0;
 
        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
 
        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        // Clean up
        lv_obj_delete(cont); // Deletes children
        cont = title_lbl = log_lbl = NULL;
        init = false;
        memset(log_buffer, 0, sizeof(log_buffer)); // Clear log for next entry
        current_cmd = 0;
 
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}



