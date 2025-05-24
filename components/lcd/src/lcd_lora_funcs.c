#include "core/lv_obj_pos.h"
#include "core/lv_obj_tree.h"
#include "esp_log.h"
#include "font/lv_symbol_def.h"

#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "nvs.h"

#include "lcd_lora_funcs.h"
#include "lora_task.h"

#include "lcd_funcs.h"
#include "lcd_task.h"

#include "espnow_task.h"
#include "portmacro.h"
#include "widgets/label/lv_label.h"

//#include "gpio_task.h"

lora_menu_t lora_menu = {
    .options = {"Add PolyPlug"},
    .keys = {},
    .size = 1,
    .index = 0,
    .cont = NULL,
};

static const char *submenu_options[] = {
    LV_SYMBOL_UPLOAD "\nSEND",
    LV_SYMBOL_LOOP "\nLOOP",
    LV_SYMBOL_HOME "\nPLAN",
    LV_SYMBOL_WARNING "\nAWAY",
    LV_SYMBOL_SETTINGS "\nEDIT",
    LV_SYMBOL_TRASH "\nDEL",
};

static const int submenu_count = sizeof(submenu_options)/sizeof(submenu_options[0]);

static const char* TAG = "LCD_LORA_FUNCS";

static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};
static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};

static bool lora_menu_overwrite = false;
static uint8_t lora_index_overwrite = 0;


void lcd_lora_setup_page(lora_menu_t *menu)
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

void lcd_lora_setup_subpage(lora_menu_t *menu)
{
	// Initialize submenu struct
    menu->submenu.size = submenu_count;
    menu->submenu.index = 0;
    for (int i = 0; i < submenu_count && i < MAX_LORA_OPTIONS; i++) {
        menu->submenu.options[i] = (char*)submenu_options[i];
    }
    
    // Create container
	menu->submenu.cont = lv_obj_create(ACTIVE_SCR);
	
	// Format
	lv_obj_set_size(menu->submenu.cont, 210, 106);
	lv_obj_center(menu->submenu.cont);
	lv_obj_set_style_bg_color(menu->submenu.cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(menu->submenu.cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_scrollbar_mode(menu->submenu.cont, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_scroll_dir(menu->submenu.cont, LV_DIR_VER);
	
	// Set flow
	lv_obj_set_flex_flow(menu->submenu.cont, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(menu->submenu.cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	
	// Set gap
	lv_obj_set_style_pad_gap(menu->submenu.cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Prepare styles 
    // Normal button style
    lv_style_init(&menu->submenu.btn_style);
    lv_style_set_radius(&menu->submenu.btn_style, 8);
    lv_style_set_bg_color(&menu->submenu.btn_style, user_primary_color);
    lv_style_set_border_width(&menu->submenu.btn_style, 2);
    lv_style_set_border_color(&menu->submenu.btn_style, user_secondary_color);
    lv_style_set_border_side(&menu->submenu.btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&menu->submenu.btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->submenu.btn_style, user_secondary_color);
    lv_style_set_text_align(&menu->submenu.btn_style, LV_TEXT_ALIGN_CENTER);

    // Selected button style
    lv_style_init(&menu->submenu.sel_style);
    lv_style_set_radius(&menu->submenu.sel_style, 8);
    lv_style_set_bg_color(&menu->submenu.sel_style, user_secondary_color);
    lv_style_set_border_width(&menu->submenu.sel_style, 2);
    lv_style_set_border_color(&menu->submenu.sel_style, user_secondary_color);
    lv_style_set_border_side(&menu->submenu.sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&menu->submenu.sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->submenu.sel_style, user_primary_color);
    lv_style_set_text_align(&menu->submenu.sel_style, LV_TEXT_ALIGN_CENTER);

    // Create button per option
	for (int i = 0; i < menu->submenu.size; i++) {
	    menu->submenu.btns[i] = lv_btn_create(menu->submenu.cont);
    	lv_obj_set_size(menu->submenu.btns[i], 58, 50);
    	
	    // Add style
	    if (i == menu->submenu.index)
	    	lv_obj_add_style(menu->submenu.btns[i], &menu->submenu.sel_style, 0);
	    else
	    	lv_obj_add_style(menu->submenu.btns[i], &menu->submenu.btn_style, 0);
	
	    // Create child label
	    lv_obj_t *lbl = lv_label_create(menu->submenu.btns[i]);
	    lv_label_set_text(lbl, menu->submenu.options[i]);
	    
	    // Format
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
	}
	
	// Hide for now
	lv_obj_add_flag(menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
}

void lcd_lora_update_menu(lora_menu_t *menu)
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

void lcd_lora_update_submenu(lora_menu_t *menu)
{    
	// Reveal
    lv_obj_remove_flag(menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

    // Wrap index
	if (menu->submenu.index >= menu->submenu.size) {
		menu->submenu.index = 0;
	}
	else if (menu->submenu.index < 0) {
		menu->submenu.index = menu->submenu.size - 1;
	}

    // Reset every button to unselected
    for (int i = 0; i < menu->submenu.size; ++i) {
        lv_obj_remove_style(menu->submenu.btns[i], &menu->submenu.sel_style, 0);
        lv_obj_add_style(menu->submenu.btns[i], &menu->submenu.btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->submenu.btns[menu->submenu.index], &menu->submenu.btn_style, 0);
    lv_obj_add_style(menu->submenu.btns[menu->submenu.index], &menu->submenu.sel_style, 0);
    
    // Enable scrolling if list gets too long
    //lv_obj_scroll_to_view(menu->submenu.btns[menu->submenu.index], LV_ANIM_ON); // LV_ANIM_OFF
}

void lcd_lora_create_enc_key(ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
	lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
	lv_obj_t *lbl_key_ins = lv_label_create(ACTIVE_SCR);
	lcd_format_label(lbl_key_ins, "1. Bring near desired PolyPlug.\n2. Press the top right button\non PolyPlug.\n3. Confirm LED is showing\ngreen on PolyPlug.\n4. On this device, hit the\nright arrow to confirm.", user_secondary_color,
                         &lv_font_montserrat_14, LV_ALIGN_CENTER, 6, 6);
                    
    while (1) {
		lv_timer_handler();
		
		// User hit cancel
        if (xSemaphoreTake(xLeftButtonSemaphore, 10) == pdTRUE) {
            
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_del(lbl_key_ins);
            
            // Go back
            return;
        }
        // User hit confirm
        else if (xSemaphoreTake(xRightButtonSemaphore, 10) == pdTRUE) {
            // Generate encryption key
            xSemaphoreGive(xGenerateEncKeySemaphore);
            
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            
            // Prompt to enter name
            ui_menu->page = LORA_NAME_PAGE;
            
            lv_obj_del(lbl_key_ins);
            
            // Go back
            return;
        }
    }
}

void lcd_lora_create_custom_name(ui_menu_t *ui_menu, lora_menu_t *lora_menu, ui_btns_t *ui_btns)
{
	static uint8_t received_enc_key_nvs[ENC_KEY_LEN];
	
    // Declare statics
    static int cur_pos = 0; // User position
    static char cur_char = '_';
    static lv_obj_t *lbl_dirs = NULL;
    static lv_obj_t *lbl_chars = NULL;
    static lv_obj_t *lbl_user_in = NULL;
    char display[MAX_CUSTOM_NAME_LEN + 2];
    
    // Create initial label
    if (!lbl_user_in) {
		
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                         &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_dirs, "  Enter plug name\nwith arrow buttons:", user_secondary_color,
                         &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, -30);
        
        lbl_chars = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_chars, "(Up to 12 characters)", user_secondary_color,
                         &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
    }

    // Take user input
    // If up, iterate up
    if (ui_btns->up_btn) {
		// Wrap
        if (cur_char == '_') {
			cur_char = 'A';
		}
        else if (cur_char == 'Z') {
			cur_char = '_';
		}
		// Else iterate 1 char
        else {
			cur_char = (char)(cur_char + 1);
		}
		
		// Save to array
        name_buf[cur_pos] = cur_char;
    }
    // If down, iterate down
    else if (ui_btns->down_btn) {
		// Wrap
        if (cur_char == '_') {
			cur_char = 'Z';
		}
        else if (cur_char == 'A') {
			cur_char = '_';
		}
		// Else iterate down 1 char
        else {
			cur_char = (char)(cur_char - 1);
		}
		
		// Save to array
        name_buf[cur_pos] = cur_char;
    }
    // If left pressed and at start
    else if (ui_btns->left_btn && cur_pos == 0) {
		// Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = NULL;
	    lbl_dirs  = NULL;
	    cur_pos  = 0;
	    cur_char = '_';
	    memset(name_buf, 0, sizeof name_buf);
	    
 		ui_menu->page = LORA_PAGE;
		return;
    }
    // If left and not at start
    else if (ui_btns->left_btn) {
        // Clear the current slot
	    name_buf[cur_pos] = '\0';
	
	    // De-increment left
	    if (cur_pos > 0) {
	        cur_pos--;
	    }
	
	    // Reload cur_char from the new slot
	    cur_char = name_buf[cur_pos] ? name_buf[cur_pos] : '_';
    }
    // If right
    else if (ui_btns->right_btn) {
		// Handle case where up/down wasn't pressed
        name_buf[cur_pos] = cur_char;
        
        // If not yet at end
        if (cur_pos < MAX_CUSTOM_NAME_LEN - 1) {
            cur_pos++;
            cur_char = '_';
        }
    }
    // If save button pressed
    else if (ui_btns->back_btn) {
		// Save final
        name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
        memcpy(saved_name, name_buf, MAX_CUSTOM_NAME_LEN + 1);
        ESP_LOGI(TAG, "%s", saved_name);
        
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = NULL;
	    lbl_dirs = NULL;
	    cur_pos = 0;
	    cur_char = '_';
	    memset(name_buf, 0, sizeof name_buf);

		// Update options
		// If overwriting an existing as a rename
		if (lora_menu_overwrite) {
			// lora_menu->index is passed as edit_idx:
			// Release old string then reallocate
			free(lora_menu->options[lora_index_overwrite]);
			lora_menu->options[lora_index_overwrite] = strdup(saved_name);

			// Persist to NVS
			lcd_lora_menu_nvs_save(lora_menu, LORA_OPTIONS_NS, LORA_OPTIONS_KEY_COUNT, LORA_OPTIONS_KEY_FMT);

			// Update the button’s label in-place
			lv_obj_t *btn = lora_menu->btns[lora_index_overwrite];
			lv_obj_t *child_lbl = lv_obj_get_child(btn, 0);
			lv_label_set_text(child_lbl, lora_menu->options[lora_index_overwrite]);

			// Clean up
			lora_menu_overwrite = false;
		}
		// Else adding a whole new remote
		else {
			lora_menu->size++;

			// Save to options, then to NVS
			char *name_copy = strdup(saved_name);
			lora_menu->options[lora_menu->size - 1] = name_copy;
			lcd_lora_menu_nvs_save(lora_menu, LORA_OPTIONS_NS, LORA_OPTIONS_KEY_COUNT, LORA_OPTIONS_KEY_FMT);
			
			// Get shared encryption key and do the same under the same index
			if (xQueueReceive(xSendEncKeyQueueNVS, received_enc_key_nvs, portMAX_DELAY) == pdPASS) {
				// Allocate a fresh buffer for this entry
		        uint8_t *slot = malloc(ENC_KEY_LEN);
		        if (!slot) {
		            ESP_LOGE(TAG, "Out of memory allocating key");
		            return;
		        }
		        memcpy(slot, received_enc_key_nvs, ENC_KEY_LEN);
		        
		        // Save to keys at next available position
		        lora_menu->keys[lora_menu->size - 1] = slot;
		        
		        //ESP_LOGI(TAG, "Key saved at slot %d:", lora_menu->size - 1);
				//ESP_LOG_BUFFER_HEX("SAVED IN QUEUE", lora_menu->keys[lora_menu->size - 1], ENC_KEY_LEN);
				
				lcd_lora_key_nvs_save(lora_menu, LORA_ENC_NS, LORA_ENC_KEY_COUNT, LORA_ENC_KEY_FMT);
			}

			// Create new button for new option
			lora_menu->btns[lora_menu->size - 1] = lv_list_add_btn(lora_menu->main_list, NULL, lora_menu->options[lora_menu->size - 1]);
			lv_obj_set_size(lora_menu->btns[lora_menu->size - 1], 200, 30);
			lv_obj_add_style(lora_menu->btns[lora_menu->size - 1],
							 &lora_menu->btn_style, 0);

			// Create and format text label
			lv_obj_t *lbl = lv_obj_get_child(lora_menu->btns[lora_menu->size - 1], 0);
			lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
			lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
		}
		
		// Switch to previous page
		ui_menu->page = LORA_PAGE;
        return;
    }

    // Build and show the text
    for (int i = 0; i < MAX_CUSTOM_NAME_LEN; i++) {
		if (i < cur_pos)
        	display[i] = name_buf[i] ? name_buf[i] : '_';
        else
        	display[i] = name_buf[i] ? name_buf[i] : ' ';
    }
    display[cur_pos] = cur_char;
    display[MAX_CUSTOM_NAME_LEN + 1] = '\0';

    lv_label_set_text(lbl_user_in, display);
}

void lcd_lora_subpage_selected(ui_menu_t *ui_menu, lora_menu_t *lora_menu, ui_btns_t *ui_btns) 
{
	
	// Scroll right
	if (ui_btns->right_btn == 1) {
		// Update selection
		lora_menu->submenu.index++;
		lcd_lora_update_submenu(lora_menu);
	}
	// Exit
	else if (ui_btns->left_btn == 1 && lora_menu->submenu.index == 0) {
		// Hide cont
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Show LoRa list
		lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Go back
		ui_menu->page = LORA_PAGE;
	}
	// Scroll left
	else if (ui_btns->left_btn == 1) {
		// Update selection
		lora_menu->submenu.index--;
		lcd_lora_update_submenu(lora_menu);
	}
	// Scroll up
	else if (ui_btns->up_btn == 1 && lora_menu->submenu.index > 2) {
		// Update selection
		lora_menu->submenu.index -= 3;
		lcd_lora_update_submenu(lora_menu);
	}
	// Select send
	else if (ui_btns->up_btn == 1 && lora_menu->submenu.index == 0) {
		xQueueSend(xReceiveEncKeyQueue, lora_menu->keys[lora_menu->index], portMAX_DELAY);
		xQueueSend(xReceiveEncIndexQueue, &lora_menu->submenu.index, portMAX_DELAY);
		ESP_LOG_BUFFER_HEX("SENDING WITH KEY", lora_menu->keys[lora_menu->index], ENC_KEY_LEN);
	}
	// Scroll down
	else if (ui_btns->down_btn == 1 && lora_menu->submenu.index < 3) {
		// Update selection
		lora_menu->submenu.index += 3;
		lcd_lora_update_submenu(lora_menu);
	}
	// Select other
	else if (ui_btns->up_btn == 1 || ui_btns->down_btn == 1) {
		// Hide submenu
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Go to subpage options page
		ui_menu->page = LORA_OPTIONS_SUBPAGE;
	}
}

void lcd_lora_subpage_option_selected(ui_menu_t *ui_menu, lora_menu_t *lora_menu, ui_btns_t *ui_btns)
{
	static lv_obj_t *lbl_subpage_ins_1 = NULL;
	static lv_obj_t *lbl_selected_icon = NULL;
	
	static bool update_once = false;
	
	// Create once
	if (!lbl_subpage_ins_1) {
		lbl_subpage_ins_1 = lv_label_create(ACTIVE_SCR);
	    lcd_format_label(lbl_subpage_ins_1, "", user_secondary_color,
				 &lv_font_montserrat_18, LV_ALIGN_CENTER, -30, 20); // +y = down, +x = right
		
		lbl_selected_icon = lv_label_create(ACTIVE_SCR);		 
		lcd_format_label(lbl_selected_icon, "", user_secondary_color,
				 &lv_font_montserrat_16, LV_ALIGN_CENTER, -30, 60);
	                     
	}
	

	// Loop was selected
	if (lora_menu->submenu.index == 1) {
		static uint8_t selected_index = 1;
		
		if (!update_once) {
			lv_label_set_text(lbl_subpage_ins_1,  "ON time:\n\nOFF time:");
			lv_label_set_text(lbl_selected_icon, LV_SYMBOL_PLAY);
			
			lv_obj_align(lbl_subpage_ins_1, LV_ALIGN_TOP_MID, -15, 20);
			lv_obj_align(lbl_selected_icon, LV_ALIGN_TOP_MID, -75, 63);
			
			update_once = true;
		}
		
		// Move up
		if (ui_btns->up_btn == 1 && selected_index == 1) {
			lv_obj_set_y(lbl_selected_icon, 21);
			
			selected_index = 0;
		}
		// Move down
		else if (ui_btns->down_btn == 1 && selected_index == 0) {
			lv_obj_set_y(lbl_selected_icon, 63);
			
			selected_index = 1;
		}
		// Wrap
		else if (ui_btns->up_btn == 1 && selected_index == 0) {
			lv_obj_set_y(lbl_selected_icon, 63);
			
			selected_index = 1;
		}
		// Go back
		else if (ui_btns->down_btn == 1) {
			// Show LoRa submenu cont
			lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
			
			// Reset objects
			lv_obj_delete(lbl_subpage_ins_1);
			lv_obj_delete(lbl_selected_icon);
			lbl_subpage_ins_1 = NULL;
			lbl_selected_icon = NULL;
			
			// Refresh statics 
			update_once = false;
			selected_index = 1;
			
			// Go back
			ui_menu->page = LORA_SUBPAGE;
			
		}
	}
}


esp_err_t lcd_lora_menu_nvs_save(const lora_menu_t *menu, const char* ns, const char* count, const char* fmt)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK)
    	return err;

    // menu->options[0] is default "Add New"
    // If menu->size == 1 there are no user names, otherwise there are menu->size - 1 names
    uint8_t user_cnt = (menu->size > 1) ? menu->size - 1 : 0;
    err = nvs_set_u8(h, count, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
    	goto out;

	// Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt; ++i) {
        char key[16];
        snprintf(key, sizeof(key), fmt, i);
        
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

esp_err_t lcd_lora_key_nvs_save(const lora_menu_t *menu, const char* ns, const char* count, const char* fmt)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK)
    	return err;

    // menu->options[0] is default "Add New"
    // If menu->size == 1 there are no user names, otherwise there are menu->size - 1 names
    uint8_t user_cnt = (menu->size > 1) ? menu->size - 1 : 0;
    err = nvs_set_u8(h, count, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
    	goto out;

	// Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt; ++i) {
        char key[16];
        snprintf(key, sizeof(key), fmt, i);
        
        // Store the key string at each key starting at index 1 to match user options
        err = nvs_set_blob(h, key, menu->keys[i + 1], ENC_KEY_LEN);
        
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

esp_err_t lcd_lora_menu_nvs_load(lora_menu_t *menu, const char* ns, const char* count, const char* fmt)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK)
    	return err;

	// Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, count, &user_cnt);
    if (err != ESP_OK) {
		nvs_close(h);
		return err;
	}

    menu->size = 1; // Don't change first option
    menu->index = 0;

	// Loop through all keys
    for (uint8_t i = 0; i < user_cnt; ++i) {
		
        char key[16];
        snprintf(key, sizeof(key), fmt, i);
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
		if (menu->size >= MAX_LORA_OPTIONS) {
		    free(buf);
		    break;
		}
        menu->options[menu->size++] = buf;
    }
    
    // Close NVS
    nvs_close(h);
    
    return ESP_OK;
}

esp_err_t lcd_lora_key_nvs_load(lora_menu_t *menu, const char* ns, const char* count, const char* fmt)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK)
    	return err;

	// Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, count, &user_cnt);
    if (err != ESP_OK) {
		nvs_close(h);
		return err;
	}

    menu->size = 1; // Don't change first option
    menu->index = 0;

	// Loop through all keys
    for (uint8_t i = 0; i < user_cnt; ++i) {
        
        char key[16];
        snprintf(key, sizeof(key), fmt, i);
        
        // Read exactly ENC_KEY_LEN bytes
        size_t blob_len = ENC_KEY_LEN;
        
        // First check existence & size
        err = nvs_get_blob(h, key, NULL, &blob_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK || blob_len != ENC_KEY_LEN) {
            break;
        }

        uint8_t *buf = malloc(ENC_KEY_LEN);
        if (!buf) {
            err = ESP_ERR_NO_MEM;
            break;
        }

        err = nvs_get_blob(h, key, buf, &blob_len);
        if (err != ESP_OK) {
            free(buf);
            break;
        }

		// Update menu struct
		if (menu->size >= MAX_LORA_OPTIONS) {
		    free(buf);
		    break;
		}
		menu->keys[menu->size++] = buf;
    }
    
    // Close NVS
    nvs_close(h);
    
    return err;
}
