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

#define MAX_PASSWORD_LEN 32
#define NUM_CHAR_ROWS 4

wifi_menu_t wifi_menu = {
    .options = {"Connect to network", "Send over Wi-Fi", "Monitor packets", "Sync with PolyPlug"},
    .size = 4,
    .index = 0,
    .cont = NULL,
};

wifi_login_t selected_network = {0};

static const char* TAG = "LCD_WIFI_FUNCS";

// Character vars for user input
static char name_buf[MAX_PASSWORD_LEN + 1] = {0};
static const char* char_rows[NUM_CHAR_ROWS] = {
    "_ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "abcdefghijklmnopqrstuvwxyz",
    "0123456789",
    "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~"
};

void lcd_wifi_setup_page(wifi_menu_t *menu)
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
    lv_obj_set_flex_flow(menu->cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(menu->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
	
	// Hide for now
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_wifi_update_menu(wifi_menu_t *menu)
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

void lcd_wifi_create_scan_list(wifi_scan_menu_t *menu)
{
	menu->size = 0;
	
	// Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 210, 106); // h: 68
    
    // Format
    lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF); // Never draw bars
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0); // y: 17
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
	
	if (menu->size > 0) {
		menu->index = 0;
	}
	
	// Hide for now
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

static void lcd_wifi_update_scan_menu(wifi_scan_menu_t *menu)
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

void lcd_wifi_scan_page(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns)
{	
	static lv_obj_t *lbl_wait;
	static lv_obj_t *lbl_option;
	static bool locked[WIFI_MAX_NETWORKS];
	static bool initialized = false;
	static bool scanned = false;
	static bool scanning = false;
	
	static uint8_t bssids[WIFI_MAX_NETWORKS][6];
	
	// Do once
    if (!initialized) {        				 
		// Option label
        lbl_option = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_option, "or press down to scan", user_secondary_color,
					 &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -10);
					 		
		/* Add prev button */
        char buf[16];
        snprintf(buf, sizeof(buf), "Connect to last");
        
        // Create button
        wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size] = lv_list_add_btn(wifi_menu->scan_menu.main_list, NULL, buf);
        lv_obj_set_size(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 200, 30);

        // Style selected
        lv_obj_add_style(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], &wifi_menu->scan_menu.sel_style, 0);

        // Create and format text label
        lv_obj_t *lbl = lv_obj_get_child(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        
        // Format buttons as container
	    wifi_menu->scan_menu.cont = lv_obj_get_parent(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size]);
	    lv_obj_set_flex_flow(wifi_menu->scan_menu.cont, LV_FLEX_FLOW_COLUMN);
	    lv_obj_set_flex_align(wifi_menu->scan_menu.cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	    lv_obj_set_style_pad_gap(wifi_menu->scan_menu.cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
	    
	    wifi_menu->scan_menu.size++; // One larger
	    
		lv_timer_handler(); // Show
					 					 
		initialized = true;
    }

	// When networks have been scanned
    wifi_scan_t result;
    while (xQueueReceive(xWifiScanQueue, &result, 0) == pdPASS) {
		// Skip any blanks
		if ((result.ssid[0] == '\0') || (strlen((char*)result.ssid) == 0)) {
	        continue;
	    }
	    
	    // Once networks have been received: delete help text
	    if (!scanned) {
			lv_obj_delete(lbl_wait);
			lbl_wait = NULL;
			scanning = false;
			scanned = true;
		}
		
		// Check if != WIFI_AUTH_OPEN
		locked[wifi_menu->scan_menu.size] = (result.auth != 0) ? true : false; 
		// Copy BSSID
		memcpy(bssids[wifi_menu->scan_menu.size], result.bssid, sizeof(result.bssid));
		
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
        lv_obj_t *lbl = lv_obj_get_child(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        
        // Size one bigger now
        wifi_menu->scan_menu.size++;
    }
    
    // Scroll up
    if (scanned && ui_btns->up_btn == 1) {
		wifi_menu->scan_menu.index--;
		lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
	}
	// Scan requested
	else if (!scanned && ui_btns->down_btn == 1) {
		if (lbl_option) { // Delete if exists
			lv_obj_delete(lbl_option);
			lbl_option = NULL;
		}
		
		if (!scanning) {
			// Wait label
		    lbl_wait = lv_label_create(ACTIVE_SCR);
			lcd_format_label(lbl_wait, "Scanning for networks...\nPlease wait...", user_secondary_color,
						 &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 20);
							 
			// Start scan
			xSemaphoreGive(xWifiStartScanSemaphore);
			
			scanning = true;
		}
	}
	// Scroll down
	else if (scanned && ui_btns->down_btn == 1) {
		wifi_menu->scan_menu.index++;
		lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
	}
	// Back
	else if ((!scanning || scanned) && ui_btns->left_btn == 1) {
		if (lbl_option) { // Delete if exists
			lv_obj_delete(lbl_option);
			lbl_option = NULL;
		}
		
		// Reset
		initialized = false;
		scanned = false;
		for(int i = 0; i < WIFI_MAX_NETWORKS; i++) {
			locked[i] = false;
			memset(bssids[i], 0, sizeof(bssids[i]));
		}
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
	// If connecting to last known
	else if (ui_btns->right_btn == 1 && wifi_menu->scan_menu.index == 0 && !scanning) {
		if (lbl_option) { // Delete if exists
			lv_obj_delete(lbl_option);
			lbl_option = NULL;
		}
		
		selected_network = wifi_funcs_get_prev(); // Loads boot state saved network info
		selected_network.prev = true; // Connecting to previous
		
		if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) {
		    ESP_LOGE(TAG, "Failed: xWifiSelectedNetworkQueue previous_network");
		}
		
		// Reset
		initialized = false;
		scanned = false;
		for(int i = 0; i < wifi_menu->scan_menu.size; i++) {
			locked[i] = false;
			memset(bssids[i], 0, sizeof(bssids[i]));
		}
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
	// Network selected
	else if (scanned && ui_btns->right_btn == 1) {
		selected_network.prev = false; // Connecting to new
		
		// Copy over SSID
		lv_obj_t *btn = wifi_menu->scan_menu.btns[wifi_menu->scan_menu.index];
	    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
	    const char *ssid = lv_label_get_text(lbl);
	    strlcpy((char*)selected_network.ssid, ssid, sizeof(selected_network.ssid));
	    
	    // Copy BSSID
	    memcpy(selected_network.bssid, bssids[wifi_menu->scan_menu.index], sizeof(bssids[wifi_menu->scan_menu.index]));
		
		// If network requires password
	    if (locked[wifi_menu->scan_menu.index]) {
			// Reset
			initialized = false;
			scanned = false;
			for(int i = 0; i < wifi_menu->scan_menu.size; i++) {
			    locked[i] = false;
			    memset(bssids[i], 0, sizeof(bssids[i]));
			}
			wifi_menu->scan_menu.size = 0;
			wifi_menu->scan_menu.index = 0;
			lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
			
			// Clear children
			lv_obj_clean(wifi_menu->scan_menu.main_list);
			
			// Hide scan menu
			lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Switch pages
			ui_menu->page = WIFI_PASSWORD_PAGE;
		}
		// Else open network: go ahead and send
		else {
			selected_network.locked = false; // Doesn't require password
			
	    	if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) {
		        ESP_LOGE(TAG, "Failed: xWifiSelectedNetworkQueue SSID");
		    }
		    
		    // Reset
			initialized = false;
			scanned = false;
			for(int i = 0; i < wifi_menu->scan_menu.size; i++) {
				locked[i] = false;
				memset(bssids[i], 0, sizeof(bssids[i]));
			}
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
}

static void update_password_label_lcd(lv_obj_t *lbl_display, char cur_char, int cur_pos)
{
    char display[MAX_PASSWORD_LEN + 2];
    size_t len = cur_pos + 1;
    if (len > MAX_PASSWORD_LEN) len = MAX_PASSWORD_LEN;
    // copy existing
    memcpy(display, name_buf, cur_pos);
    // show current selection
    display[cur_pos] = cur_char;
    display[cur_pos + 1] = '\0';
    lv_label_set_text(lbl_display, display);
    lv_obj_align(lbl_display, LV_ALIGN_CENTER, 0, 30);
}

void lcd_wifi_get_password(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t  *ui_btns)
{
	// Statics
    static lv_obj_t *lbl_dirs, *lbl_user_in;
    static int cur_pos = 0;
    static int row_idx = 0; // Active row
    static int char_idx = 0; // Index within that row
    static bool initialized = false;
    static char cur_char; // Current character

	// Do once
    if (!initialized) {
		// Everything is zero'd out to start
        memset(name_buf, 0, sizeof name_buf);
        row_idx = 0;
        char_idx = 0;
        cur_char = char_rows[0][0]; // Start at 0, 0

		// Helper labels
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                         &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_dirs, "Enter Wi-Fi password: Press\n  home to cycle characters.", user_secondary_color,
                         &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -30);

        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
        
        initialized = true;
    }

    // Switch rows (ex A->a)
    if (ui_btns->back_btn) {
		 // Increment with wrap
        row_idx = (row_idx + 1) % NUM_CHAR_ROWS;
        
        // Update current char
        char_idx = 0;
        cur_char = char_rows[row_idx][char_idx];
        
        // Show to LCD
        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    }
    // Cycle specific character
    else if (ui_btns->up_btn || ui_btns->down_btn) {
		// Get length of selected row
        size_t row_len = strlen(char_rows[row_idx]);
        
        // Increment/decrement that row with wrap
        if (ui_btns->up_btn) {
			char_idx = (char_idx + 1) % row_len;
		}
        else if (ui_btns->down_btn) {
			char_idx = (char_idx + row_len - 1) % row_len;
		}
		
		// Update the current character
        cur_char = char_rows[row_idx][char_idx];
        
        // Show to LCD
        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    }
    // Move character position
    else if (ui_btns->right_btn) {
		// Save current char to name buffer
        name_buf[cur_pos] = cur_char;
        
        // Increment position
        if (cur_pos < MAX_PASSWORD_LEN) {
            cur_pos++;
            
			// Reset characters
            char_idx = 0;
            cur_char = char_rows[row_idx][0];
        }
        
        // Show to LCD
        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    }
    // Back
    else if (ui_btns->left_btn && cur_pos == 0) {
		// Delete objects
        lv_obj_del(lbl_user_in);
        lv_obj_del(lbl_dirs);
        
        // Reset statics
        lbl_user_in = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        initialized = false;

        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_PAGE;
        
        return;
    }
    // Backspace
    else if (ui_btns->left_btn) {
		// Save change to name buffer
        name_buf[cur_pos] = '\0';
        
        // Decrement position
	    if (cur_pos > 0) {
	        cur_pos--;
	    }

        // Reload cur_char from the new slot
        char target = name_buf[cur_pos] ? name_buf[cur_pos] : '_';
        // Search each row for that character
        for (row_idx = 0; row_idx < NUM_CHAR_ROWS; row_idx++) {
            const char *row = char_rows[row_idx]; // Get active row
            const char *p = strchr(row, target); // Scan row for character target
            if (p) {
                char_idx = (uint16_t)(p - row); // How many chars from row to reach p
                break;
            }
        }

        // Update cur_char
        cur_char = char_rows[row_idx][char_idx];

        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    }
    // Save
    else if (ui_btns->select_btn) {
		// Commit the current character
	    if (cur_pos < MAX_PASSWORD_LEN && name_buf[cur_pos] == '\0') {
	        name_buf[cur_pos++] = cur_char;
	    }
    
        name_buf[cur_pos] = '\0'; // Null-terminate
        
        // Send to Wi-Fi task
        selected_network.locked = true; // Requires password
	    strlcpy((char*)selected_network.password, name_buf, sizeof(selected_network.password));
	    if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) { // SSID was copied earlier
		    ESP_LOGE(TAG, "Failed: xWifiSelectedNetworkQueue PASSWORD");
		}

        // Delete objects
        lv_obj_del(lbl_user_in);
        lv_obj_del(lbl_dirs);
        
        // Reset statics
        lbl_user_in = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        initialized = false;

        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_PAGE;
        
        return;
    }
}





