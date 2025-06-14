#include "core/lv_obj.h"
#include "polycast5_macros.h"

#include "core/lv_obj_pos.h"
#include "core/lv_obj_tree.h"

#include "widgets/label/lv_label.h"

#include "nvs.h"
#include "esp_log.h"

#include "lcd_wifi_funcs.h"
#include "wifi_task.h"
#include "wifi_funcs.h"

#include "lcd_funcs.h"
#include "lcd_task.h"


wifi_menu_t wifi_menu = {
    .options = {"Connect to network", "Send over Wi-Fi", "Monitor packets"},
    .size = 3,
    .index = 0,
    .cont = NULL,
};

static const char* TAG = "LCD_WIFI_FUNCS";

void lcd_wifi_setup_page(wifi_menu_t* menu)
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
        lv_obj_t* lbl = lv_obj_get_child(menu->btns[i], 0);
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

void lcd_wifi_update_menu(wifi_menu_t* menu)
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

void lcd_wifi_create_scan_list(wifi_scan_menu_t* menu)
{
	menu->size = 0;
	
	// Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 210, 68);
    
    // Format
    lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF); // Never draw bars
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 17);
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
	
	// Hide for now
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

static void lcd_wifi_update_scan_menu(wifi_scan_menu_t* menu)
{    
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

void lcd_wifi_scan_page(ui_menu_t* ui_menu, wifi_menu_t* wifi_menu, ui_btns_t* ui_btns)
{	
	static lv_obj_t* lbl_wait;
	static bool initialized = false;
	static bool scanned = false;
	
	// Do once
    if (!initialized) {        
        // Wait label
        lbl_wait = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_wait, "Scanning for networks...\nPlease wait...", user_secondary_color,
					 &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 20);
					 
		lv_timer_handler();
					 
		// Start scan
		xSemaphoreGive(xWifiStartScanSemaphore);
					 
		initialized = true;
    }

	// When networks have been scanned
    wifi_scan_t result;
    while (xQueueReceive(xWifiScanQueue, &result, 0) == pdPASS) {
		// Skip any blanks
		if ((result.ssid[0] == '\0') || (strlen((char*)result.ssid) == 0)) {
	        continue;
	    }
	    
	    // Once networks have been delete help text
	    if (!scanned) {
			lv_obj_delete(lbl_wait);
			scanned = true;
		}
		
		// Format SSID
        char buf[33];
        snprintf(buf, sizeof(buf), "%s", result.ssid); //, result.rssi, result.channel, result.auth
        
        // Add SSID as button
        wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size] = lv_list_add_btn(wifi_menu->scan_menu.main_list, NULL, buf);
        lv_obj_set_size(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 200, 30);

        // Style selected
        if (wifi_menu->scan_menu.size == wifi_menu->scan_menu.index) {
            lv_obj_add_style(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], &wifi_menu->scan_menu.sel_style, 0);
        }
        else {
            lv_obj_add_style(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], &wifi_menu->scan_menu.btn_style, 0);
        }

        // Create and format text label
        lv_obj_t* lbl = lv_obj_get_child(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        
        // Format buttons as container
	    wifi_menu->scan_menu.cont = lv_obj_get_parent(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size]);
	    lv_obj_set_flex_flow(wifi_menu->scan_menu.cont, LV_FLEX_FLOW_COLUMN);
	    lv_obj_set_flex_align(wifi_menu->scan_menu.cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	    lv_obj_set_style_pad_gap(wifi_menu->scan_menu.cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
        
        wifi_menu->scan_menu.size++;
    }
    
    // Scroll up
    if (scanned && ui_btns->up_btn == 1) {
		wifi_menu->scan_menu.index--;
		lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
	}
	// Scroll down
	else if (scanned && ui_btns->down_btn == 1) {
		wifi_menu->scan_menu.index++;
		lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
	}
	// Back
	else if (scanned && ui_btns->left_btn == 1) {
		// Reset
		initialized = false;
		scanned = false;
		wifi_menu->scan_menu.size = 0;
		wifi_menu->scan_menu.index = 0;
		lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
				
		// Clear children
		lv_obj_clean(wifi_menu->scan_menu.main_list);
		
		// Hide scan menu
		lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show Wi-Fi menu
		lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = WIFI_PAGE;
	}
}




