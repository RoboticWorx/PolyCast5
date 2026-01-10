#include "core/lv_obj_pos.h"

#include "core/lv_obj.h"
#include "lcd_hotkey_funcs.h"
#include "portmacro.h"
#include "misc/lv_area.h"

#include "nvs.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_random.h"

#include "lcd_asset_macros.h"
#include "lcd_utils.h"
#include "lcd_lora_funcs.h"
#include "lcd_espnow_funcs.h"
#include "espnow_task.h"

#include "gpio_task.h"

// RX MAC addresses saved via ESP-NOW
#define ESPNOW_RX_MAC_NS "espnow_rxmac" // Namespace
#define ESPNOW_RX_MAC_KEY_COUNT "count" // u8: number of MACs
#define ESPNOW_RX_MAC_KEY_FMT "rxmac_%d"// e.g. "mac_0", "mac_1", ...

// Menu options for ESP-NOW
#define ESPNOW_MENU_NS "espnow_menu" // Namespace
#define ESPNOW_MENU_KEY_COUNT "count" // u8: number of items
#define ESPNOW_MENU_KEY_FMT "item_%02d" // e.g. "item_00", "item_01", ...

// Local master key for ESP-NOW peers
#define ESPNOW_LMK_NS "espnow_lmk" // Namespace
#define ESPNOW_LMK_KEY_COUNT "count" // u8: number of keys
#define ESPNOW_LMK_KEY_FMT "lmk_%02d" // e.g. "key_00", "key_01", ...

#define RX_MAC_IN_SEL_COLOR lv_palette_main(LV_PALETTE_RED)
#define TX_TXT "Transmit: "
#define RX_TXT "Received: "

#define ESPNOW_NUM_CHAR_ROWS 4

static const char *TAG = "LCD_ESPNOW_FUNCS";

static bool espnow_menu_overwrite = false;
static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};

static const char *espnow_char_rows[ESPNOW_NUM_CHAR_ROWS] = {
    "_ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "abcdefghijklmnopqrstuvwxyz",
    "0123456789",
    "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~"
};

espnow_menu_t espnow_menu = {
    .options = {"Add ESP32"},
    .size = 1,
    .index = 0,
    .cont = NULL,
};

void lcd_espnow_setup_page(espnow_menu_t *menu)
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
    
    if (menu->size > 1) {
        menu->index = 1;
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

void lcd_espnow_update_menu(espnow_menu_t *menu)
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

static bool display_mac_and_lmk(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    // Hide all but right arrow
    lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
    
    // Get device MAC address
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    char mac_str[30]; // “XX:XX:XX:XX:XX:XX\0” = 18 + "Device MAC:\n" = 30
    snprintf(mac_str, sizeof(mac_str), "Device MAC:\n%02X:%02X:%02X:%02X:%02X:%02X", my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
    
    // Generate ESP-NOW LMK
    #define OUT_BUF_LEN (16*2 + 15 /*colons*/ + 1 /*newline*/ + 16 /*"Generated key:"*/ + 1 /*'\0'*/)
    uint8_t lmk[LMK_LEN];
    
    esp_fill_random(lmk, LMK_LEN);
    
    char lmk_str[OUT_BUF_LEN];
    char *p = lmk_str;
    int written = snprintf(p, OUT_BUF_LEN, "Generated key:\n");
    p += written;
    // Append each byte as two‐digit hex + colon (except last)
    for (int i = 0; i < LMK_LEN; ++i) {
        if (i == 8) {
            *p++ = '\n';
        }
        p += snprintf(p, OUT_BUF_LEN - (p - lmk_str), "%02X", lmk[i]);
        if (i + 1 < LMK_LEN) {
            *p++ = ':';  // Add colon separator
        }
    }
    *p = '\0'; // Ensure null‐termination
    
    memcpy(espnow_menu->lmk[espnow_menu->size], lmk, LMK_LEN); // Save to struct
    
    // Create and format ins labels
    lv_obj_t *lbl_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ins, "Write this down!", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, 5, 2);
                 
    lv_obj_t *lbl_ok = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ok, "OK", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);
                 
    lv_obj_t *lbl_my_mac = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_my_mac, mac_str, user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_LEFT_MID, 5, -20);
                 
    lv_obj_t *lbl_lmk = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_lmk, lmk_str, user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_LEFT, 5, -2);    
                    
    while (1) {
        lv_timer_handler();
        
        // OK
        if (xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {
            // Show arrows
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_ins);
            lv_obj_delete(lbl_my_mac);
            lv_obj_delete(lbl_lmk);
            lv_obj_delete(lbl_ok);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Prompt to enter name
            ui_menu->page = ESPNOW_NAME_PAGE;
            
            // Go back
            return false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool prompt_yn_encryption(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
    
    // Create and format ins labels
    lv_obj_t *lbl_ask_enc = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ask_enc, "     Would you like\nto setup encryption?", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
                 
    lv_obj_t *lbl_enc_yes = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_enc_yes, "YES", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 13);
                 
    lv_obj_t *lbl_enc_no = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_enc_no, "NO", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -13);
                    
    while (1) {
        lv_timer_handler();
        
        // User hit cancel
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_enc_yes);
            lv_obj_delete(lbl_enc_no);
            
            // Hide right arrow
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Show ESP-NOW menu
            lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Switch pages
            ui_menu->page = ESPNOW_PAGE;
            
            // Go back
            return false;
        }
        // Yes encryption 
        else if (xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_enc_yes);
            lv_obj_delete(lbl_enc_no);
            
            // Go back
            return true;
        }
        // No encryption 
        else if (xSemaphoreTake(xDownButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_enc_yes);
            lv_obj_delete(lbl_enc_no);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
                        
            // Prompt to enter name
            ui_menu->page = ESPNOW_NAME_PAGE;
            
            // Go back
            return false;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lcd_espnow_get_rx_mac(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{    
    // Statics
    static uint8_t mac_bytes[ESPNOW_MAC_SIZE]; // 6 bytes of the MAC
    static uint8_t digit_index = 0; // Which hex‐digit is selected
    static lv_obj_t *lbl_sel_digit[12];
    static lv_obj_t *lbl_enter_mac = NULL;
    static lv_obj_t *lbl_how_to = NULL;
    static lv_obj_t *container = NULL;

    // Create everything once
    if (!container) {
        // Zero out the 6 bytes
        memset(mac_bytes, 0, sizeof(mac_bytes));

        // Create a container to hold the labels
        container = lv_obj_create(ACTIVE_SCR);
        
        // Format
        lv_obj_set_size(container, 220, 35);
        lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF); // Never draw bars
        lv_obj_set_style_bg_color(container, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_align(container, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_set_style_border_width(container, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);

        // Create labels for container
        for (int i = 0; i < 12; ++i) {
            lbl_sel_digit[i] = lv_label_create(container);
            
            // Start all digits at 0
            lcd_format_label(lbl_sel_digit[i], "0", user_secondary_color,
                     &lv_font_montserrat_20, LV_ALIGN_LEFT_MID, (i * 17) - 6, 0);

            // Color selected
            if (i == (int)digit_index) {
                lv_obj_set_style_text_color(lbl_sel_digit[i], lv_palette_main(LV_PALETTE_RED), 0);
            } else {
                lv_obj_set_style_text_color(lbl_sel_digit[i], user_secondary_color, 0);
            }
        }

        // Helper text at the bottom
        lbl_enter_mac = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_enter_mac, "Enter receiver MAC:", user_secondary_color,
                     &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -56);
            
        // Instruction text at the top     
        lbl_how_to = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_how_to, "polycast5.com/blogs\n   /tutorials/get-mac", user_secondary_color,
                     &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 15);
    }

    // Iterate digit up
    if (ui_btns->up_btn) {
        int byte_idx = digit_index / 2; // Byte index that's being edited
        int nibble_pos = digit_index % 2; // 0 = high nibble, 1 = low nibble

        // Extract current nibble
        uint8_t cur_byte = mac_bytes[byte_idx]; // Select byte
        uint8_t high_n = (cur_byte >> 4) & 0x0F;
        uint8_t low_n = (cur_byte >> 0) & 0x0F;

        if (nibble_pos == 0) {
            // Increment high nibble with wrap
            high_n = (high_n + 1) & 0x0F;
        } else {
            // Increment low nibble with wrap
            low_n = (low_n + 1) & 0x0F;
        }
        // Update byte
        mac_bytes[byte_idx] = (uint8_t)((high_n << 4) | low_n);

        // Update just the one digit label
        char hex_char;
        if (nibble_pos == 0)
            hex_char = "0123456789ABCDEF"[high_n];
        else
            hex_char = "0123456789ABCDEF"[low_n];
            
        lv_label_set_text_fmt(lbl_sel_digit[digit_index], "%c", hex_char);
    } else if (ui_btns->down_btn) { // Decrement digit down
        int byte_idx = digit_index / 2; // Byte index that's being edited
        int nibble_pos = digit_index % 2; // 0 = high nibble, 1 = low nibble

        // Extract current nibble
        uint8_t cur_byte = mac_bytes[byte_idx]; // Select byte
        uint8_t high_n = (cur_byte >> 4) & 0x0F;
        uint8_t low_n = (cur_byte >> 0) & 0x0F;

        if (nibble_pos == 0) {
            // Decrement high nibble with wrap
            high_n = (high_n == 0 ? 0x0F : high_n - 1);
        } else {
            // Decrement low nibble with wrap
            low_n = (low_n == 0 ? 0x0F : low_n - 1);
        }

        // Update byte
        mac_bytes[byte_idx] = (uint8_t)((high_n << 4) | low_n);

        // Update just the one digit label
        char hex_char;
        if (nibble_pos == 0)
            hex_char = "0123456789ABCDEF"[high_n];
        else
            hex_char = "0123456789ABCDEF"[low_n];
            
        lv_label_set_text_fmt(lbl_sel_digit[digit_index], "%c", hex_char);
    } else if (ui_btns->left_btn && digit_index > 0) { // Move selection left
        // De-style old digit
        lv_obj_set_style_text_color(lbl_sel_digit[digit_index], user_secondary_color, 0);
        
        // Decrement
        digit_index--;
        
        // Style new digit
        lv_obj_set_style_text_color(lbl_sel_digit[digit_index], RX_MAC_IN_SEL_COLOR, 0);
    } else if (ui_btns->left_btn) { // Go back
        // Clean all
        for (int i = 0; i < 12; ++i) {
            lv_obj_delete(lbl_sel_digit[i]);
            lbl_sel_digit[i] = NULL;
        }

        lv_obj_delete(lbl_enter_mac);
        lv_obj_delete(lbl_how_to);
        lv_obj_delete(container);

        lbl_enter_mac = NULL;
        lbl_how_to = NULL;
        container = NULL;
        
        // Reset selected digit
        digit_index = 0;
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show ESP-NOW list
        lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Go back
        ui_menu->page = ESPNOW_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Go home or power off
        // Clean all
        for (int i = 0; i < 12; ++i) {
            lv_obj_delete(lbl_sel_digit[i]);
            lbl_sel_digit[i] = NULL;
        }

        lv_obj_delete(lbl_enter_mac);
        lv_obj_delete(lbl_how_to);
        lv_obj_delete(container);

        lbl_enter_mac = NULL;
        lbl_how_to = NULL;
        container = NULL;
        
        // Reset selected digit
        digit_index = 0;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    } else if (ui_btns->right_btn && digit_index < 11) { // Move selection right
        // De-style old digit
        lv_obj_set_style_text_color(lbl_sel_digit[digit_index], user_secondary_color, 0);
            
        // Increment
        digit_index++;
            
        // Style new digit
        lv_obj_set_style_text_color(lbl_sel_digit[digit_index], RX_MAC_IN_SEL_COLOR, 0);
    } else if (ui_btns->right_btn) { // Confirm
        // Copy the 6 bytes into espnow_menu->rx_mac[] for later use.
        for (int b = 0; b < ESPNOW_MAC_SIZE; b++) {
            espnow_menu->rx_mac[espnow_menu->size][b] = mac_bytes[b];
        }        
        
        // Clean all
        for (int i = 0; i < 12; ++i) {
            lv_obj_delete(lbl_sel_digit[i]);
            lbl_sel_digit[i] = NULL;
        }

        lv_obj_delete(lbl_enter_mac);
        lv_obj_delete(lbl_how_to);
        lv_obj_delete(container);

        lbl_enter_mac = NULL;
        lbl_how_to = NULL;
        container = NULL;
        
        // Reset selected digit
        digit_index = 0;
        
        // Ask if encryption is needed
        bool enc_peer = prompt_yn_encryption(ui_menu, espnow_menu);
        if (enc_peer) {
            display_mac_and_lmk(ui_menu, espnow_menu);
        } else {
            memset(espnow_menu->lmk[espnow_menu->size], 0, LMK_LEN); // Zero out enc entry
        }
    }
}

static void prompt_upload_qr(ui_menu_t *ui_menu)
{
    static lv_obj_t *qr_canvas = NULL;
    static uint8_t *qr_buf = NULL; // Canvas backing buffer
    
    // Hide arrows
    lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
    
    // Create and format ins labels
    lv_obj_t *lbl_ask_enc = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ask_enc, "Example receiver\ncode:", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, 5, 5);
                 
    lv_obj_t *lbl_qr_ok = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_qr_ok, "OK", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);
                 
    // Create QR canvas
    qr_canvas = lv_canvas_create(ACTIVE_SCR);
    lv_obj_set_size(qr_canvas, 100, 100);
    lv_obj_align(qr_canvas, LV_ALIGN_CENTER, 0, 12);
    
    // Draw the URL as a QR
    const char *url = "https://polycast5.com/blogs/tutorials/arduino-esp-now-receiver-examples";
    int n = lcd_draw_qr(qr_canvas, url, 100, &qr_buf);
    if (n != 0) {
        ESP_LOGE(TAG, "prompt_upload_qr lcd_draw_qr failed: %d", n);
    }
    
    while (1) {
        lv_timer_handler();
        
        // OK
        if (xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {
            // Show arrows
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            
            // Delete used
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_qr_ok);
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

static void update_name_label_lcd(lv_obj_t *lbl_display, char cur_char, int cur_pos)
{
    char display[MAX_CUSTOM_NAME_LEN + 2]; // Buffer
    
    int len = cur_pos + 1; // Current length of name
    
    // Cap
    if (len > MAX_CUSTOM_NAME_LEN + 1) {
        len = MAX_CUSTOM_NAME_LEN + 1;
    }
    
    // Copy name into display buffer
    if (cur_pos > 0) {
        memcpy(display, name_buf, cur_pos);
    }
    
    // Get current
    display[cur_pos] = cur_char;
    display[len] = '\0';
    
    // Set text and re-center
    lv_label_set_text(lbl_display, display);
    lv_obj_align(lbl_display, LV_ALIGN_CENTER, 0, 30);
}

void lcd_espnow_create_custom_name(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};
    
    // Declare statics
    static int cur_pos = 0; // User position
    static int row_idx = 0; // Which character row is active
    static int char_idx = 0; // Index within that row
    static char cur_char = '_';
    static lv_obj_t *lbl_dirs = NULL;
    static lv_obj_t *lbl_chars = NULL;
    static lv_obj_t *lbl_user_in = NULL;
    
    // Create initial label
    if (!lbl_user_in) {
        
        // If renaming, autofill what was there previously
        if (espnow_menu_overwrite) {
            // Copy the old name into buffer
            strncpy(name_buf, espnow_menu->options[espnow_menu->index], MAX_CUSTOM_NAME_LEN);

            // Place cursor at the end
            cur_pos = strlen(name_buf);
        } else { // Else blank slate
            memset(name_buf, 0, sizeof name_buf);
            cur_pos = 0;
        }
        
        // Starting char
        row_idx = 0;
        char_idx = 0;
        cur_char = espnow_char_rows[row_idx][char_idx];
        
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                         &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_dirs, "       Enter ESP32 name:\nPress HOME to cycle chars.", user_secondary_color,
                         &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -31);
                         
        if (espnow_menu_overwrite) {
            lv_label_set_text(lbl_dirs, "  Enter new ESP32 name:\nPress HOME to cycle chars.");
        }
        
        lbl_chars = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_chars, "(Up to 12 characters)", user_secondary_color,
                         &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
                         
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    }

    /* User input */
    // Cycle chars
    if (ui_btns->home_btn) {
        // Cycle character row
        row_idx = (row_idx + 1) % ESPNOW_NUM_CHAR_ROWS;
        char_idx = 0; // Reset within row
        
        // New current char
        cur_char = espnow_char_rows[row_idx][char_idx];
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->up_btn) { // If up, iterate up
        // Increment with wrap
        size_t row_len = strlen(espnow_char_rows[row_idx]);
        char_idx = (char_idx + 1) % (int)row_len;
        cur_char = espnow_char_rows[row_idx][char_idx];
        
        // Save to array
        name_buf[cur_pos] = cur_char;
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->down_btn) { // If down, iterate down
        // Decrement with wrap
        size_t row_len = strlen(espnow_char_rows[row_idx]);
        char_idx = (char_idx + (int)row_len - 1) % (int)row_len;
        cur_char = espnow_char_rows[row_idx][char_idx];
        
        // Save to array
        name_buf[cur_pos] = cur_char;
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->left_btn && cur_pos == 0) { // Can back out if at start
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(name_buf, 0, sizeof name_buf);
        
        // Only clear the staged slot if we were adding a new peer
        if (!espnow_menu_overwrite) {
            memset(espnow_menu->lmk[espnow_menu->size], 0, LMK_LEN); // Zero out enc entry
            memset(espnow_menu->rx_mac[espnow_menu->size], 0, ESPNOW_MAC_SIZE);
        }
        
        espnow_menu_overwrite = false;
        
        // Show ESP-NOW list
        lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
         ui_menu->page = ESPNOW_PAGE;
        return;
    } else if (ui_btns->pwr_btn == 1) { // Power off
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(name_buf, 0, sizeof name_buf);
        
        // Only clear the staged slot if we were adding a new peer
        if (!espnow_menu_overwrite) {
            memset(espnow_menu->lmk[espnow_menu->size], 0, LMK_LEN);
            memset(espnow_menu->rx_mac[espnow_menu->size], 0, ESPNOW_MAC_SIZE);
        }
        
        espnow_menu_overwrite = false;
        
        lcd_transition_back(false, ui_menu); // True = home, false = sleep
    } else if (ui_btns->left_btn && cur_pos != 0) { // If left and not at start
        // Clear the current slot
        name_buf[cur_pos] = '\0';
    
        // Decrement left
        if (cur_pos > 0) {
            cur_pos--;
        }
    
        // Reload row/idx from the new slot's char
        char target = name_buf[cur_pos] ? name_buf[cur_pos] : '_';
        for (row_idx = 0; row_idx < ESPNOW_NUM_CHAR_ROWS; row_idx++) {
            const char *row = espnow_char_rows[row_idx];
            const char *p = strchr(row, target);
            
            if (p) {
                char_idx = (int)(p - row);
                break;
            }
        }
        cur_char = espnow_char_rows[row_idx][char_idx];
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->right_btn) { // If right
        // Handle case where up/down wasn't pressed
        name_buf[cur_pos] = cur_char;
        
        // If not yet at end
        if (cur_pos < MAX_CUSTOM_NAME_LEN - 1) {
            cur_pos++;
            name_buf[cur_pos] = '\0';
            char_idx = 0;
            cur_char = espnow_char_rows[row_idx][char_idx];
        } else {
            name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
        }
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->select_btn) { // If save button pressed
        // Save final
        if (cur_pos < MAX_CUSTOM_NAME_LEN) {
            name_buf[cur_pos] = cur_char;

            // Terminate one past the last written char if room, else clamp
            size_t term = (cur_pos + 1 <= MAX_CUSTOM_NAME_LEN) ? (cur_pos + 1) : MAX_CUSTOM_NAME_LEN;
            name_buf[term] = '\0';
        }
        
        name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
        memcpy(saved_name, name_buf, MAX_CUSTOM_NAME_LEN + 1);
        
        #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "%s", saved_name);
        #endif
        
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(name_buf, 0, sizeof name_buf);

        // Update options
        // If overwriting an existing as a rename
        if (espnow_menu_overwrite) {
            // espnow_menu->index is edit_idx
            // Release old string then reallocate
            free(espnow_menu->options[espnow_menu->index]);
            espnow_menu->options[espnow_menu->index] = strdup(saved_name);

            // Persist to NVS
            lcd_espnow_menu_nvs_save(espnow_menu);

            // Update the button’s label in-place
            lv_obj_t *btn = espnow_menu->btns[espnow_menu->index];
            lv_obj_t *child_lbl = lv_obj_get_child(btn, 0);
            lv_label_set_text(child_lbl, espnow_menu->options[espnow_menu->index]);

            // Reset flag
            espnow_menu_overwrite = false;
        } else { // Else adding a whole new ESP32
            // Size one bigger
            espnow_menu->size++;
        
            // Save to options, then to NVS
            char *name_copy = strdup(saved_name);
            espnow_menu->options[espnow_menu->size - 1] = name_copy;
            lcd_espnow_menu_nvs_save(espnow_menu);
            
            // Create new button for new option
            espnow_menu->btns[espnow_menu->size - 1] = lv_list_add_btn(espnow_menu->main_list, NULL, espnow_menu->options[espnow_menu->size - 1]);
            lv_obj_set_size(espnow_menu->btns[espnow_menu->size - 1], 200, 30);
            lv_obj_add_style(espnow_menu->btns[espnow_menu->size - 1], &espnow_menu->btn_style, 0);
            
            // Create and format text label
            lv_obj_t *lbl = lv_obj_get_child(espnow_menu->btns[espnow_menu->size - 1], 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    
            // Save RX MAC from earlier to NVS
            lcd_espnow_rx_mac_nvs_save(espnow_menu);
            
            // Save LMK if it exists
            lcd_espnow_lmk_nvs_save(espnow_menu);
            
            prompt_upload_qr(ui_menu); // Show example QR
        }
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show ESP-NOW list
        lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch to ESP-NOW page
        ui_menu->page = ESPNOW_PAGE;
        return;
    }
}

void lcd_espnow_setup_send_page(espnow_menu_t *espnow_menu)
{
    #define X_POS -38
    espnow_menu->espnow_submenu.cmd_to_send = 1; // Set default
    
    // Create labels
    espnow_menu->espnow_submenu.lbl_send_tx = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT, user_secondary_color,
            &lv_font_montserrat_16, LV_ALIGN_CENTER, X_POS, 39);

    espnow_menu->espnow_submenu.lbl_send_rx = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT, user_secondary_color,
            &lv_font_montserrat_16, LV_ALIGN_CENTER, X_POS, 57);

    espnow_menu->espnow_submenu.lbl_send_cmd = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.lbl_send_cmd, "1", user_secondary_color,
            &lv_font_montserrat_30, LV_ALIGN_CENTER, X_POS, -20);

    espnow_menu->espnow_submenu.lbl_send_box = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.lbl_send_box, "", user_secondary_color,
            &lv_font_montserrat_24, LV_ALIGN_CENTER, X_POS, -20);
                     
    espnow_menu->espnow_submenu.lbl_send = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.lbl_send, "SEND", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);
                     
    espnow_menu->espnow_submenu.lbl_edit = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.lbl_edit, LV_SYMBOL_HOME " EDIT", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_BOTTOM_RIGHT, -5, -4);
                     
    espnow_menu->espnow_submenu.arrow_top = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.arrow_top, LV_SYMBOL_UP, user_secondary_color,
            &lv_font_montserrat_14, LV_ALIGN_CENTER, X_POS, -50);
                     
    espnow_menu->espnow_submenu.arrow_bot = lv_label_create(ACTIVE_SCR);
    lcd_format_label(espnow_menu->espnow_submenu.arrow_bot, LV_SYMBOL_DOWN, user_secondary_color,
            &lv_font_montserrat_14, LV_ALIGN_CENTER, X_POS, 10);

    // Create a style for the send cmd box
    static lv_style_t style_cmd;
    lv_style_init(&style_cmd);

    lv_style_set_radius(&style_cmd, 8);
    lv_style_set_bg_color(&style_cmd, user_primary_color);
    lv_style_set_border_width(&style_cmd, 2);
    lv_style_set_border_color(&style_cmd, user_secondary_color);
    lv_style_set_border_side(&style_cmd, LV_BORDER_SIDE_FULL);
    lv_style_set_text_color(&style_cmd, user_secondary_color);
    
    lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 100); // % darker 
    lv_style_set_shadow_spread(&style_cmd, 3);
    lv_style_set_shadow_width(&style_cmd, 6);
    lv_style_set_shadow_offset_x(&style_cmd, 3);
    lv_style_set_shadow_offset_y(&style_cmd, 3);
    lv_style_set_shadow_color(&style_cmd, darker_user_primary_color);
        
    lv_style_set_pad_left(&style_cmd, 55);
    lv_style_set_pad_right(&style_cmd, 55);
    lv_style_set_pad_top(&style_cmd, 25);
    lv_style_set_pad_bottom(&style_cmd, 25);
        
    lv_obj_add_style(espnow_menu->espnow_submenu.lbl_send_box, &style_cmd, 0);
    
    // Create a style for the edit box
    static lv_style_t style_edit;
    lv_style_init(&style_edit);

    lv_style_set_radius(&style_edit, 8);
    lv_style_set_bg_color(&style_edit, user_primary_color);
    lv_style_set_border_width(&style_edit, 2);
    lv_style_set_border_color(&style_edit, user_secondary_color);
    lv_style_set_border_side(&style_edit, LV_BORDER_SIDE_FULL);
    lv_style_set_text_color(&style_edit, user_secondary_color);
    
    lv_style_set_pad_left(&style_edit, 10);
    lv_style_set_pad_right(&style_edit, 10);
    lv_style_set_pad_top(&style_edit, 6);
    lv_style_set_pad_bottom(&style_edit, 6);
    
    lv_obj_add_style(espnow_menu->espnow_submenu.lbl_edit, &style_edit, 0);
    
    // Hide everything for now
    lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_tx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_rx, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
}

static void prompt_name_or_del(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
    
    // Create and format ins labels
    lv_obj_t *lbl_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ins, LV_SYMBOL_SETTINGS, user_secondary_color,
                 &lv_font_montserrat_30, LV_ALIGN_CENTER, 0, 0);
                 
    lv_obj_t *lbl_exit = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_exit, "BACK", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_LEFT_MID, 16, -1);
                 
    lv_obj_t *lbl_name = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_name, "RENAME", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 13);
                 
    lv_obj_t *lbl_del = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_del, "DELETE", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -13);
                    
    while (1) {
        lv_timer_handler();
        
        // User hit cancel
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            // Show ESP-NOW submenu
            lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_tx, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_rx, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(espnow_menu->espnow_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(espnow_menu->espnow_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(espnow_menu->espnow_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
            
            // Hide up and down arrows
            lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
                
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Switch pages
            ui_menu->page = ESPNOW_OPTION_PAGE;
            
            // Go back
            return;
        }
        // Rename
        else if (xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            espnow_menu_overwrite = true; // Set overwrite flag
            
            // Prompt to enter name
            ui_menu->page = ESPNOW_NAME_PAGE;
            
            // Go back
            return;
        }
        // Delete
        else if (xSemaphoreTake(xDownButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Delete the entry
            // Get user entry to remove
            int del_idx = espnow_menu->index;     
            
            // Just in case
            if (del_idx == 0) {
                lcd_clear_pending_inputs = true;
                lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
                ui_menu->page = ESPNOW_PAGE;
                return;
            }
            
            // Free any heap buffers allocated for that slot
            free(espnow_menu->options[del_idx]); // Name string
            lv_obj_delete(espnow_menu->btns[del_idx]); // LVGL list button
        
            // Shift everything above it down one
            for (int i = del_idx; i < espnow_menu->size - 1; ++i) {
                // Change each to the one after
                espnow_menu->options[i] = espnow_menu->options[i + 1];
                espnow_menu->btns[i] = espnow_menu->btns[i + 1];
        
                // Update the label inside the button
                lv_obj_t *lbl = lv_obj_get_child(espnow_menu->btns[i], 0);
                lv_label_set_text(lbl, espnow_menu->options[i]);
            }
            
            // Delete RX MAC (espnow_menu->size--)
            lcd_espnow_rx_mac_lmk_nvs_delete(espnow_menu, (uint8_t)(del_idx - 1));
            
            // Null out dangling index
            espnow_menu->options[espnow_menu->size] = NULL;
            espnow_menu->btns[espnow_menu->size] = NULL;
            
            // Adjust if was last
            if (espnow_menu->index >= espnow_menu->size) {
                espnow_menu->index = espnow_menu->size - 1;
            }
            
            // Persist to NVS
            lcd_espnow_menu_nvs_save(espnow_menu);
            
            // Refresh the list UI
            lcd_espnow_update_menu(espnow_menu);
            
            // Hide right arrow
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Switch pages
            ui_menu->page = ESPNOW_PAGE;
            
            // Go back
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lcd_espnow_option(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    #define BUF_SIZE 4
    
    if (xSemaphoreTake(xEspCmdTxSuccessSemaphore, 0) == pdTRUE) { // If transmission successful
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT LV_SYMBOL_OK);
    } else if (xSemaphoreTake(xEspCmdTxFailedSemaphore, 0) == pdTRUE) { // If transmission failed
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT LV_SYMBOL_CLOSE);
    }
    
    if (xSemaphoreTake(xEspCmdRxStatusSemaphore, 0) == pdTRUE) { // If data received
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT LV_SYMBOL_OK);
    }
    
    // Send command
    if (ui_btns->right_btn == 1) {
        // Reset receipts
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT);
        
        // Build the packet
        espnow_cmd_t espnow_cmd = {0};
        memcpy(espnow_cmd.mac_selected, espnow_menu->rx_mac[espnow_menu->index], ESPNOW_MAC_SIZE);
        espnow_cmd.cmd_to_send = espnow_menu->espnow_submenu.cmd_to_send;
        // enc = true if LMK
        espnow_cmd.enc = memcmp(espnow_menu->lmk[espnow_menu->index], (uint8_t[LMK_LEN]){0}, LMK_LEN) != 0;
        // If enc, copy LMK
        if (espnow_cmd.enc) {
            memcpy(espnow_cmd.lmk, espnow_menu->lmk[espnow_menu->index], LMK_LEN);
        }
        
        // If recording command as hotkey
        if (!lv_obj_has_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN)) {
            // Zero out at start
            memset(&hotkey_cmd.espnow_cmd[hotkey_cmd.active_idx], 0, sizeof(espnow_cmd_t));
            
            // Save into hotkey struct under selected "Hotx"
            hotkey_cmd.espnow_cmd[hotkey_cmd.active_idx].cmd_to_send = espnow_menu->espnow_submenu.cmd_to_send;
            memcpy(hotkey_cmd.espnow_cmd[hotkey_cmd.active_idx].mac_selected, espnow_menu->rx_mac[espnow_menu->index], ESPNOW_MAC_SIZE);
            
            // enc = true if LMK
            hotkey_cmd.espnow_cmd[hotkey_cmd.active_idx].enc = memcmp(espnow_menu->lmk[espnow_menu->index], (uint8_t[LMK_LEN]){0}, LMK_LEN) != 0;
            // If enc, copy LMK
            if (hotkey_cmd.espnow_cmd[hotkey_cmd.active_idx].enc) {
                memcpy(hotkey_cmd.espnow_cmd[hotkey_cmd.active_idx].lmk, espnow_menu->lmk[espnow_menu->index], LMK_LEN);
            }
            
            // Flag that command exists
            hotkey_cmd.has_espnow[hotkey_cmd.active_idx] = true;
            // Remove others
            hotkey_cmd.has_lora[hotkey_cmd.active_idx] = false;
            hotkey_cmd.has_ir[hotkey_cmd.active_idx] = false;
            
            // Hide hotkey icon
            lv_obj_add_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN);
            
            // Persist to NVS
            lcd_hotkey_nvs_save(&hotkey_cmd);
        }

        // Send the command
        xQueueSend(xEspSendCmdQueue, &espnow_cmd, portMAX_DELAY);
        
        // RGB indicator
        uint8_t rgb_state = RGB_BLINK_TEAL;
        xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
        
        lcd_clear_pending_inputs = true; // Would sometimes get "ghost" up press
    } else if (ui_btns->left_btn == 1) { // Exit
        // Reset receipts
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT);
        
        // Back to default
        espnow_menu->espnow_submenu.cmd_to_send = 1;
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", espnow_menu->espnow_submenu.cmd_to_send);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_cmd, buf);
        
        // Hide everything
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_tx, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_rx, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show up and down arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show ESP-NOW list
        lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Go back
        ui_menu->page = ESPNOW_PAGE;
    } else if (ui_btns->pwr_btn == 1) { // Power off
        // Reset receipts
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT);
        
        // Back to default
        espnow_menu->espnow_submenu.cmd_to_send = 1;
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", espnow_menu->espnow_submenu.cmd_to_send);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_cmd, buf);
        
        // Hide everything
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_tx, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_rx, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        lcd_transition_back(false, ui_menu); // True = home, false = sleep
    } else if (ui_btns->up_btn == 1) { // Increment command
        // Reset receipts
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT);
        
        espnow_menu->espnow_submenu.cmd_to_send++;
        
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", espnow_menu->espnow_submenu.cmd_to_send);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_cmd, buf);
    } else if (ui_btns->down_btn == 1) { // Decrement command
        // Reset receipts
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT);
        
        espnow_menu->espnow_submenu.cmd_to_send--;
        
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", espnow_menu->espnow_submenu.cmd_to_send);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_cmd, buf);
    } else if (ui_btns->select_btn == 1) { // Increment command by 3
        // Reset receipts
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT);
        
        espnow_menu->espnow_submenu.cmd_to_send += 3;
        
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", espnow_menu->espnow_submenu.cmd_to_send);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_cmd, buf);
    } else if (ui_btns->home_btn == 1) { // Edit
        // Reset receipts
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_tx, TX_TXT);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_rx, RX_TXT);
        
        // Back to default
        espnow_menu->espnow_submenu.cmd_to_send = 1;
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", espnow_menu->espnow_submenu.cmd_to_send);
        lv_label_set_text(espnow_menu->espnow_submenu.lbl_send_cmd, buf);
        
        // Hide everything
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_tx, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_rx, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(espnow_menu->espnow_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Show up and down arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        prompt_name_or_del(ui_menu, espnow_menu);
    }
}

esp_err_t lcd_espnow_menu_nvs_save(const espnow_menu_t *menu)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(ESPNOW_MENU_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    // menu->options[0] is default "Add New"
    // If menu->size == 1 there are no user names, otherwise there are menu->size - 1 names
    uint8_t user_cnt = (menu->size > 1) ? menu->size - 1 : 0;
    err = nvs_set_u8(h, ESPNOW_MENU_KEY_COUNT, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
        goto out;

    // Loop through all and number them: 00, 01, etc.
    for (uint8_t i = 0; i < user_cnt; ++i) {
        char key[16];
        snprintf(key, sizeof(key), ESPNOW_MENU_KEY_FMT, i);
        
        // Store the menu option string at each key starting at index 1
        err = nvs_set_str(h, key, menu->options[i + 1]);
        
        // Exit if error
        if (err != ESP_OK)
            goto out;
    }
    
    // Flush pending writes to flash
    err = nvs_commit(h);

    // Close NVS
    out: nvs_close(h);
    
    return err;
}

esp_err_t lcd_espnow_menu_nvs_load(espnow_menu_t *menu)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(ESPNOW_MENU_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;

    // Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, ESPNOW_MENU_KEY_COUNT, &user_cnt);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    menu->size = 1; // Don't change first option
    menu->index = 0;

    // Loop through all keys
    for (uint8_t i = 0; i < user_cnt; ++i) {
        
        char key[16];
        snprintf(key, sizeof(key), ESPNOW_MENU_KEY_FMT, i);
        size_t len = 0;
        
        // Extract the size of the string
        if (nvs_get_str(h, key, NULL, &len) != ESP_OK) {
            break;
        }

        // Ensure enough memory is available
        char *buf = malloc(len);
        if (!buf)
            break;
        
        // Extract the string
        if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
            free(buf);
            break;
        }

        // Update menu struct
        if (menu->size >= MAX_ESPNOW_OPTIONS ) {
            free(buf);
            break;
        }
        menu->options[menu->size++] = buf;
    }
    
    // Close NVS
    nvs_close(h);
    
    return ESP_OK;
}

esp_err_t lcd_espnow_rx_mac_nvs_save(const espnow_menu_t *espnow_menu)
{
    nvs_handle_t nvs_handle;
    
    // Open NVS
    esp_err_t err = nvs_open(ESPNOW_RX_MAC_NS, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) 
        return err;

    // Save how many MACs we have 
    // size - 1 since first is "Add ESP32"
    uint8_t user_cnt = (espnow_menu->size > 1) ? espnow_menu->size - 1 : 0;
    err = nvs_set_u8(nvs_handle, ESPNOW_RX_MAC_KEY_COUNT, user_cnt);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    // Write each present MAC, erase any that were removed
    for (int i = 0; i < MAX_ESPNOW_OPTIONS; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), ESPNOW_RX_MAC_KEY_FMT, i);

        // Up to num of MACs saved
        if (i < user_cnt) {
            // Save MAC in 6-byte blob
            err = nvs_set_blob(nvs_handle, key, espnow_menu->rx_mac[i + 1], ESPNOW_MAC_SIZE); // Skip 0 to allign with index
        } else {
            // Erase leftover key if it exists
            err = nvs_erase_key(nvs_handle, key);
            if (err == ESP_ERR_NVS_NOT_FOUND)
                err = ESP_OK;
        }

        if (err != ESP_OK) {
            nvs_close(nvs_handle);
            return err;
        }
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    
    // Close NVS
    nvs_close(nvs_handle);
    return err;
}

esp_err_t lcd_espnow_lmk_nvs_save(const espnow_menu_t *espnow_menu)
{
    nvs_handle_t nvs_handle;
    
    // Open NVS
    esp_err_t err = nvs_open(ESPNOW_LMK_NS, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) 
        return err;

    // Save how many LMKs we have 
    // size - 1 since first is "Add ESP32"
    uint8_t user_cnt = (espnow_menu->size > 1) ? espnow_menu->size - 1 : 0;
    err = nvs_set_u8(nvs_handle, ESPNOW_LMK_KEY_COUNT, user_cnt);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    // Write each present LMK, erase any that were removed
    for (int i = 0; i < MAX_ESPNOW_OPTIONS; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), ESPNOW_LMK_KEY_FMT, i);

        // Up to num of LMKs saved
        if (i < user_cnt) {
            // Save LMK in 16-byte blob
            err = nvs_set_blob(nvs_handle, key, espnow_menu->lmk[i + 1], LMK_LEN); // Skip 0 to allign with index
        } else {
            // Erase leftover key if it exists
            err = nvs_erase_key(nvs_handle, key);
            if (err == ESP_ERR_NVS_NOT_FOUND)
                err = ESP_OK;
        }

        if (err != ESP_OK) {
            nvs_close(nvs_handle);
            return err;
        }
    }

    // Commit changes
    err = nvs_commit(nvs_handle);
    
    // Close NVS
    nvs_close(nvs_handle);
    return err;
}

esp_err_t lcd_espnow_rx_mac_nvs_load(espnow_menu_t *espnow_menu)
{
    nvs_handle_t nvs;
    
    // Open NVS
    esp_err_t err = nvs_open(ESPNOW_RX_MAC_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) 
        return err;

    // Get number of MACs saved
    uint8_t cnt = 0;
    err = nvs_get_u8(nvs, ESPNOW_RX_MAC_KEY_COUNT, &cnt);
    
    if (err == ESP_ERR_NVS_NOT_FOUND)
        cnt = 0; // Nothing stored yet
    else if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    // Zero out macs
    memset(espnow_menu->rx_mac, 0, sizeof(espnow_menu->rx_mac));

    // Read each MAC into a blob
    for (uint8_t i = 0; i < cnt; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), ESPNOW_RX_MAC_KEY_FMT, i);

        size_t len = ESPNOW_MAC_SIZE;
        
        // Get the MAC blob
        err = nvs_get_blob(nvs, key, espnow_menu->rx_mac[i + 1], &len);
        
        if (err == ESP_ERR_NVS_NOT_FOUND) { // If there's a hole, zero it out
            memset(espnow_menu->rx_mac[i + 1], 0, ESPNOW_MAC_SIZE);
            continue;
        }
        if (err != ESP_OK || len != ESPNOW_MAC_SIZE) {
            nvs_close(nvs);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Close NVS
    nvs_close(nvs);

    // Bookkeeping
    espnow_menu->size = cnt + 1; // Menu size is number of MACs + 1
    espnow_menu->index = 0;

    return (cnt > 0) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t lcd_espnow_lmk_nvs_load(espnow_menu_t *espnow_menu)
{
    nvs_handle_t nvs;
    
    // Open NVS
    esp_err_t err = nvs_open(ESPNOW_LMK_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) 
        return err;

    // Get number of LMKs saved
    uint8_t cnt = 0;
    err = nvs_get_u8(nvs, ESPNOW_LMK_KEY_COUNT, &cnt);
    
    if (err == ESP_ERR_NVS_NOT_FOUND)
        cnt = 0; // Nothing stored yet
    else if (err != ESP_OK) {
        nvs_close(nvs);
        return err;
    }

    // Zero out LMKs
    memset(espnow_menu->lmk, 0, sizeof(espnow_menu->lmk));

    // Read each MAC into a blob
    for (uint8_t i = 0; i < cnt; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), ESPNOW_LMK_KEY_FMT, i);

        size_t len = LMK_LEN;
        
        // Get the MAC blob
        err = nvs_get_blob(nvs, key, espnow_menu->lmk[i + 1], &len);
        
        if (err == ESP_ERR_NVS_NOT_FOUND) { // If there's a hole, zero it out
            memset(espnow_menu->lmk[i + 1], 0, LMK_LEN);
            continue;
        }
        if (err != ESP_OK || len != LMK_LEN) {
            nvs_close(nvs);
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Close NVS
    nvs_close(nvs);

    // Bookkeeping
    espnow_menu->size = cnt + 1; // Menu size is number of LMKs + 1
    espnow_menu->index = 0;

    return (cnt > 0) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t lcd_espnow_rx_mac_lmk_nvs_delete(espnow_menu_t *espnow_menu, uint8_t slot)
{
    // If nothing to delete
    if (espnow_menu->size <= 1) 
        return ESP_ERR_INVALID_ARG;
        
    // Num macs
    uint8_t user_cnt = espnow_menu->size - 1;
    
    // If out of range
    if (slot >= user_cnt)
        return ESP_ERR_INVALID_ARG;

    // Shift everything after slot up one remove slot
    for (uint8_t i = slot; i < user_cnt - 1; ++i) {
        memcpy(espnow_menu->rx_mac[i + 1], espnow_menu->rx_mac[i + 2], ESPNOW_MAC_SIZE);
        memcpy(espnow_menu->lmk[i + 1], espnow_menu->lmk[i + 2], LMK_LEN);
    }
    
    // Zero out the dangling
    memset(espnow_menu->rx_mac[user_cnt], 0, ESPNOW_MAC_SIZE);
    memset(espnow_menu->lmk[user_cnt], 0, LMK_LEN);

    // Size one less
    espnow_menu->size--;

    // Persist changes to NVS
    esp_err_t err = lcd_espnow_rx_mac_nvs_save(espnow_menu);
    if (err == ESP_OK)
        err = lcd_espnow_lmk_nvs_save(espnow_menu);
    return err;
}


#ifdef POLYCAST5_ESPNOW_DUMP_NVS
    static void dump_names(void)
    {
        nvs_handle_t h;
        esp_err_t err = nvs_open(ESPNOW_MENU_NS, NVS_READONLY, &h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "menu-ns open failed: %s", esp_err_to_name(err));
            return;
        }
    
        uint8_t cnt = 0;
        nvs_get_u8(h, ESPNOW_MENU_KEY_COUNT, &cnt);
        ESP_LOGI(TAG, "=== ESP-NOW peer names (%u) ===", cnt);
    
        for (uint8_t i = 0; i < cnt; ++i) {
            char key[16];  snprintf(key, sizeof(key), ESPNOW_MENU_KEY_FMT, i);
    
            size_t len = 0;
            err = nvs_get_str(h, key, NULL, &len);
            if (err == ESP_OK && len > 1 && len < 64) {
                char *buf = malloc(len);
                if (buf) {
                    nvs_get_str(h, key, buf, &len);
                    ESP_LOGI(TAG, "  [%u] \"%s\"", i, buf);
                    free(buf);
                }
            } else {
                ESP_LOGW(TAG, "  [%u] missing or too long (%s)", i, esp_err_to_name(err));
            }
        }
        nvs_close(h);
    }
    
    static void dump_macs(void)
    {
        nvs_handle_t h;
        esp_err_t err = nvs_open(ESPNOW_RX_MAC_NS, NVS_READONLY, &h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mac-ns open failed: %s", esp_err_to_name(err));
            return;
        }
    
        uint8_t cnt = 0;
        nvs_get_u8(h, ESPNOW_RX_MAC_KEY_COUNT, &cnt);
        ESP_LOGI(TAG, "=== ESP-NOW peer MACs (%u) ===", cnt);
    
        for (uint8_t i = 0; i < cnt; ++i) {
            char key[16];  snprintf(key, sizeof(key), ESPNOW_RX_MAC_KEY_FMT, i);
            uint8_t mac[6]; size_t len = sizeof(mac);
    
            err = nvs_get_blob(h, key, mac, &len);
            if (err == ESP_OK && len == 6) {
                ESP_LOGI(TAG, "  [%u] %02X:%02X:%02X:%02X:%02X:%02X",
                         i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            } else {
                ESP_LOGW(TAG, "  [%u] missing / wrong size (%s)", i, esp_err_to_name(err));
            }
        }
        nvs_close(h);
    }
    
    static void dump_lmks(void)
    {
        nvs_handle_t h;
        esp_err_t err = nvs_open(ESPNOW_LMK_NS, NVS_READONLY, &h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "lmk-ns open failed: %s", esp_err_to_name(err));
            return;
        }
    
        uint8_t cnt = 0;
        nvs_get_u8(h, ESPNOW_LMK_KEY_COUNT, &cnt);
        ESP_LOGI(TAG, "=== ESP-NOW LMKs (%u) ===", cnt);
    
        for (uint8_t i = 0; i < cnt; ++i) {
            char key[16];
            snprintf(key, sizeof(key), ESPNOW_LMK_KEY_FMT, i);
    
            uint8_t lmk[LMK_LEN];
            size_t  len = sizeof(lmk);
    
            err = nvs_get_blob(h, key, lmk, &len);
            if (err == ESP_OK && len == LMK_LEN) {
    
                // build a 32-char hex string in a tiny buffer
                char hex[LMK_LEN * 2 + 1];
                for (int j = 0; j < LMK_LEN; j++) {
                    sprintf(&hex[j * 2], "%02X", lmk[j]);
                }
                hex[LMK_LEN * 2] = '\0';
    
                ESP_LOGI(TAG, "  [%u] %s", i, hex);
            } else {
                ESP_LOGW(TAG, "  [%u] missing / wrong size (%s)",
                         i, esp_err_to_name(err));
            }
        }
        nvs_close(h);
    }
    
    void lcd_espnow_dump_nvs(void)
    {
        ESP_LOGI(TAG, "========================================");
        dump_names();
        dump_macs();
        dump_lmks();
        ESP_LOGI(TAG, "========================================");
    }
#endif