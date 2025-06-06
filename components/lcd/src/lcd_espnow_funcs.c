#include "core/lv_obj.h"
#include "lcd_lora_funcs.h"
#include "lcd_espnow_funcs.h"

#include "lcd_funcs.h"
#include "lcd_task.h"

#include "espnow_task.h"
#include "misc/lv_area.h"

#define RX_MAC_IN_SEL_COLOR lv_palette_main(LV_PALETTE_RED)

espnow_menu_t espnow_menu = {
    .options = {"Add ESP32"},
    //.keys = {},
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
    lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF); // Never draw bars
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF);
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
	}
	else if (menu->index < 0) {
		menu->index = menu->size - 1;
	}
	
	if (menu->size > 1) {
		menu->index = 1;
	}
	
	// Create button for each option
    for (int i = 0; i < menu->size; i++) {

        menu->btns[i] = lv_list_add_btn(menu->main_list, NULL, menu->options[i]);
        lv_obj_set_size(menu->btns[i], 200, 30);

        // Style selected
        if (i == menu->index) {
            lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
        }
        else {
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
	}
	else if (menu->index < 0) {
		menu->index = menu->size - 1;
	}

    // Reset every button to unselected
    for (int i = 0; i < menu->size; i++) {
        lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
        lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);
    
    // Enable scrolling if list gets too long
    lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
}

void lcd_espnow_get_rx_mac(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns)
{
    // Statics
    static uint8_t mac_bytes[6]; // 6 bytes of the MAC
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
        for (int i = 0; i < 12; i++) {
            lbl_sel_digit[i] = lv_label_create(container);
            
            // Start all digits at 0
            lcd_format_label(lbl_sel_digit[i], "0", user_secondary_color,
					 &lv_font_montserrat_20, LV_ALIGN_LEFT_MID, (i * 17) - 6, 0);

            // Color selected
            if (i == (int)digit_index) {
                lv_obj_set_style_text_color(lbl_sel_digit[i], lv_palette_main(LV_PALETTE_RED), 0);
            }
            else {
                lv_obj_set_style_text_color(lbl_sel_digit[i], user_secondary_color, 0);
            }
        }

        // Helper text at the bottom
        lbl_enter_mac = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_enter_mac, "Enter receiver MAC:", user_secondary_color,
					 &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -56);
			
		// Instruction text at the top	 
		lbl_how_to = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_how_to, "polycast5.com/blogs\n      /how/get-mac", user_secondary_color,
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
        }
        else {
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
    }
    // Decrement digit down
    else if (ui_btns->down_btn) {
        int byte_idx = digit_index / 2; // Byte index that's being edited
        int nibble_pos = digit_index % 2; // 0 = high nibble, 1 = low nibble

        // Extract current nibble
        uint8_t cur_byte = mac_bytes[byte_idx]; // Select byte
        uint8_t high_n = (cur_byte >> 4) & 0x0F;
        uint8_t low_n = (cur_byte >> 0) & 0x0F;

        if (nibble_pos == 0) {
            // Decrement high nibble with wrap
            high_n = (high_n == 0 ? 0x0F : high_n - 1);
        }
        else {
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
    }
    // Move selection left
    else if (ui_btns->left_btn && digit_index > 0) {
        // De-style old digit
        lv_obj_set_style_text_color(lbl_sel_digit[digit_index], user_secondary_color, 0);
        
        // Decrement
        digit_index--;
        
        // Style new digit
        lv_obj_set_style_text_color(lbl_sel_digit[digit_index], RX_MAC_IN_SEL_COLOR, 0);
    }
    // Go back
    else if (ui_btns->left_btn) {
        // Clean all
		for (int i = 0; i < 12; i++) {
			lv_obj_del(lbl_sel_digit[i]);
			lbl_sel_digit[i] = NULL;
		}

		lv_obj_del(lbl_enter_mac);
		lv_obj_del(lbl_how_to);
		lv_obj_del(container);

		lbl_enter_mac = NULL;
		lbl_how_to = NULL;
		container = NULL;
		
		// Reset selected digit
		digit_index = 0;
		
		// Show espnow_menu
		lv_obj_remove_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Go back
		ui_menu->page = ESPNOW_PAGE;
    }
    // Move selection right
    else if (ui_btns->right_btn && digit_index < 11) {
        // De-style old digit
	    lv_obj_set_style_text_color(lbl_sel_digit[digit_index], user_secondary_color, 0);
	        
	    // Increment
	    digit_index++;
	        
	    // Style new digit
	    lv_obj_set_style_text_color(lbl_sel_digit[digit_index], RX_MAC_IN_SEL_COLOR, 0);
	}
    // Confirm
    else if (ui_btns->right_btn) {
        // Copy the 6 bytes into espnow_menu->rx_mac[] for later use.
        //for (int b = 0; b < 6; b++) {
            //espnow_menu->rx_mac[b] = mac_bytes[b];
        //}

        //lcd_espnow_save_rx_mac_to_nvs(espnow_menu->rx_mac);
		
		// Clean all
		for (int i = 0; i < 12; i++) {
			lv_obj_del(lbl_sel_digit[i]);
			lbl_sel_digit[i] = NULL;
		}

		lv_obj_del(lbl_enter_mac);
		lv_obj_del(lbl_how_to);
		lv_obj_del(container);

		lbl_enter_mac = NULL;
		lbl_how_to = NULL;
		container = NULL;
		
		// Reset selected digit
		digit_index = 0;

		// Go to next
		ui_menu->page = ESPNOW_PAGE;
	}
}