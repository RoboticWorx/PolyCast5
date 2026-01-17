#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

// System info
#include <inttypes.h>
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_idf_version.h"

#include "core/lv_obj_scroll.h"
#include "font/lv_symbol_def.h"
#include "core/lv_obj_pos.h"
#include "core/lv_obj.h"
#include "misc/lv_style.h"
#include "misc/lv_area.h"
#include "widgets/label/lv_label.h"

#include "gpio_task.h"
#include "lcd_utils.h"
#include "lcd_settings.h"
#include "wifi_ota_update.h"

#define TAG "LCD_SETTINGS"

// Color settings
#define SETTINGS_COLOR_NS "set_color" // NVS namespace
#define SETTINGS_COLOR_PRIM_KEY "primary" // Primary color value
#define SETTINGS_COLOR_SEC_KEY "secondary" // Secondary color value

// PIN settings
#define SETTINGS_PIN_NS "set_pin" // NVS namespace
#define SETTINGS_PIN_KEY "combo" // Stored PIN combination
#define SETTINGS_PIN_SET_KEY "set" // Whether PIN is enabled (0 or 1)

// Attempt count settings
#define SETTINGS_ATTEMPTS_NS "set_attempts" // NVS namespace
#define SETTINGS_ATTEMPTS_KEY "num_attempts" // Number of wrong entry attempts

// Haptics settings
#define SETTINGS_HAPTICS_NS "set_haptics" // NVS namespace
#define SETTINGS_HAPTIC_DUR_KEY "duration" // Vibration duration (ms)
#define SETTINGS_HAPTIC_STATES_KEY "states" // Which buttons to buzz on

// Sleep timer settings
#define SETTINGS_SLEEP_TIMER_NS "set_sleep" // NVS namespace
#define SETTINGS_SLEEP_TIMER_KEY "timer_len" // Sleep timer length

// RGB LED settings
#define SETTINGS_RGB_LED_NS "set_rgb" // NVS namespace
#define SETTINGS_RGB_LED_PERIOD_KEY "every_ms" // RGB LED blink period
#define SETTINGS_RGB_LED_TOTAL_KEY "for_ms" // RGB LED blink total time

// LCD LEDC settings
#define SETTINGS_LCD_LEDC_NS "set_ledc" // NVS namespace
#define SETTINGS_LCD_LEDC_KEY "brightness" // LCD brightness

// Uptime settings
#define SETTINGS_UPTIME_NS "set_uptime" // NVS namespace
#define SETTINGS_UPTIME_KEY "uptime" // Uptime (seconds)

#define COLOR_OPTION_COUNT 23

#define SLEEP_TIMER_MIN_S 5 // 5 sec
#define SLEEP_TIMER_MAX_S 120 // 2 min

settings_menu_t settings_menu = {
    .options = {"Check for Updates", SETTINGS_SET_LOCK_TXT, "Change Colors", "LCD Brightness", "Adjust Haptics",
            "Adjust Sleep Timer", "Adjust RGB LED", "Tips and Tricks", "System Info", "Reboot", "Factory Reset"},
    .size = 11,
    .index = 0,
    .cont = NULL,
    .pin_menu.pin_set = false,
};

// ota_update.c: Safe to use, no simultaneous calls
extern char ota_update_info[512];
extern char ota_update_url[512];

extern bool pin_signing_in;

extern volatile uint8_t haptic_len_ms;
extern volatile bool haptic_btns[6];

extern uint16_t home_sleep_after_s;

extern int16_t rbg_blink_period_ms;
extern int16_t rgb_blink_total_ms;

extern int8_t lcd_ledc_brightness;

static bool primary_color_selected = true;

static const lv_color_t primary_color_options[COLOR_OPTION_COUNT] = {
    // Neutrals
    LV_COLOR_MAKE(0x00, 0x00, 0x00), // Black
    LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), // White

    // Warm reds / oranges
    LV_COLOR_MAKE(0x8B, 0x00, 0x00), // Dark Red
    LV_COLOR_MAKE(0xFF, 0x00, 0x00), // Pure Red
    LV_COLOR_MAKE(0xFF, 0x57, 0x22), // Deep Orange
    LV_COLOR_MAKE(0xFF, 0x98, 0x00), // Orange 500
    LV_COLOR_MAKE(0xFF, 0xC1, 0x07), // Amber 500
    LV_COLOR_MAKE(0xFF, 0xD7, 0x00), // Gold
    LV_COLOR_MAKE(0x79, 0x55, 0x48), // Brown
    LV_COLOR_MAKE(0xFF, 0xEB, 0x3B), // Yellow 500
    LV_COLOR_MAKE(0xE9, 0x1E, 0x63), // Pink 500

    // Greens
    LV_COLOR_MAKE(0xCD, 0xDC, 0x39), // Lime 500
    LV_COLOR_MAKE(0x4C, 0xAF, 0x50), // Green 500
    LV_COLOR_MAKE(0x00, 0x8B, 0x00), // 8B Green
    LV_COLOR_MAKE(0x00, 0x64, 0x00), // Dark Green
    LV_COLOR_MAKE(0x00, 0x96, 0x88), // Teal 500

    // Blues and purples
    LV_COLOR_MAKE(0x03, 0xA9, 0xF4), // Light Blue 500
    LV_COLOR_MAKE(0x46, 0x82, 0xB4), // Steel Blue
    LV_COLOR_MAKE(0x00, 0x00, 0x8B), // Dark Blue
    LV_COLOR_MAKE(0x3F, 0x51, 0xB5), // Indigo 500
    LV_COLOR_MAKE(0x9C, 0x27, 0xB0), // Purple 500
    LV_COLOR_MAKE(0xA0, 0x20, 0xF0), // Pure Purple
    LV_COLOR_MAKE(0x30, 0x19, 0x34), // Dark Purple
};

static const lv_color_t secondary_color_options[COLOR_OPTION_COUNT] = {
    // Neutrals
    LV_COLOR_MAKE(0x00, 0x00, 0x00), // Black
    LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), // White

    // Warm reds / oranges
    LV_COLOR_MAKE(0x8B, 0x00, 0x00), // Dark Red
    LV_COLOR_MAKE(0xFF, 0x00, 0x00), // Pure Red
    LV_COLOR_MAKE(0xFF, 0x57, 0x22), // Deep Orange
    LV_COLOR_MAKE(0xFF, 0x98, 0x00), // Orange 500
    LV_COLOR_MAKE(0xFF, 0xC1, 0x07), // Amber 500
    LV_COLOR_MAKE(0xFF, 0xD7, 0x00), // Gold
    LV_COLOR_MAKE(0x79, 0x55, 0x48), // Brown
    LV_COLOR_MAKE(0xFF, 0xEB, 0x3B), // Yellow 500
    LV_COLOR_MAKE(0xE9, 0x1E, 0x63), // Pink 500

    // Greens
    LV_COLOR_MAKE(0xCD, 0xDC, 0x39), // Lime 500
    LV_COLOR_MAKE(0x4C, 0xAF, 0x50), // Green 500
    LV_COLOR_MAKE(0x00, 0x8B, 0x00), // 8B Green
    LV_COLOR_MAKE(0x00, 0x64, 0x00), // Dark Green
    LV_COLOR_MAKE(0x00, 0x96, 0x88), // Teal 500

    // Blues and purples
    LV_COLOR_MAKE(0x03, 0xA9, 0xF4), // Light Blue 500
    LV_COLOR_MAKE(0x46, 0x82, 0xB4), // Steel Blue
    LV_COLOR_MAKE(0x00, 0x00, 0x8B), // Dark Blue
    LV_COLOR_MAKE(0x3F, 0x51, 0xB5), // Indigo 500
    LV_COLOR_MAKE(0x9C, 0x27, 0xB0), // Purple 500
    LV_COLOR_MAKE(0xA0, 0x20, 0xF0), // Pure Purple
    LV_COLOR_MAKE(0x30, 0x19, 0x34), // Dark Purple
};

void lcd_settings_setup_page(settings_menu_t *menu)
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

void lcd_settings_update_menu(settings_menu_t *menu)
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

static void create_next_box(lv_obj_t *pin_container, lv_obj_t **pin_labels, int *num_boxes) {
    if (*num_boxes >= SETTINGS_MAX_PIN_LEN) {
        return;
    }
    
    // Create new box
    lv_obj_t *box = lv_obj_create(pin_container);
    lv_obj_set_size(box, 35, 35);
    lv_obj_set_style_bg_color(box, user_primary_color, 0);
    lv_obj_set_style_border_color(box, user_secondary_color, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    
    // Fill with label
    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(label, user_secondary_color, 0);
    lv_obj_center(label);
    
    pin_labels[*num_boxes] = label;
    (*num_boxes)++;
}
static const char *code_to_symbol(char c) {
    switch(c) {
        case 'U':
            return LV_SYMBOL_UP;
        case 'D':
            return LV_SYMBOL_DOWN;
        case 'L':
            return LV_SYMBOL_LEFT;
        case 'R':
            return LV_SYMBOL_RIGHT;
        default:
            return "";
    }
}

void lcd_settings_rebuild_pin_boxes(lv_obj_t *pin_container, lv_obj_t **pin_labels, char *unlock_pin, int *num_boxes, int num_filled)
{
    // Start fresh
    lv_obj_clean(pin_container);
    *num_boxes = 0;
    
    if (num_filled == 0) {
        // Nothing entered yet: one blank slot
        create_next_box(pin_container, pin_labels, num_boxes);
    } else {
        // Fill for each
        for (int i = 0; i < num_filled; ++i) {
            create_next_box(pin_container, pin_labels, num_boxes);
            
            // If signing in, hide input with asterisk
            if (pin_signing_in) {
                lv_label_set_text(pin_labels[i], "*");
                lv_obj_set_style_text_font(pin_labels[i], &lv_font_montserrat_30, 0);
                lv_obj_align(pin_labels[i], LV_ALIGN_CENTER, 0, 6);
            } else {
                lv_label_set_text(pin_labels[i], code_to_symbol(unlock_pin[i]));
                lv_obj_set_style_text_font(pin_labels[i], &lv_font_montserrat_18, 0);
                lv_obj_center(pin_labels[i]);
            }
        }
    }
}

void lcd_settings_setup_pin_page(settings_menu_t *menu)
{
    // Refresh prompt if pin_set
    if (menu->pin_menu.pin_set) {
        menu->pin_menu.prompt_pin = true;
    } else {
        menu->pin_menu.prompt_pin = false;
    }
    
    // Update text based on NVS load
    menu->options[0] = menu->pin_menu.pin_set ? SETTINGS_REMOVE_LOCK_TXT : SETTINGS_SET_LOCK_TXT;
    
    static lv_style_t container_style;
    static int zero = 0;
    
    // Create labels
    menu->pin_menu.lbl_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(menu->pin_menu.lbl_ins, "Enter PIN:", user_secondary_color, 
            &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 18);
            
    menu->pin_menu.lbl_attempts = lv_label_create(ACTIVE_SCR);
    lcd_format_label(menu->pin_menu.lbl_attempts, "", user_secondary_color, 
            &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -32);
            
    menu->pin_menu.lbl_back = lv_label_create(ACTIVE_SCR);
    lcd_format_label(menu->pin_menu.lbl_back, "Press home to go back", user_secondary_color, 
            &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -16);
    
    // Create pin container
    menu->pin_menu.pin_container = lv_obj_create(ACTIVE_SCR);
    lv_obj_set_size(menu->pin_menu.pin_container, LV_SIZE_CONTENT, 37);
    lv_obj_center(menu->pin_menu.pin_container);
    lv_obj_set_style_bg_color(menu->pin_menu.pin_container, user_primary_color, 0);
    lv_obj_set_style_border_width(menu->pin_menu.pin_container, 0, 0);
    lv_obj_set_flex_flow(menu->pin_menu.pin_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(menu->pin_menu.pin_container, 5, 0);
    lv_obj_set_scrollbar_mode(menu->pin_menu.pin_container, LV_SCROLLBAR_MODE_OFF);
    
    // Remove outside padding
    lv_style_init(&container_style);
    lv_style_set_pad_left(&container_style, 0);
    lv_style_set_pad_right(&container_style, 0);
    lv_style_set_pad_top(&container_style, 0);
    lv_style_set_pad_bottom(&container_style, 0);
    lv_obj_add_style(menu->pin_menu.pin_container, &container_style, 0);
    
    static lv_obj_t *unlock_labels[SETTINGS_MAX_PIN_LEN];
    lcd_settings_rebuild_pin_boxes(menu->pin_menu.pin_container, unlock_labels, menu->pin_menu.unlock_pin, &zero, 0);
    
    // Hide all for now
    lv_obj_add_flag(menu->pin_menu.pin_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(menu->pin_menu.lbl_ins, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(menu->pin_menu.lbl_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(menu->pin_menu.lbl_attempts, LV_OBJ_FLAG_HIDDEN);
}

static void confirm_entered_pin(ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    lv_obj_t *lbl_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ins, "Your PIN is:", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 18);
            
    lv_obj_t *lbl_ok = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ok, "OK", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);
            
    lv_obj_t *lbl_write = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_write, "Don't forget it!", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -18);
    
    // Create symbols from unlock_pin
    char symbol_buf[64];
    symbol_buf[0] = '\0'; // Empty start
    int offset = 0;
    
    // Append each arrow symbol
    for (int i = 0; i < strlen(settings_menu->pin_menu.unlock_pin); ++i) {
        const char *sym = code_to_symbol(settings_menu->pin_menu.unlock_pin[i]);
        
        int written = snprintf(symbol_buf + offset, sizeof(symbol_buf) - offset, "%s  ", sym);
        
        if (written < 0) {
            break;
        }
        
        offset += written;
    }
    
    // Display it
    lv_obj_t *lbl_pin = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_pin, symbol_buf, user_secondary_color,
            &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
   
    // Wait for user to confirm
    while (1) {
        lv_timer_handler();
        
        // OK
        if (xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {
            // Show arrows
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_ins);
            lv_obj_delete(lbl_pin);
            lv_obj_delete(lbl_ok);
            lv_obj_delete(lbl_write);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Exit
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lcd_settings_ota_confirm_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu) 
{    
    #define OTA_CONF_Y_OFFSET 40
    
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
        lv_label_set_text(title_lbl, "Update Available!");
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

        // Set instruction text
        const char *instr_text =
                "A new firmware update is available. "
                "Press RIGHT to start the update or LEFT to dismiss this message."
                "\n\nMore update info below:\n\n%s";
        
        // Combine into single string
        POLYCAST5_USE_PSRAM static char buf[1024];
        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf), instr_text, ota_update_info);
        
        lv_label_set_text(instr_lbl, buf);
    
        init = true;
    }
    
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, OTA_CONF_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -OTA_CONF_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->left_btn) { // Dismiss
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        // Show settings list
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        lv_timer_handler();

        // Disconnect Wi-Fi
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->right_btn == 1) { // Start the update selected
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        // Hide right and left arrows
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

        // Go to progress bar page
        ui_menu->page = SETTINGS_OTA_UPDATING_PAGE;
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

void lcd_settings_ota_updating_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu) 
{
    #define OTA_UPDATING_Y_OFFSET 40

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;

    static lv_obj_t *prog_row = NULL;
    static lv_obj_t *prog_bar = NULL;
    static lv_obj_t *pct_lbl  = NULL;

    static int ota_pct = 0;

    if (!init) {
        // Create a scrollable container for the instructions
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners
        lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding

        // Title
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "Updating...");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instructions
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100));
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        const char *instr_text =
                "Please do not turn off your device.\n\n\n"
                "If you get stuck at 0% for any reason, please reboot your device by pressing the HOME and RIGHT buttons at the same time then try again.";

        lv_label_set_text(instr_lbl, instr_text);

        // Progress row
        prog_row = lv_obj_create(cont);
        lv_obj_remove_style_all(prog_row);
        lv_obj_set_width(prog_row, lv_pct(100));
        lv_obj_set_style_pad_row(prog_row, 0, 0);
        lv_obj_set_style_pad_column(prog_row, 4, 0); // Space between bar and %
        lv_obj_set_style_pad_all(prog_row, 2, 0);
        lv_obj_set_flex_flow(prog_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(prog_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align_to(prog_row, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, -10); // Progress bar offset

        // Bar
        prog_bar = lv_bar_create(prog_row);
        lv_bar_set_range(prog_bar, 0, 100);
        lv_bar_set_value(prog_bar, 0, LV_ANIM_OFF);
        lv_obj_set_height(prog_bar, 12);
        lv_obj_set_width(prog_bar, lv_pct(100));
        lv_obj_set_flex_grow(prog_bar, 1); // Take remaining width
        // Bar style
        lv_obj_set_style_border_width(prog_bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(prog_bar, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(prog_bar, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(prog_bar, lv_color_darken(user_primary_color, 100), LV_PART_MAIN);
        lv_obj_set_style_bg_color(prog_bar, user_secondary_color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(prog_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(prog_bar, 6, LV_PART_MAIN | LV_PART_INDICATOR);

        // Percentage label
        pct_lbl = lv_label_create(prog_row);
        lv_label_set_text(pct_lbl, "0%");
        lv_obj_set_style_text_color(pct_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_font(pct_lbl, &lv_font_montserrat_14, 0);

        // Start the OTA update
        wifi_ota_update_start(ota_update_url);

        init = true;
    }

    // Update progress when OTA task posts a new value
    if (xQueueReceive(xWifiOtaPctQueue, &ota_pct, 0) == pdTRUE) {
        // Success
        if (ota_pct == -1) {
            lv_bar_set_value(prog_bar, 100, LV_ANIM_ON);
            lv_label_set_text(pct_lbl, "Done!");
        } else if (ota_pct == -2) { // Fail
            lv_bar_set_value(prog_bar, 0, LV_ANIM_ON);
            lv_label_set_text(pct_lbl, "Fail!");
        } else { // Normal percentage
            if (ota_pct < 0) {
                ota_pct = 0;
            } else if (ota_pct > 100) {
                ota_pct = 100;
            }
            
            // Smooth % fill
            lv_bar_set_value(prog_bar, ota_pct, LV_ANIM_ON);
            lv_label_set_text_fmt(pct_lbl, "%d%%", ota_pct);
        }
    }

    // Scrolling
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, OTA_UPDATING_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -OTA_UPDATING_Y_OFFSET, LV_ANIM_ON);
    }
}

void lcd_settings_pin_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    // Statics
    static bool do_once = false;
    static int num_filled = 0;
    static int num_boxes = 0;
    
    static lv_obj_t *pin_container;
    static lv_obj_t *lbl_ins;
    static lv_obj_t *lbl_conf;
    static lv_obj_t *pin_labels[SETTINGS_MAX_PIN_LEN];
    static lv_style_t container_style;
    
    // Only execute once
    if (!do_once) {
        // Clear any old PIN data
        memset(settings_menu->pin_menu.unlock_pin, 0, sizeof(settings_menu->pin_menu.unlock_pin));
        
        // Create labels
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "Create PIN with arrows:", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 18);
                     
        lbl_conf = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_conf, "  Home to back,\nSelect to confirm", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -13);
        
        // Create pin container
        pin_container = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(pin_container, LV_SIZE_CONTENT, 37);
        lv_obj_center(pin_container);
        lv_obj_set_style_bg_color(pin_container, user_primary_color, 0);
        lv_obj_set_style_border_width(pin_container, 0, 0);
        lv_obj_set_flex_flow(pin_container, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(pin_container, 5, 0);
        lv_obj_set_scrollbar_mode(pin_container, LV_SCROLLBAR_MODE_OFF);
        
        // Remove outside padding
        lv_style_init(&container_style);
        lv_style_set_pad_left(&container_style, 0);
        lv_style_set_pad_right(&container_style, 0);
        lv_style_set_pad_top(&container_style, 0);
        lv_style_set_pad_bottom(&container_style, 0);
        lv_obj_add_style(pin_container, &container_style, 0);
        
        lcd_settings_rebuild_pin_boxes(pin_container, pin_labels, settings_menu->pin_menu.unlock_pin, &num_boxes, num_filled);
        
        do_once = true;
    }
    
    // Pin input
    if ((ui_btns->up_btn == 1 || ui_btns->down_btn == 1 || ui_btns->left_btn == 1 || ui_btns->right_btn == 1) && (num_filled < SETTINGS_MAX_PIN_LEN)) {
        char code = '\0';
        
        // Assign code for unlock_pin
        if (ui_btns->up_btn) {
            code = 'U';
        } else if (ui_btns->down_btn) {
            code = 'D';
        } else if (ui_btns->left_btn) {
            code = 'L';
        } else if (ui_btns->right_btn) {
            code = 'R';
        }

        // Save and rebuild
        settings_menu->pin_menu.unlock_pin[num_filled++] = code;
        lcd_settings_rebuild_pin_boxes(pin_container, pin_labels, settings_menu->pin_menu.unlock_pin, &num_boxes, num_filled);
    } else if (ui_btns->select_btn == 1) { // Save
        settings_menu->pin_menu.unlock_pin[num_filled] = '\0'; // Ensure termination
            
        #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Entered pin: %s", settings_menu->pin_menu.unlock_pin);
        #endif
        
        // Update menu text and flag
        settings_menu->options[0] = SETTINGS_REMOVE_LOCK_TXT;
        lv_list_set_button_text(settings_menu->main_list, settings_menu->btns[0], settings_menu->options[0]);
        settings_menu->pin_menu.pin_set = true;
        
        // Save to NVS
        lcd_settings_pin_nvs_save(settings_menu);
        
        // Reset objects
        lv_obj_delete(pin_container); // Clears children
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_conf);
        lv_style_reset(&container_style);
        
        // Reset statics
        pin_container = NULL;
        num_filled = num_boxes = 0;
        lbl_ins = lbl_conf = NULL;
        do_once = false;
        
        // Hide arrows for confirmation
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
        
        confirm_entered_pin(ui_menu, settings_menu);
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show others
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            
        // Show settings list
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            
        // Switch pages
        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn == 1) { // Go back
        // Back a box
        if (num_filled > 0) {
            // Remove one and rebuild
            settings_menu->pin_menu.unlock_pin[num_filled--] = '\0';
            lcd_settings_rebuild_pin_boxes(pin_container, pin_labels, settings_menu->pin_menu.unlock_pin, &num_boxes, num_filled);
        } else { // First box: exit
            // Reset objects
            lv_obj_delete(pin_container); // Clears children
            lv_obj_delete(lbl_ins);
            lv_obj_delete(lbl_conf);
            lv_style_reset(&container_style);
            
            // Reset statics
            pin_container = NULL;
            num_filled = num_boxes = 0;
            lbl_ins = lbl_conf = NULL;
            do_once = false;
            
            // Hide right arrow
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Show settings list
            lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            
            // Switch pages
            ui_menu->page = SETTINGS_PAGE;
        }
    } else if (ui_btns->pwr_btn == 1) { // Power off
        // Reset objects
        lv_obj_delete(pin_container); // Clears children
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_conf);
        lv_style_reset(&container_style);
            
        // Reset statics
        pin_container = NULL;
        num_filled = num_boxes = 0;
        lbl_ins = lbl_conf = NULL;
        do_once = false;
        
        lcd_transition_back(false, ui_menu); // True = home, false = sleep
    }
}

void lcd_settings_colors_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    #define X_COL_POS 53
    
    // Statics
    static bool do_once = false;
    
    static lv_obj_t *lbl_ins;
    static lv_obj_t *lbl_primary;
    static lv_obj_t *lbl_secondary;
    static lv_obj_t *lbl_selected;
    static lv_obj_t *primary_color_box;
    static lv_obj_t *secondary_color_box;
    
    // Only execute once
    if (!do_once) {    
        primary_color_selected = true;
            
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "Select a color to change:", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

        // Border color for contrast             
        lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 100); // % darker 
                     
        // Create color boxes to show the change
        primary_color_box = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(primary_color_box,  100,  80);
        lv_obj_align(primary_color_box, LV_ALIGN_CENTER, -X_COL_POS,  10);
        lv_obj_set_style_bg_color(primary_color_box, user_primary_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(primary_color_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(primary_color_box, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(primary_color_box, darker_user_primary_color, LV_PART_MAIN);
        
        secondary_color_box = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(secondary_color_box,  100,  80);
        lv_obj_align(secondary_color_box, LV_ALIGN_CENTER, X_COL_POS,  10);
        lv_obj_set_style_bg_color(secondary_color_box, user_secondary_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(secondary_color_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(secondary_color_box, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(secondary_color_box, darker_user_primary_color, LV_PART_MAIN);
        
        // Text labels
        lbl_primary = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_primary, "Primary", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_TOP_MID, -X_COL_POS, 43);
                     
        lbl_secondary = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_secondary, "Secondary", user_primary_color,
                &lv_font_montserrat_16, LV_ALIGN_TOP_MID, X_COL_POS, 43);
                     
        lbl_selected = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_selected, LV_SYMBOL_CLOSE, user_secondary_color,
                &lv_font_montserrat_30, LV_ALIGN_CENTER, -X_COL_POS, 18);
        
        do_once = true;
    }
    
    // Select a color
    if (ui_btns->select_btn == 1) {
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_primary);
        lv_obj_delete(lbl_secondary);
        lv_obj_delete(lbl_selected);
        lv_obj_delete(primary_color_box);
        lv_obj_delete(secondary_color_box);
        
        // Reset statics
        lbl_ins = primary_color_box = secondary_color_box = lbl_primary = lbl_secondary = lbl_selected = NULL;
        do_once = false;
        
        // Show top and bottom arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Hide right
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = SETTINGS_COLORS_SEL_PAGE;
    } else if (ui_btns->right_btn == 1) { // Switch selected right    
        // Move selected from primary to secondary
        if (primary_color_selected) {
            lv_obj_set_x(lbl_selected, X_COL_POS);
            lv_obj_set_style_text_color(lbl_selected, user_primary_color, 0);
        } else { // Secondary to primary
            lv_obj_set_x(lbl_selected, -X_COL_POS);
            lv_obj_set_style_text_color(lbl_selected, user_secondary_color, 0);
        }
        primary_color_selected = !primary_color_selected;
    } else if (ui_btns->left_btn == 1 && !primary_color_selected) { // Switch selected left    
        lv_obj_set_x(lbl_selected, -X_COL_POS);
        lv_obj_set_style_text_color(lbl_selected, user_secondary_color, 0);
        primary_color_selected = !primary_color_selected;
    } else if (ui_btns->left_btn == 1) { // Back selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_primary);
        lv_obj_delete(lbl_secondary);
        lv_obj_delete(lbl_selected);
        lv_obj_delete(primary_color_box);
        lv_obj_delete(secondary_color_box);
        
        // Reset statics
        lbl_ins = primary_color_box = secondary_color_box = lbl_primary = lbl_secondary = lbl_selected = NULL;
        do_once = false;
        
        // Show settings list
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Show top and bottom arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Hide right
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_primary);
        lv_obj_delete(lbl_secondary);
        lv_obj_delete(lbl_selected);
        lv_obj_delete(primary_color_box);
        lv_obj_delete(secondary_color_box);
        
        // Reset statics
        lbl_ins = primary_color_box = secondary_color_box = lbl_primary = lbl_secondary = lbl_selected = NULL;
        do_once = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_settings_colors_sel_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    #define X_SEL_POS 62
    
    // Statics
    static bool do_once = false;
    static uint8_t new_color_idx = 0;
    
    static lv_obj_t *lbl_ins;
    static lv_obj_t *lbl_arr;
    static lv_obj_t *old_color_box;
    static lv_obj_t *new_color_box;
    
    // Only execute once
    if (!do_once) {
        new_color_idx = 0;
                
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "Use up/down to adjust.", user_secondary_color,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 18);
                     
        lbl_arr = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_arr, LV_SYMBOL_MINUS LV_SYMBOL_RIGHT, user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 10);
        
        // Border color for contrast             
        lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 100); // % darker 
        
        // Create color boxes to show the change
        old_color_box = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(old_color_box,  60,  60);
        lv_obj_align(old_color_box, LV_ALIGN_CENTER, -X_SEL_POS,  10);
        lv_obj_set_style_bg_opa(old_color_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(old_color_box, 3, LV_PART_MAIN);
        
        // Set 'old' box to what was selected
        if (primary_color_selected) {
            lv_obj_set_style_bg_color(old_color_box, user_primary_color, LV_PART_MAIN);
            lv_obj_set_style_border_color(old_color_box, darker_user_primary_color, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(old_color_box, user_secondary_color, LV_PART_MAIN);
            lv_obj_set_style_border_color(old_color_box, darker_user_primary_color, LV_PART_MAIN);
        }

        // New color
        new_color_box = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(new_color_box,  60,  60);
        lv_obj_align(new_color_box, LV_ALIGN_CENTER, X_SEL_POS,  10);
        lv_obj_set_style_bg_color(new_color_box, user_secondary_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(new_color_box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(new_color_box, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(new_color_box, user_secondary_color, LV_PART_MAIN);
        
        const lv_color_t *opts = primary_color_selected ? primary_color_options : secondary_color_options;
        lv_color_t c = opts[new_color_idx];
        
        // Skip if forbidden color (current or secondary)
        while(lv_color_eq(c, user_primary_color) || lv_color_eq(c, user_secondary_color)) {
            new_color_idx = (new_color_idx + 1) % COLOR_OPTION_COUNT;
            c = opts[new_color_idx];
        }
        lv_obj_set_style_bg_color(new_color_box, c, LV_PART_MAIN);
        lv_obj_set_style_border_color(new_color_box, c, LV_PART_MAIN);

        do_once = true;
    }
    
    // Increment new color up
    if (ui_btns->up_btn == 1) {
        // Pick which palette to use
        const lv_color_t *opts = primary_color_selected ? primary_color_options : secondary_color_options;
                                
        // Increment with wrap
        new_color_idx = (new_color_idx + 1) % COLOR_OPTION_COUNT;
        
        // Assign to index of selected
        lv_color_t c = opts[new_color_idx];
        
        // Skip if forbidden color (current or secondary)
        while(lv_color_eq(c, user_primary_color) || lv_color_eq(c, user_secondary_color)) {
            new_color_idx = (new_color_idx + 1) % COLOR_OPTION_COUNT;
            c = opts[new_color_idx];
        }
        
        // Show
        lv_obj_set_style_bg_color(new_color_box, c, LV_PART_MAIN);
        lv_obj_set_style_border_color(new_color_box, c, LV_PART_MAIN);
    } else if (ui_btns->down_btn == 1) { // Decrement new color down
        // Pick which palette to use
        const lv_color_t *opts = primary_color_selected ? primary_color_options : secondary_color_options;
                                
        // Decrement with wrap
        new_color_idx = (new_color_idx + COLOR_OPTION_COUNT - 1) % COLOR_OPTION_COUNT;
        
        // Assign to index of selected
        lv_color_t c = opts[new_color_idx];
        
        // Skip if forbidden color (current or secondary)
        while(lv_color_eq(c, user_primary_color) || lv_color_eq(c, user_secondary_color)) {
            new_color_idx = (new_color_idx + COLOR_OPTION_COUNT - 1) % COLOR_OPTION_COUNT;
            c = opts[new_color_idx];
        }
        
        // Show
        lv_obj_set_style_bg_color(new_color_box, c, LV_PART_MAIN);
        lv_obj_set_style_border_color(new_color_box, c, LV_PART_MAIN);
    } else if (ui_btns->select_btn == 1) { // Confirm new color
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_arr);
        lv_obj_delete(old_color_box);
        lv_obj_delete(new_color_box);
        
        // Hide arrows
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Confirmation text
        lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_rst, "Reloading with\nnew color...", user_secondary_color,
                 &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
        lv_timer_handler();
        
        // Set and save color
        lv_color_t c = primary_color_selected ? primary_color_options[new_color_idx] : secondary_color_options[new_color_idx];
        if (primary_color_selected) {
            user_primary_color = c;
        } else {
            user_secondary_color = c;
        }
        
        // Save to NVS to load at boot
        lcd_settings_color_nvs_save(new_color_idx, primary_color_selected);
        
        // Let user see
        vTaskDelay(pdMS_TO_TICKS(1500));
        
        // Reboot
        esp_restart();
    } else if (ui_btns->left_btn == 1) { // Back selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_arr);
        lv_obj_delete(old_color_box);
        lv_obj_delete(new_color_box);
        
        // Reset statics
        lbl_ins = lbl_arr = old_color_box = new_color_box = NULL;
        do_once = false;
        
        // Show settings list
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Show arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_arr);
        lv_obj_delete(old_color_box);
        lv_obj_delete(new_color_box);
        
        // Reset statics
        lbl_ins = lbl_arr = old_color_box = new_color_box = NULL;
        do_once = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_settings_adjust_haptics_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    #define ADJ_HAPTIC_Y_OFFSET 38
    
    static bool init = false;
    static int selected = 0;
    
    static lv_obj_t *cont, *slider, *lbl_spin, *pointer, *sw_row[6], *sw_arr[6];
    
    static lv_style_t row_style;
    
    const char *btn_names[6] = {
        "Buzz on Select ", "Buzz on Home ", "Buzz on Up       ",
        "Buzz on Down ", "Buzz on Left     ", "Buzz on Right  "
    };

    if (!init) {
        xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics
        lcd_settings_haptics_nvs_load(); // Reload haptics
        xSemaphoreGive(xHapticsMutex); // Release haptics
        
        // Create parent container
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cont, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_left(cont, 20, LV_PART_MAIN | LV_STATE_DEFAULT); // Space for pointer
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
                
        // Labels
        lbl_spin = lv_label_create(cont);
        lcd_format_label(lbl_spin, "", user_secondary_color,
                    &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 0);
        xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics
        lv_label_set_text_fmt(lbl_spin, "Buzz for %" PRIu8 " ms", haptic_len_ms);
        
        // Slider
        slider = lv_slider_create(cont);
        lv_obj_set_size(slider, 100, 5);
        lv_obj_align(slider, LV_ALIGN_TOP_MID, 30, 0);
        lv_slider_set_range(slider, HAPTIC_MIN_MS, HAPTIC_MAX_MS);
        lv_slider_set_value(slider, haptic_len_ms, LV_ANIM_OFF);
        xSemaphoreGive(xHapticsMutex); // Release haptics
        
        // Six switch rows
        for (int i = 0; i < 6; ++i) {
            // Create a row container for each row
            sw_row[i] = lv_obj_create(cont);
            lv_obj_set_size(sw_row[i], 180, 30);
            lv_obj_set_style_bg_color(sw_row[i], user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(sw_row[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_scrollbar_mode(sw_row[i], LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_style_border_width(sw_row[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_style_init(&row_style);
            lv_style_set_margin_top(&row_style, 2);
            lv_style_set_margin_bottom(&row_style, 2);
            lv_obj_add_style(sw_row[i], &row_style, 0);
            
            // Flex formatting
            lv_obj_set_flex_flow(sw_row[i],  LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(sw_row[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            
            // Add the label into the row
            lv_obj_t *lbl = lv_label_create(sw_row[i]);
            lcd_format_label(lbl, btn_names[i], user_secondary_color,
                    &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 0);
            
            // Add the switch
            sw_arr[i] = lv_switch_create(sw_row[i]);
            lv_obj_set_size(sw_arr[i], 30, 20);
            xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics
            if (haptic_btns[i]) {
                lv_obj_add_state(sw_arr[i], LV_STATE_CHECKED);
            }
            xSemaphoreGive(xHapticsMutex); // Release haptics
            
            lv_obj_set_style_margin_right(lbl, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        }

        // Pointer on screen
        pointer = lv_label_create(ACTIVE_SCR);
        lcd_format_label(pointer, LV_SYMBOL_PLAY, user_secondary_color,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, 23, 28);

        init = true;
    }

    // Scroll down
    if (ui_btns->down_btn == 1) {
        // Decrement with wrap
        selected = (selected + 1) % 7;
        
        lv_obj_scroll_by(cont, 0, -ADJ_HAPTIC_Y_OFFSET, LV_ANIM_ON); 
    } else if (ui_btns->up_btn == 1) { // Scroll up
        // Increment with wrap
        selected = (selected + 6) % 7;
        
        lv_obj_scroll_by(cont, 0, ADJ_HAPTIC_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->select_btn == 1) { // Toggle/iterate
        xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics        
        // Iterate slider
        if (selected == 0) {
            // Increment slider with wrap
            haptic_len_ms = (haptic_len_ms + 1) % (HAPTIC_MAX_MS + 1);
            if (haptic_len_ms < HAPTIC_MIN_MS) {
                haptic_len_ms = HAPTIC_MIN_MS;
            }
            
            lv_slider_set_value(slider, haptic_len_ms, LV_ANIM_OFF);
            
            // Update label
            lv_label_set_text_fmt(lbl_spin, "Buzz for %" PRIu8 " ms", haptic_len_ms);
        } else { // Toggle selected switch
            int idx = selected - 1;
            lv_obj_t *sw = sw_arr[idx];
            
            // Toggle
            if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
                lv_obj_clear_state(sw, LV_STATE_CHECKED);
                haptic_btns[idx] = false;
            } else {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
                haptic_btns[idx] = true;
            }
        }
        
        // Persist to NVS
        lcd_settings_haptics_nvs_save();
        xSemaphoreGive(xHapticsMutex); // Release haptics
    } else if (ui_btns->left_btn == 1) { // Back
        // Delete objects
        lv_obj_delete(cont); // Deletes all children
        lv_obj_delete(pointer);
        lv_style_reset(&row_style);
        
        // Reset statics
        cont = slider = lbl_spin = pointer = NULL;
        init = false;
        selected = 0;
        for (int i = 0; i < 6; ++i) {
            sw_arr[i] = sw_row[i] = NULL;
        }

        // Show settings
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        // Delete objects
        lv_obj_delete(cont); // Deletes all children
        lv_obj_delete(pointer);
        lv_style_reset(&row_style);
        
        // Reset statics
        cont = slider = lbl_spin = pointer = NULL;
        init = false;
        selected = 0;
        for (int i = 0; i < 6; ++i) {
            sw_arr[i] = sw_row[i] = NULL;
        }

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu);
    }
}

void lcd_settings_sleep_timer_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    #define SLEEP_TIMER_TXT "When home,\nsleep after\n%us / %u.%02um"
    
    // Statics
    static bool init = false;
    
    static lv_obj_t *lbl_ins;
    static lv_obj_t *slider;
    
    // Only execute once
    if (!init) {
        uint32_t mins = home_sleep_after_s / 60;
        uint32_t frac = (home_sleep_after_s % 60) * 100 / 60;
        
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_CENTER, -25, 0);
        lv_label_set_text_fmt(lbl_ins, SLEEP_TIMER_TXT, (unsigned)home_sleep_after_s, (unsigned)mins, (unsigned)frac);

        // Slider
        slider = lv_slider_create(ACTIVE_SCR);
        lv_obj_set_size(slider, 10, 100);
        lv_obj_align(slider, LV_ALIGN_CENTER, 55, 0);
        lv_slider_set_range(slider, SLEEP_TIMER_MIN_S, SLEEP_TIMER_MAX_S);
        lv_slider_set_value(slider, home_sleep_after_s, LV_ANIM_OFF);
        
        init = true;
    }
    
    // Increase sleep timer
    if (ui_btns->up_btn == 1) {
        home_sleep_after_s += 5;
        
        // Wrap
        if (home_sleep_after_s > SLEEP_TIMER_MAX_S) {
            home_sleep_after_s = SLEEP_TIMER_MIN_S;
        }
        
        // Create text
        uint32_t mins = home_sleep_after_s / 60;
        uint32_t frac = (home_sleep_after_s % 60) * 100 / 60;
        lv_label_set_text_fmt(lbl_ins, SLEEP_TIMER_TXT, (unsigned)home_sleep_after_s, (unsigned)mins, (unsigned)frac);
        lv_slider_set_value(slider, home_sleep_after_s, LV_ANIM_OFF);
        
        // Persist to NVS
        lcd_settings_sleep_timer_nvs_save();
    } else if (ui_btns->down_btn == 1) { // Decrease sleep timer
        home_sleep_after_s -= 5;
        
        // Wrap
        if (home_sleep_after_s < SLEEP_TIMER_MIN_S) {
            home_sleep_after_s = SLEEP_TIMER_MAX_S;
        }
        
        // Create text
        uint32_t mins = home_sleep_after_s / 60;
        uint32_t frac = (home_sleep_after_s % 60) * 100 / 60;
        lv_label_set_text_fmt(lbl_ins, SLEEP_TIMER_TXT, (unsigned)home_sleep_after_s, (unsigned)mins, (unsigned)frac);
        lv_slider_set_value(slider, home_sleep_after_s, LV_ANIM_OFF);
        
        // Persist to NVS
        lcd_settings_sleep_timer_nvs_save();
    } else if (ui_btns->left_btn == 1) { // Back selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(slider);
        
        // Reset statics
        lbl_ins = NULL;
        slider = NULL;
        init = false;
        
        // Show settings list
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(slider);
        
        // Reset statics
        lbl_ins = NULL;
        slider = NULL;
        init = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_settings_adjust_rgb_led_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{    
    #define RGB_BLINK_EVERY_TXT "Blink\nevery\n%d ms"
    #define RGB_BLINK_FOR_TXT "Blink\nfor\n%d ms"
    
    #define RGB_POINTER_OFFSET 15
    
    // Statics
    static bool init = false;
    static bool every_selected = true;
    
    static lv_obj_t *lbl_every;
    static lv_obj_t *lbl_for;
    static lv_obj_t *slider_every;
    static lv_obj_t *slider_for;
    static lv_obj_t *pointer;
    
    // Only execute once
    if (!init) {
        xSemaphoreTake(xRgbLedMutex, portMAX_DELAY); // Lock RGB LED
        // Label
        lbl_every = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_every, "", user_secondary_color,
                     &lv_font_montserrat_18, LV_ALIGN_LEFT_MID, 20, 0);
        // Half the time is off: blink every would be double
        lv_label_set_text_fmt(lbl_every, RGB_BLINK_EVERY_TXT, (unsigned)(rbg_blink_period_ms * 2));

        // Slider
        slider_every = lv_slider_create(ACTIVE_SCR);
        lv_obj_set_size(slider_every, 10, 100);
        lv_obj_align(slider_every, LV_ALIGN_LEFT_MID, 98, 0);
        lv_slider_set_range(slider_every, RGB_PERIOD_MIN_MS, RGB_PERIOD_MAX_MS);
        lv_slider_set_value(slider_every, rbg_blink_period_ms, LV_ANIM_OFF);
        
        // Label
        lbl_for = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_for, "", user_secondary_color,
                     &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_label_set_text_fmt(lbl_for, RGB_BLINK_FOR_TXT, (unsigned)rgb_blink_total_ms);
        
        // Slider
        slider_for = lv_slider_create(ACTIVE_SCR);
        lv_obj_set_size(slider_for, 10, 100);
        lv_obj_align(slider_for, LV_ALIGN_RIGHT_MID, -98, 0);
        lv_slider_set_range(slider_for, RGB_TOTAL_MIN_MS, RGB_TOTAL_MAX_MS);
        lv_slider_set_value(slider_for, rgb_blink_total_ms, LV_ANIM_OFF);
        
        // Pointer
        pointer = lv_label_create(ACTIVE_SCR);
        lcd_format_label(pointer, LV_SYMBOL_EJECT, user_secondary_color,
                     &lv_font_montserrat_20, LV_ALIGN_LEFT_MID, 20 + RGB_POINTER_OFFSET, 50);
        xSemaphoreGive(xRgbLedMutex); // Release RGB LED
        
        init = true;
    }
    
    // Increase sleep timer
    if (ui_btns->up_btn == 1) {
        xSemaphoreTake(xRgbLedMutex, portMAX_DELAY); // Lock RGB LED
        // On first option
        if (every_selected) {
            rbg_blink_period_ms += 5;
        
            // Wrap
            if (rbg_blink_period_ms > RGB_PERIOD_MAX_MS) {
                rbg_blink_period_ms = RGB_PERIOD_MIN_MS;
            } else if (rbg_blink_period_ms > 0 && rbg_blink_period_ms < 10) { // Skip 5
                rbg_blink_period_ms = 10;
            }
            
            // Update text
            // Half the time is off: blink every would be double
            lv_label_set_text_fmt(lbl_every, RGB_BLINK_EVERY_TXT, (unsigned)(rbg_blink_period_ms * 2));
            lv_slider_set_value(slider_every, rbg_blink_period_ms, LV_ANIM_OFF);
        } else { // Second option (for duration)
            rgb_blink_total_ms += 10;
        
            // Wrap
            if (rgb_blink_total_ms > RGB_TOTAL_MAX_MS) {
                rgb_blink_total_ms = RGB_TOTAL_MIN_MS;
            }
            
            // Update text
            lv_label_set_text_fmt(lbl_for, RGB_BLINK_FOR_TXT, (unsigned)rgb_blink_total_ms);
            lv_slider_set_value(slider_for, rgb_blink_total_ms, LV_ANIM_OFF);
        }
        
        // Persist to NVS
        lcd_settings_rgb_led_nvs_save();
        xSemaphoreGive(xRgbLedMutex); // Release RGB LED
    } else if (ui_btns->down_btn == 1) { // Decrease sleep timer        
        xSemaphoreTake(xRgbLedMutex, portMAX_DELAY); // Lock RGB LED
        // On first option
        if (every_selected) {
            rbg_blink_period_ms -= 5;
            
            // Wrap
            if (rbg_blink_period_ms < RGB_PERIOD_MIN_MS) {
                rbg_blink_period_ms = RGB_PERIOD_MAX_MS;
            } else if (rbg_blink_period_ms > 0 && rbg_blink_period_ms < 10) { // Skip 5
                rbg_blink_period_ms = 0;
            }
            
            // Update text
            // Half the time is off: blink every would be double
            lv_label_set_text_fmt(lbl_every, RGB_BLINK_EVERY_TXT, (unsigned)(rbg_blink_period_ms * 2));
            lv_slider_set_value(slider_every, rbg_blink_period_ms, LV_ANIM_OFF);
        } else { // Second option (for duration)
            rgb_blink_total_ms -= 10;
        
            // Wrap
            if (rgb_blink_total_ms  < RGB_TOTAL_MIN_MS) {
                rgb_blink_total_ms  = RGB_TOTAL_MAX_MS;
            }
            
            // Update text
            lv_label_set_text_fmt(lbl_for, RGB_BLINK_FOR_TXT, (unsigned)rgb_blink_total_ms);
            lv_slider_set_value(slider_for, rgb_blink_total_ms, LV_ANIM_OFF);
        }
        
        // Persist to NVS
        lcd_settings_rgb_led_nvs_save();
        xSemaphoreGive(xRgbLedMutex); // Release RGB LED
    } else if (ui_btns->right_btn == 1) { // Move selector
        every_selected = !every_selected;
        
        if (every_selected) {
            lv_obj_set_x(pointer, lv_obj_get_x(lbl_every) + RGB_POINTER_OFFSET);
        } else {
            lv_obj_set_x(pointer, lv_obj_get_x(lbl_for) + RGB_POINTER_OFFSET);
        }
    } else if (ui_btns->select_btn == 1) { // Test setting
        // RGB indicator
        uint8_t rgb_state = RGB_BLINK_GREEN;
        xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
    } else if (ui_btns->left_btn == 1) { // Back selected
        // On left-most
        if (every_selected) {
            // Delete objects
            lv_obj_delete(pointer);
            lv_obj_delete(lbl_every);
            lv_obj_delete(lbl_for);
            lv_obj_delete(slider_every);
            lv_obj_delete(slider_for);
            
            // Reset statics
            lbl_every = lbl_for = pointer = NULL;
            slider_every = slider_for = NULL;
            init = false;
            
            // Hide right arrow
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Show settings list
            lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            
            // Switch pages
            ui_menu->page = SETTINGS_PAGE;
        } else { // Select lbl_every
            every_selected = true;
            lv_obj_set_x(pointer, lv_obj_get_x(lbl_every) + RGB_POINTER_OFFSET);
        }
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Delete objects
        lv_obj_delete(pointer);
        lv_obj_delete(lbl_every);
        lv_obj_delete(lbl_for);
        lv_obj_delete(slider_every);
        lv_obj_delete(slider_for);
        
        // Reset statics
        lbl_every = lbl_for = pointer = NULL;
        slider_every = slider_for = NULL;
        init = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

static const char *system_chip_model_to_str(esp_chip_model_t m)
{
    #define SYSTEM_MODEL_WARNING "\nWARNING: You may have a knock-off! This should be ESP32-C5."
    
    switch (m) {
        case CHIP_ESP32:
            return "ESP32" SYSTEM_MODEL_WARNING;
        case CHIP_ESP32S2:
            return "ESP32-S2" SYSTEM_MODEL_WARNING;
        case CHIP_ESP32S3:
            return "ESP32-S3" SYSTEM_MODEL_WARNING;
        case CHIP_ESP32C3:
            return "ESP32-C3" SYSTEM_MODEL_WARNING;
        case CHIP_ESP32C2:
            return "ESP32-C2" SYSTEM_MODEL_WARNING;
        case CHIP_ESP32C6:
            return "ESP32-C6" SYSTEM_MODEL_WARNING;
        case CHIP_ESP32H2:
            return "ESP32-H2" SYSTEM_MODEL_WARNING;
        case CHIP_ESP32C5:
            return "ESP32-C5";
        default:
            return "Unknown" SYSTEM_MODEL_WARNING;
    }
}

static void system_fmt_mac(char *out, size_t n, const uint8_t mac[6])
{
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void system_build_info(char *buf, size_t n)
{
    // Heap totals
    size_t heap_free_total = esp_get_free_heap_size(); // Total free bytes malloc-able now
    size_t heap_min_free_total = esp_get_minimum_free_heap_size(); // Lowest value heap_free_total has ever reached since boot (watermark)
    
    size_t heap_free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); // Internal (on-chip) RAM only
    size_t heap_largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL); // Size of the largest single contiguous free block in internal RAM

    size_t heap_free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM); // External PSRAM only
    size_t heap_largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); // Largest contiguous PSRAM block

    // Load uptime
    uint64_t uptime_s_live = lcd_get_uptime_seconds();

    // Calculate days/hours/minutes
    uint64_t total = uptime_s_live;
    uint64_t uptime_d = total / 86400ULL;
    total %= 86400ULL;
    uint64_t uptime_h = total / 3600ULL;
    total %= 3600ULL;
    uint64_t uptime_m = total / 60ULL;
    uint64_t uptime_s = total % 60ULL;

    // Chip / IDF
    esp_chip_info_t ci;
    esp_chip_info(&ci);

    // MACs
    uint8_t mac_sta[6] = {0}, mac_ap[6] = {0}, mac_bt[6] = {0};
    (void)esp_read_mac(mac_sta, ESP_MAC_WIFI_STA);
    (void)esp_read_mac(mac_ap,  ESP_MAC_WIFI_SOFTAP);
    (void)esp_read_mac(mac_bt,  ESP_MAC_BT);

    char mac_sta_str[18], mac_ap_str[18], mac_bt_str[18];
    system_fmt_mac(mac_sta_str, sizeof(mac_sta_str), mac_sta);
    system_fmt_mac(mac_ap_str,  sizeof(mac_ap_str),  mac_ap);
    system_fmt_mac(mac_bt_str,  sizeof(mac_bt_str),  mac_bt);

    // Stack watermark (this task)
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL); // Minimum free stack your task has had since it started

    const char *idf = esp_get_idf_version();
    
    // Get this firmware version
    char pc5_fw_version[64];
    esp_err_t err = wifi_ota_update_get_nvs_version(pc5_fw_version, sizeof(pc5_fw_version));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_ota_update_get_nvs_version failed: %s", esp_err_to_name(err));
    }

    // Format chip revision and cores
    unsigned rev = (unsigned)ci.revision;
    unsigned rev_major = rev / 100; // X
    unsigned rev_minor = (rev / 10) % 10; // Y
    unsigned rev_sub = rev % 10; // Z
    
    char chip_line[96];
    snprintf(chip_line, sizeof(chip_line), "Chip: %s\n%u core(s), rev %u.%u.%u",
            system_chip_model_to_str(ci.model), (unsigned)ci.cores, rev_major, rev_minor, rev_sub);

    // NVS stats
    nvs_stats_t st = {0};
    const esp_partition_t *nvs_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
    char nvs_line[96] = "NVS: (not found)\n\n";
    if (nvs_part) {
        err = nvs_get_stats(nvs_part->label, &st);
        if (err == ESP_OK) {
            size_t used_est_bytes = st.used_entries * 32;
            size_t free_est_bytes = st.free_entries * 32;
    
            // Show physical NVS partition size (KB) + logical entries used/free
            snprintf(nvs_line, sizeof(nvs_line), "NVS (user data):\n%u KB partition\n%.1f of %.1f KB used\n\n",
                    (unsigned)(nvs_part->size / 1000U), (double)used_est_bytes / 1000.0, (double)((free_est_bytes + used_est_bytes) / 1000.0));
        } else {
            ESP_LOGE(TAG, "nvs_get_stats failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "esp_partition_find_first failed");
    }

    // Compose buffer text
    snprintf(buf, n,
        "Total uptime:\n"
        "%" PRIu64 " days\n"
        "%" PRIu64 " hours\n"
        "%" PRIu64 " minutes\n"
        "%" PRIu64 " seconds\n\n"

        "Firmware: v%s\n"
        "ESP-IDF: %s\n\n"

        "%s\n\n" // Chip info

        "%s" // NVS info
        
        "Heap free: %.1f KB\n"
        "(Min reached: %.1f KB)\n\n"
        
        "SRAM free: %.1f KB\n"
        "(Max block: %.1f KB)\n\n"
        
        "PSRAM free: %.1f KB\n"
        "(Max block: %.1f KB)\n\n"
        
        "Stack high-water mark:\n"
        "%u words\n\n"
        
        "WiFi:\n"
        "STA: %s\n"
        "AP: %s\n\n"
        "BT:\n"
        "MAC: %s",
        
        uptime_d, uptime_h, uptime_m, uptime_s,

        pc5_fw_version,
        idf,

        chip_line,

        nvs_line,
        
        (float)(heap_free_total / 1000.0f),
        (float)(heap_min_free_total / 1000.0f),
        
        (float)(heap_free_int / 1000.0f),
        (float)(heap_largest_int / 1000.0f),
        
        (float)(heap_free_psram / 1000.0f),
        (float)(heap_largest_psram / 1000.0f),
        
        (unsigned)watermark,
        
        mac_sta_str,
        mac_ap_str,
        mac_bt_str
    );
}

static void system_refresh_cb(lv_timer_t *t)
{
    lv_obj_t *label = (lv_obj_t *)lv_timer_get_user_data(t);
    
    POLYCAST5_USE_PSRAM static char text[512];
    memset(text, 0, sizeof(text));
    system_build_info(text, sizeof(text));
    
    lv_label_set_text(label, text);
    lv_timer_handler();
}

void lcd_settings_system_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    #define SYSTEM_Y_OFFSET 40

    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;
    static lv_timer_t *refresh_timer = NULL;

    // Init once
    if (!init) {
        // Main container
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Title label
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "System Info");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // System text
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100));
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        static char boot_text[512]; // instr_lbl buffer
        system_build_info(boot_text, sizeof(boot_text));
        lv_label_set_text(instr_lbl, boot_text);

        // Refresh data every second
        refresh_timer = lv_timer_create(system_refresh_cb, 1000, instr_lbl);

        // Show initial
        lv_timer_handler();
        init = true;
    }
    
    // Scroll up
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, SYSTEM_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) { // Scroll down
        lv_obj_scroll_by_bounded(cont, 0, -SYSTEM_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->left_btn) { // Back selected
        // Delete objects
        if (refresh_timer) {
            lv_timer_del(refresh_timer);
            refresh_timer = NULL;
        }
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        // Show settings page
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch
        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off selected
        // Delete objects
        if (refresh_timer) {
            lv_timer_del(refresh_timer);
            refresh_timer = NULL;
        }
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_settings_help_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    #define HELP_Y_OFFSET 40
    
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
        lv_label_set_text(title_lbl, "Tips and Tricks");
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

        // Set custom text
        const char *instr_text =
                                "Some tips and tricks for using PolyCast5 like a pro!\n\n"
                                "First and foremost, PolyCast5 has a hardware level reset that can be triggered by pressing "
                                "the HOME and RIGHT buttons at the same time.\n\n"
                                "This will not erase any user data, so feel free to use it any time if something is being weird.\n\n"
                                "PolyCast5 also has OTA wireless updates so you can always be running the newest firmware.\n\n"
                                "To check if an update is available, simply click 'Check for Updates' in the 'Settings' menu.\n\n"
                                "Being open-source, you can also feel free to check out the source code at:\n\ngithub.com/RoboticWo rx/PolyCast5\n\n"
                                "For additional information, docs, and guides, please visit polycast5.com or scan the QR codes in 'Read the Docs' under the 'Tools' menu!";
        
        lv_label_set_text(instr_lbl, instr_text);

        lv_timer_handler();

        init = true;
    }
    
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, HELP_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -HELP_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->left_btn) { // Go back
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
            
        // Show settings menu
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch back
        ui_menu->page = SETTINGS_PAGE;
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

// Helper to calculate power savings based on lcd brightness
static float get_mA(int brightness) {
    // Create struct for % vs mA
    static const struct {
        int b;
        float ma;
    } points[] = { // Observed values (% brightness, mA draw):
        {100, 67.0f}, {80, 62.0f}, {60, 57.5f},
        {40, 53.0f}, {20, 49.0f}, {0, 44.5f}
    };
    
    const int num_intervals = sizeof(points) / sizeof(points[0]) - 1;
    
    // Cap
    if (brightness >= 100) {
        return 67.0f;
    } else if (brightness <= 0) {
        return 44.5f;
    }

    // Linearly interpolate actual based on points
    for (int i = 0; i < num_intervals; ++i) {
        int b_hi = points[i].b;
        float ma_hi = points[i].ma;
        int b_lo = points[i + 1].b;
        float ma_lo = points[i + 1].ma;
        
        // If actual brightness is between the two points
        if (b_lo <= brightness && brightness <= b_hi) {
            // Interpolate mA based on percentage fraction
            float frac = (float)(brightness - b_lo) / (b_hi - b_lo);
            return ma_lo + frac * (ma_hi - ma_lo);
        }
    }
    
    return 44.5f; // Fallback
}
static int mA_to_percent(float ma)
{
    float i_ref = get_mA(100);; // 100% brightness total current draw 
    float i_now = get_mA(lcd_ledc_brightness); // Current total draw (mA)
    
    // Guard against near-zero
    if (i_now < 0.5f) {
        i_now = 0.5f;
    }
    
    // +x% longer runtime
    int pct_runtime = (int)(((i_ref / i_now) - 1.0f) * 100.0f + 0.5f);
        
    return pct_runtime;
}
void lcd_settings_adjust_lcd_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    #define ADJ_LCD_INS_TXT "LCD brightness:"
    #define ADJ_LCD_TXT "%d%%"
    #define ADJ_LCD_SAVINGS_TXT "+%d%% battery life"
    
    #define LCD_LEDC_MAX 100
    #define LCD_LEDC_MIN 0
    
    // Statics
    static bool init = false;
    
    static lv_obj_t *lbl_ins;
    static lv_obj_t *lbl_val;
    static lv_obj_t *lbl_savings;
    static lv_obj_t *slider;
    
    // Only execute once
    if (!init) {
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, ADJ_LCD_INS_TXT, user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_CENTER, -25, -13);
        
        xSemaphoreTake(xLEDCMutex, portMAX_DELAY); // Lock LEDC
        lbl_val = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_val, "", user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_CENTER, -20, 13);
        lv_label_set_text_fmt(lbl_val, ADJ_LCD_TXT, lcd_ledc_brightness);
        
        lbl_savings = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_savings, "", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, -20, 40);
        
        // Update savings text
        if (mA_to_percent(get_mA(lcd_ledc_brightness)) > 0) {
            lv_label_set_text_fmt(lbl_savings, ADJ_LCD_SAVINGS_TXT, mA_to_percent(get_mA(lcd_ledc_brightness)));
        } else { // Don't show if savings is 0mA
            lv_label_set_text(lbl_savings, "");
        }

        // Slider
        slider = lv_slider_create(ACTIVE_SCR);
        lv_obj_set_size(slider, 10, 100);
        lv_obj_align(slider, LV_ALIGN_CENTER, 65, 0);
        lv_slider_set_range(slider, LCD_LEDC_MIN, LCD_LEDC_MAX);
        lv_slider_set_value(slider, lcd_ledc_brightness, LV_ANIM_OFF);
        xSemaphoreGive(xLEDCMutex); // Release LEDC
        
        init = true;
    }
    
    // Increase brightness
    if (ui_btns->up_btn == 1) {
        xSemaphoreTake(xLEDCMutex, portMAX_DELAY); // Lock LEDC
        lcd_ledc_brightness += 5;
        
        // Wrap
        if (lcd_ledc_brightness > LCD_LEDC_MAX) {
            lcd_ledc_brightness = LCD_LEDC_MIN;
        }
        
        // Create text
        lv_label_set_text_fmt(lbl_val, ADJ_LCD_TXT, lcd_ledc_brightness);
        lv_slider_set_value(slider, lcd_ledc_brightness, LV_ANIM_OFF);
        
        // Update savings text
        if (mA_to_percent(get_mA(lcd_ledc_brightness)) > 0) {
            lv_label_set_text_fmt(lbl_savings, ADJ_LCD_SAVINGS_TXT, mA_to_percent(get_mA(lcd_ledc_brightness)));
        } else { // Don't show if savings is 0mA
            lv_label_set_text(lbl_savings, "");
        }
        
        // Persist to NVS
        lcd_settings_lcd_ledc_nvs_save();
        xSemaphoreGive(xLEDCMutex); // Release LEDC
        
        xSemaphoreGive(xLEDCSemaphore); // Update brightness
    } else if (ui_btns->down_btn == 1) { // Decrease brightness
        xSemaphoreTake(xLEDCMutex, portMAX_DELAY); // Lock LEDC
        lcd_ledc_brightness -= 5;
        
        // Wrap
        if (lcd_ledc_brightness < LCD_LEDC_MIN) {
            lcd_ledc_brightness = LCD_LEDC_MAX;
        }
        
        // Create text
        lv_label_set_text_fmt(lbl_val, ADJ_LCD_TXT, lcd_ledc_brightness);
        lv_slider_set_value(slider, lcd_ledc_brightness, LV_ANIM_OFF);
        
        // Update savings text
        if (mA_to_percent(get_mA(lcd_ledc_brightness)) > 0) {
            lv_label_set_text_fmt(lbl_savings, ADJ_LCD_SAVINGS_TXT, mA_to_percent(get_mA(lcd_ledc_brightness)));
        } else { // Don't show if savings is 0mA
            lv_label_set_text(lbl_savings, "");
        }
        
        // Persist to NVS
        lcd_settings_lcd_ledc_nvs_save();
        xSemaphoreGive(xLEDCMutex); // Release LEDC
        
        xSemaphoreGive(xLEDCSemaphore); // Update brightness
    } else if (ui_btns->left_btn == 1) { // Back selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_val);
        lv_obj_delete(lbl_savings);
        lv_obj_delete(slider);
        
        // Reset statics
        lbl_ins = lbl_val = lbl_savings = NULL;
        slider = NULL;
        init = false;
        
        // Show settings list
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_val);
        lv_obj_delete(lbl_savings);
        lv_obj_delete(slider);
        
        // Reset statics
        lbl_ins = lbl_val = lbl_savings = NULL;
        slider = NULL;
        init = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_settings_factory_rst_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
    // Statics
    static bool do_once = false;
    
    static lv_obj_t *lbl_ins;
    static lv_obj_t *lbl_note;
    
    // Only execute once
    if (!do_once) {
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "Press RIGHT to factory reset.", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 18);
                     
        lbl_note = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_note, "NOTE: This will erase\n       all user data!", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);

        do_once = true;
    }
    
    // Factory reset
    if (ui_btns->right_btn == 1) {
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_note);
        
        // Reset statics
        lbl_ins = lbl_note = NULL;
        do_once = false;
        
        // Confirmation text
        lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_rst, "Resetting...", user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 0);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        ESP_ERROR_CHECK(nvs_flash_erase()); // Factory reset
        esp_restart();
    } else if (ui_btns->left_btn == 1) { // Back selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_note);
        
        // Reset statics
        lbl_ins = lbl_note = NULL;
        do_once = false;
        
        // Show settings list
        lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Show arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = SETTINGS_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Delete objects
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_note);
        
        // Reset statics
        lbl_ins = lbl_note = NULL;
        do_once = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

/* NVS functions */

void lcd_settings_color_nvs_save(int new_color_idx, bool is_primary)
{
    esp_err_t err;
    nvs_handle_t handle;

    // Open NVS
    err = nvs_open(SETTINGS_COLOR_NS, NVS_READWRITE, &handle);
    ESP_ERROR_CHECK(err);

    // Pick a key for primary vs secondary
    const char *key = is_primary ? SETTINGS_COLOR_PRIM_KEY : SETTINGS_COLOR_SEC_KEY;

    // Store the index
    err = nvs_set_i32(handle, key, new_color_idx);
    ESP_ERROR_CHECK(err);

    //Commit & close
    err = nvs_commit(handle);
    ESP_ERROR_CHECK(err);
    nvs_close(handle);
}

void lcd_settings_color_nvs_load(void)
{
    esp_err_t err;
    nvs_handle_t handle;

    // Open NVS
    err = nvs_open(SETTINGS_COLOR_NS, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        int32_t idx;
        // Get primary color
        if (nvs_get_i32(handle, SETTINGS_COLOR_PRIM_KEY, &idx) == ESP_OK) {
            user_primary_color = primary_color_options[idx];
        }
        
        // Get secondary color
        if (nvs_get_i32(handle, SETTINGS_COLOR_SEC_KEY, &idx) == ESP_OK) {
            user_secondary_color = secondary_color_options[idx];
        }
        nvs_close(handle);
    }
}

void lcd_settings_pin_nvs_save(const settings_menu_t *menu)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_PIN_NS, NVS_READWRITE, &h);
    
    // Check
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_pin_nvs_save NVS open error");
    }

    // Save pin_set as a u8
    err = nvs_set_u8(h, SETTINGS_PIN_SET_KEY, menu->pin_menu.pin_set ? 1 : 0);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_pin_nvs_save NVS set u8 error");
        goto out;
    }

    // Save unlock_pin as a string
    err = nvs_set_str(h, SETTINGS_PIN_KEY, menu->pin_menu.unlock_pin);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_pin_nvs_save NVS set str error");
        goto out;
    }

    // Persist to NVS
    nvs_commit(h);

    out:
    nvs_close(h);
}

void lcd_settings_pin_nvs_load(settings_menu_t *menu)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_PIN_NS, NVS_READONLY, &h);
    
    // If nothing saved yet
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        menu->pin_menu.pin_set = false;
        menu->pin_menu.unlock_pin[0] = '\0';
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "lcd_settings_pin_nvs_load nvs_open failed: %s", esp_err_to_name(err));
        #endif
        
        return;
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_pin_nvs_load nvs_open 2 failed: %s", esp_err_to_name(err));
    }

    // Read pin_set
    uint8_t u8;
    
    err = nvs_get_u8(h, SETTINGS_PIN_SET_KEY, &u8);
    if (err == ESP_OK) {
        menu->pin_menu.pin_set = (u8 != 0);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        menu->pin_menu.pin_set = false;
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "lcd_settings_pin_nvs_load nvs_get_u8 ESP_ERR_NVS_NOT_FOUND");
        #endif
    } else {
        goto out;
    }

    // Read unlock_pin
    size_t required_size = 0;
    
    // Get size
    err = nvs_get_str(h, SETTINGS_PIN_KEY, NULL, &required_size);
    
    // If DNE
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        menu->pin_menu.unlock_pin[0] = '\0';
        #ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "lcd_settings_pin_nvs_load nvs_get_str ESP_ERR_NVS_NOT_FOUND");
        #endif
    } else if (err == ESP_OK) {
        // Stored string too long for our buffer
        if (required_size > sizeof(menu->pin_menu.unlock_pin)) {
            ESP_LOGE(TAG, "lcd_settings_pin_nvs_load size error");
            goto out;
        }
        // Actually read it
        err = nvs_get_str(h, SETTINGS_PIN_KEY, menu->pin_menu.unlock_pin, &required_size);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "lcd_settings_pin_nvs_load set str error");
        }
    }

    out:
    nvs_close(h);
}

void lcd_settings_pin_attempts_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_ATTEMPTS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Pin attempts NVS open error: %s", esp_err_to_name(err));
        goto out;
    }

    // Store pin_attempts as a uint32
    err = nvs_set_u32(h, SETTINGS_ATTEMPTS_KEY, pin_attempts);
    if (err == ESP_OK) {
        // Commit to flash
        err = nvs_commit(h);
        
        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Saved pin attempts: %" PRIu32, pin_attempts);
        #endif
    } else {
        ESP_LOGE(TAG, "Failed to save pin attempts: %s", esp_err_to_name(err));
    }
    
    // Close NVS
    out:
    nvs_close(h);
}

void lcd_settings_pin_attempts_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_ATTEMPTS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Pin attempts nvs_open failed: %s", esp_err_to_name(err));
        #endif
        
        goto out;
    }
    
    // Get the uint32
    uint32_t stored = 0;
    err = nvs_get_u32(h, SETTINGS_ATTEMPTS_KEY, &stored);
    switch (err) {
        case ESP_OK:
            pin_attempts = stored;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            // First‐boot or key erased: default
            pin_attempts = 0;
            break;
        default:
            break;
    }
    
    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Loaded pin attempts: %" PRIu32, stored);
    #endif
    
    // Close NVS
    out:
    nvs_close(h);
}

void lcd_settings_haptics_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_HAPTICS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_haptics_nvs_save: open failed (%s)", esp_err_to_name(err));
        return;
    }

    // Save the haptic length
    err = nvs_set_u8(h, SETTINGS_HAPTIC_DUR_KEY, haptic_len_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_haptics_nvs_save: len set failed");
    }

    // Pack the 6 bools into a single byte mask
    uint8_t mask = 0;
    for (int i = 0; i < 6; ++i) {
        if (haptic_btns[i]) {
            mask |= (1 << i);
        }
    }
    
    // Save that mask
    err = nvs_set_u8(h, SETTINGS_HAPTIC_STATES_KEY, mask);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_haptics_nvs_save: states set failed");
    }

    // Commit changes
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_haptics_nvs_save: commit failed");
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_haptics_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_HAPTICS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // First boot: leave defaults
        return;
    }

    // Load slider length
    uint8_t len;
    if (nvs_get_u8(h, SETTINGS_HAPTIC_DUR_KEY, &len) == ESP_OK) {
        haptic_len_ms = len;
    }

    // Load haptic btn states
    uint8_t mask;
    if (nvs_get_u8(h, SETTINGS_HAPTIC_STATES_KEY, &mask) == ESP_OK) {
        for (int i = 0; i < 6; ++i) {
            haptic_btns[i] = !!(mask & (1 << i));
        }
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_sleep_timer_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_SLEEP_TIMER_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_sleep_timer_nvs_save: open failed (%s)", esp_err_to_name(err));
        return;
    }

    // Save the timer length
    err = nvs_set_u16(h, SETTINGS_SLEEP_TIMER_KEY, home_sleep_after_s);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_sleep_timer_nvs_save: len set failed");
    }

    // Commit changes
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_sleep_timer_nvs_save: commit failed");
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_sleep_timer_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_SLEEP_TIMER_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // First boot: leave defaults
        return;
    }

    // Load sleep timer length
    uint16_t len;
    if (nvs_get_u16(h, SETTINGS_SLEEP_TIMER_KEY, &len) == ESP_OK) {
        home_sleep_after_s = len;
        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Loaded sleep timer: %u sec", home_sleep_after_s);
        #endif
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_rgb_led_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_RGB_LED_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_rgb_led_nvs_save: open failed (%s)", esp_err_to_name(err));
        return;
    }
    
    // Save the RGB LED period
    err = nvs_set_i16(h, SETTINGS_RGB_LED_PERIOD_KEY, rbg_blink_period_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_rgb_led_nvs_save: rbg_blink_period_ms set failed");
    }
    
    // Save the RGB LED total duration
    err = nvs_set_i16(h, SETTINGS_RGB_LED_TOTAL_KEY, rgb_blink_total_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_rgb_led_nvs_save: rgb_blink_total_ms set failed");
    }

    // Commit changes
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_rgb_led_nvs_save: commit failed");
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_rgb_led_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_RGB_LED_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // First boot: leave defaults
        return;
    }

    // Load the RGB LED period
    int16_t len;
    if (nvs_get_i16(h, SETTINGS_RGB_LED_PERIOD_KEY, &len) == ESP_OK) {
        rbg_blink_period_ms = len;
        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Loaded RGB LED period: %d ms", rbg_blink_period_ms);
        #endif
    }
    
    // Load the RGB LED total duration
    if (nvs_get_i16(h, SETTINGS_RGB_LED_TOTAL_KEY, &len) == ESP_OK) {
        rgb_blink_total_ms = len;
        #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Loaded RGB LED total duration: %d ms", rgb_blink_total_ms);
        #endif
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_lcd_ledc_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_LCD_LEDC_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_lcd_ledc_nvs_save: open failed (%s)", esp_err_to_name(err));
        return;
    }

    // Save the brightness (0-100)
    err = nvs_set_i8(h, SETTINGS_LCD_LEDC_KEY, lcd_ledc_brightness);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_lcd_ledc_nvs_save: brightness set failed");
    }

    // Commit changes
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_lcd_ledc_nvs_save: commit failed");
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_lcd_ledc_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_LCD_LEDC_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // First boot or NS doesn't exist: return default
        #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "LCD brightness NS not found, using default %d%%", lcd_ledc_brightness);
        #endif
        return;
    }

    // Load the brightness
    int8_t loaded;
    err = nvs_get_i8(h, SETTINGS_LCD_LEDC_KEY, &loaded);
    if (err == ESP_OK) {
        lcd_ledc_brightness = loaded;
        #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Loaded LCD brightness: %d%%", lcd_ledc_brightness);
        #endif
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Key not found: use default
        #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "LCD brightness key not found, using default 100%%");
        #endif
    } else {
        ESP_LOGE(TAG, "lcd_settings_lcd_ledc_nvs_load: get failed (%s)", esp_err_to_name(err));
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_uptime_nvs_save(uint64_t uptime_seconds)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_UPTIME_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_uptime_nvs_save: open failed: %s", esp_err_to_name(err));
        return;
    }

    // Save the uptime
    err = nvs_set_u64(h, SETTINGS_UPTIME_KEY, uptime_seconds);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_uptime_nvs_save: set failed: %s", esp_err_to_name(err));
    }

    // Commit changes
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_uptime_nvs_save: commit failed: %s", esp_err_to_name(err));
    }
    
    // Close NVS
    nvs_close(h);
}

void lcd_settings_uptime_nvs_load(uint64_t *uptime_seconds)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(SETTINGS_UPTIME_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "lcd_settings_uptime_nvs_load: open failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "lcd_settings_uptime_nvs_load: open failed: %s", esp_err_to_name(err));
        }
        return;
    }
    
    // Load the uptime
    err = nvs_get_u64(h, SETTINGS_UPTIME_KEY, uptime_seconds);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Key not found: default to 0
        *uptime_seconds = 0;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_settings_uptime_nvs_load: get failed: %s", esp_err_to_name(err));
    }
    
    // Close NVS
    nvs_close(h);
}