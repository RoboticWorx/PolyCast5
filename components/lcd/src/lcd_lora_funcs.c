#include "core/lv_obj.h"
#include "misc/lv_area.h"
#include "polycast5_macros.h"

#include "core/lv_obj_pos.h"
#include "core/lv_obj_tree.h"
#include "font/lv_symbol_def.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

#include "misc/lv_timer.h"
#include "portmacro.h"
#include "widgets/label/lv_label.h"

#include "nvs.h"
#include "esp_log.h"

#include "lcd_lora_funcs.h"
#include "lora_task.h"
#include "lora_funcs.h"
#include "gpio_task.h"
#include "lcd_utils.h"

#include "espnow_task.h"

//#include "gpio_task.h"

#define LORA_OPTIONS_NS "loram_data" // NVS namespace
#define LORA_OPTIONS_KEY_COUNT "loram_count" // u8: number of option
#define LORA_OPTIONS_KEY_FMT "loram_op%02d" // loram_op00, loram_op01 ...

#define LORA_ENC_NS "lorae_data"
#define LORA_ENC_KEY_COUNT "lorae_count"
#define LORA_ENC_KEY_FMT "lorae_op%02d"

#define LORA_PLAN_SEL_INS "Select day(s)"

lora_menu_t lora_menu = {
    .options = {"Add PolyPlug"},
    .keys = {},
    .size = 1,
    .index = 0,
    .cont = NULL,
};

lora_plan_menu_t lora_plan_menu = {0};

static lora_cmd_t lora_cmd = {
    .key = {0},
    .index = -1,
    .instr = {0}
};

static const char *submenu_options[] = {
    LV_SYMBOL_UPLOAD "\nSEND",
    LV_SYMBOL_LOOP "\nLOOP",
    LV_SYMBOL_HOME "\nPLAN",
    LV_SYMBOL_WARNING "\nAWAY",
    LV_SYMBOL_SETTINGS "\nEDIT",
    LV_SYMBOL_TRASH "\nDEL",
};

static const char* TAG = "LCD_LORA_FUNCS";

static const int submenu_count = sizeof(submenu_options)/sizeof(submenu_options[0]);

static char plan_selected_days[8]; // Up to 7 days + NULL

static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};
static bool lora_menu_overwrite = false;


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
	// Create receipt label (check/x) for send confirmation
	menu->submenu.lbl_receipt = lv_label_create(ACTIVE_SCR);
	lcd_format_label(menu->submenu.lbl_receipt, "", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, 6, 2);
	
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
	lv_obj_add_flag(menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
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

void lcd_lora_update_submenu(lora_menu_t *menu)
{    
	// Hide and reset receipt label
	lv_obj_add_flag(menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(menu->submenu.lbl_receipt, "");
		
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
    for (int i = 0; i < menu->submenu.size; i++) {
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
	lcd_format_label(lbl_key_ins, "1. Bring near desired PolyPlug.\n2. Press the top right button\non the PolyPlug.\n3. Confirm LED is showing\nblue on the PolyPlug.\n4. On this device, hit the\nright arrow to confirm.", user_secondary_color,
                         &lv_font_montserrat_14, LV_ALIGN_CENTER, 6, 6);
                    
    while (1) {
		lv_timer_handler();
		
		// User hit cancel
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {
            
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_del(lbl_key_ins);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Hide right arrow
			lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Show LoRa menu
			lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            
            // Go back
            return;
        }
        // User hit confirm
        else if (xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {
            // Generate encryption key
            xSemaphoreGive(xLoraGenerateEncKeySemaphore);
            
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			
			lv_obj_del(lbl_key_ins);
			
			lcd_clear_pending_inputs = true; // Clear any false inputs
			
			// Show right arrow
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Prompt to enter name
            ui_menu->page = LORA_NAME_PAGE;

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

void lcd_lora_create_custom_name(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
	static uint8_t received_enc_key_nvs[LORA_ENC_KEY_LEN];
	
    // Declare statics
    static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};
    static int cur_pos = 0; // User position
    static char cur_char = '_';
    static lv_obj_t *lbl_dirs = NULL;
    static lv_obj_t *lbl_chars = NULL;
    static lv_obj_t *lbl_user_in = NULL;
    
    // Create initial label
    if (!lbl_user_in) {
		
		// If renaming, autofill what was there previously
        if (lora_menu_overwrite) {
            // Copy the old name into buffer
            strncpy(name_buf, lora_menu->options[lora_menu->index], MAX_CUSTOM_NAME_LEN);

            // Place cursor at the end
            cur_pos = strlen(name_buf);
            
            // Start with '_'
            cur_char = '_';
        }
        else { // Else blank slate
            memset(name_buf, 0, sizeof name_buf);
            cur_pos = 0;
            cur_char = '_';
        }
		
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                         &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_dirs, "  Enter plug name\nwith arrow buttons:", user_secondary_color,
                         &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, -30);
                         
        if (lora_menu_overwrite)
        	lv_label_set_text(lbl_dirs, "Enter new plug name\n with arrow buttons:");
        
        lbl_chars = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_chars, "(Up to 12 characters)", user_secondary_color,
                         &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
                         
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
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
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
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
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    }
    // Can back out if at start and renaming
    else if (ui_btns->left_btn && cur_pos == 0 && lora_menu_overwrite) {
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
	    
	    lora_menu_overwrite = false; // Switch back
		
		// Reset submenu to first index
		lora_menu->submenu.index = 0;
		lcd_lora_update_submenu(lora_menu);
	    
 		ui_menu->page = LORA_SUBPAGE;
		return;
    }
    // Go home or power off if ranaming
    else if ((ui_btns->home_btn || ui_btns->pwr_btn) && lora_menu_overwrite) {
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
	    
	    lora_menu_overwrite = false; // Switch back
		
		// Reset submenu to first index
		lora_menu->submenu.index = 0;
		lcd_lora_update_submenu(lora_menu);
		
		// Hide
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
	    
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
    // If left and not at start
    else if (ui_btns->left_btn && cur_pos != 0) {
        // Clear the current slot
	    name_buf[cur_pos] = '\0';
	
	    // De-increment left
	    if (cur_pos > 0) {
	        cur_pos--;
	    }
	
	    // Reload cur_char from the new slot
	    cur_char = name_buf[cur_pos] ? name_buf[cur_pos] : '_';
	    
	    update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
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
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    }
    // If save button pressed
    else if (ui_btns->select_btn) {
		// Save final
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
        lbl_user_in = NULL;
	    lbl_dirs = NULL;
	    cur_pos = 0;
	    cur_char = '_';
	    memset(name_buf, 0, sizeof name_buf);

		// Update options
		// If overwriting an existing as a rename
		if (lora_menu_overwrite) {
			// lora_menu->index is edit_idx
			// Release old string then reallocate
			free(lora_menu->options[lora_menu->index]);
			lora_menu->options[lora_menu->index] = strdup(saved_name);

			// Persist to NVS
			lcd_lora_menu_nvs_save(lora_menu);

			// Update the button’s label in-place
			lv_obj_t *btn = lora_menu->btns[lora_menu->index];
			lv_obj_t *child_lbl = lv_obj_get_child(btn, 0);
			lv_label_set_text(child_lbl, lora_menu->options[lora_menu->index]);

			// Reset flag
			lora_menu_overwrite = false;
			
			// Reset submenu to first index
			lora_menu->submenu.index = 0;
			lcd_lora_update_submenu(lora_menu);
			lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN); // Hide
		}
		// Else adding a whole new remote
		else {
			lora_menu->size++;

			// Save to options, then to NVS
			char *name_copy = strdup(saved_name);
			lora_menu->options[lora_menu->size - 1] = name_copy;
			lcd_lora_menu_nvs_save(lora_menu);
			
			// Get shared encryption key and do the same under the same index
			if (xQueueReceive(xEspSendEncKeyQueueNVS, received_enc_key_nvs, portMAX_DELAY) == pdPASS) {
				// Allocate a fresh buffer for this entry
		        uint8_t *slot = malloc(LORA_ENC_KEY_LEN);
		        if (!slot) {
		            ESP_LOGE(TAG, "Out of memory allocating LORA_ENC_KEY_LEN key");
		            return;
		        }
		        memcpy(slot, received_enc_key_nvs, LORA_ENC_KEY_LEN);
		        
		        // Save to keys at next available position
		        lora_menu->keys[lora_menu->size - 1] = slot;
		        
		        #ifdef POLYCAST5_DEBUG
				    ESP_LOGI(TAG, "Key saved at slot %d:", lora_menu->size - 1);
					ESP_LOG_BUFFER_HEX("SAVED IN QUEUE", lora_menu->keys[lora_menu->size - 1], LORA_ENC_KEY_LEN);
				#endif
		        
				lcd_lora_key_nvs_save(lora_menu);
			}

			// Create new button for new option
			lora_menu->btns[lora_menu->size - 1] = lv_list_add_btn(lora_menu->main_list, NULL, lora_menu->options[lora_menu->size - 1]);
			lv_obj_set_size(lora_menu->btns[lora_menu->size - 1], 200, 30);
			lv_obj_add_style(lora_menu->btns[lora_menu->size - 1], &lora_menu->btn_style, 0);

			// Create and format text label
			lv_obj_t *lbl = lv_obj_get_child(lora_menu->btns[lora_menu->size - 1], 0);
			lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
			lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
		}
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Show LoRa list
		lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch to previous page
		ui_menu->page = LORA_PAGE;
        return;
    }
}

void lcd_lora_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu) 
{	
	// If received a valid receipt from the receiver
	if (xSemaphoreTake(xLoraReceiptValidSemaphore, 0) == pdTRUE) {
		// Show check in top left corner
		lv_obj_remove_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lora_menu->submenu.lbl_receipt, LV_SYMBOL_OK);
	}
	
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
		
		// Hide and reset receipt label
		lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Show LoRa list
		lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Go back
		ui_menu->page = LORA_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Reset selection
		lora_menu->submenu.index = 0;
		lcd_lora_update_submenu(lora_menu);
		
		// Hide cont
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Hide and reset receipt label
		lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// Scroll left
	else if (ui_btns->left_btn == 1) {
		// Update selection
		lora_menu->submenu.index--;
		lcd_lora_update_submenu(lora_menu);
	}
	// Scroll up
	else if (ui_btns->up_btn == 1) {
		// Update selection
		if (lora_menu->submenu.index > 2) {
			lora_menu->submenu.index -= 3;
		}
		else if (lora_menu->submenu.index < 3) {
			lora_menu->submenu.index += 3;
		}
		lcd_lora_update_submenu(lora_menu);
	}
	// Send selected
	else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 0) {
		lora_cmd.index = lora_menu->submenu.index;
		memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_ENC_KEY_LEN);
		xQueueSend(xLoraSendEncQueue, &lora_cmd, 0);
		
		#ifdef POLYCAST5_DEBUG
		    //ESP_LOG_BUFFER_HEX("SENDING WITH KEY", lora_menu->keys[lora_menu->index], LORA_ENC_KEY_LEN);
		#endif
		
		// Reset receipt label
		lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
	}
	// Delete selected
	else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 5) {
		
		// Get user entry to remove
	    int del_idx = lora_menu->index;     
	    
	    // Can't be "Add PolyPlug"     
	    if (del_idx == 0)
	    	return;
	    
	    // Free any heap buffers allocated for that slot
	    free(lora_menu->options[del_idx]); // Name string
	    free(lora_menu->keys[del_idx]); // Key blob
	    lv_obj_del(lora_menu->btns[del_idx]); // LVGL list button
	
	    // Shift everything above it down one
	    for (int i = del_idx; i < lora_menu->size - 1; i++) {
			// Change each to the one after
	        lora_menu->options[i] = lora_menu->options[i + 1];
	        lora_menu->keys[i] = lora_menu->keys[i + 1];
	        lora_menu->btns[i] = lora_menu->btns[i + 1];
	
	        // Update the label inside the button
	        lv_obj_t *lbl = lv_obj_get_child(lora_menu->btns[i], 0);
	        lv_label_set_text(lbl, lora_menu->options[i]);
	    }
	
		// List is now one shorter
	    lora_menu->size--;
	    
	    // Null out dangling index
		lora_menu->options[lora_menu->size] = NULL;
		lora_menu->keys[lora_menu->size] = NULL;
		lora_menu->btns[lora_menu->size] = NULL;
	    
	    // Adjust if was last
	    if (lora_menu->index >= lora_menu->size)
	        lora_menu->index = lora_menu->size-1;
	        
	    // Remove entry from NVS
	    lcd_lora_menu_nvs_delete(del_idx);
		lcd_lora_key_nvs_delete(del_idx);
	
	    // Refresh the list UI
	    lcd_lora_update_menu(lora_menu);
	    
	    // Reset submenu index
	    lora_menu->submenu.index = 0;
	    // Refresh the submenu UI
	    lcd_lora_update_submenu(lora_menu);
	    
	    // Go back to LoRa page
	    lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
    	lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
    	ui_menu->page = LORA_PAGE;
	}
	// Scroll down
	else if (ui_btns->down_btn == 1) {
		// Update selection
		if (lora_menu->submenu.index < 3) {
			lora_menu->submenu.index += 3;
		}
		else if (lora_menu->submenu.index > 2) {
			lora_menu->submenu.index -= 3;
		}
		lcd_lora_update_submenu(lora_menu);
	}
	// Loop selected
	else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 1) {
		// Hide and reset receipt label
		lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
		
		// Hide submenu
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Go to subpage options page
		ui_menu->page = LORA_LOOP_SUBPAGE;
	}
	// Away selected
	else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 3) {
		// Hide and reset receipt label
		lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
		
		// Hide submenu
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Go to subpage options page
		ui_menu->page = LORA_AWAY_SUBPAGE;
	}
	// Plan selected
	else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 2) {
		// Hide and reset receipt label
		lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
		
		// Hide submenu
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Reveal the container and label
		lv_obj_remove_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
		
		// Default index 0
		lora_plan_menu->plan_index = 0;
		
		// Update plan menu
		lcd_lora_update_plan_menu(lora_plan_menu);
		
		// Go to subpage options page
		ui_menu->page = LORA_PLAN_SUBPAGE;
	}
	// Edit selected
	else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 4) {
		// Hide and reset receipt label
		lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
		
		// Hide submenu
		lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Don’t allow renaming the first index
	    if (lora_menu->index == 0) {
	        return;
	    }
	
	    // Trigger overwrite 
	    lora_menu_overwrite = true;
	
	    // Prompt rename
	    ui_menu->page = LORA_NAME_PAGE;
	}
}

void lcd_lora_loop_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
	#define LOOP_TIME_OPT_COUNT (sizeof(time_opts)/sizeof(time_opts[0]))
	#define LOOP_Y_SEL_POS 43
	
	// Create statics
	static lv_obj_t *lbl_subpage_times = NULL;
	static lv_obj_t *lbl_subpage_ins = NULL;
	static lv_obj_t *lbl_selected_icon = NULL;
	static lv_obj_t *lbl_top_time = NULL;
	static lv_obj_t *lbl_bot_time = NULL;
	
	static uint8_t selected_index = 1;
	static int on_idx = 0;
	static int off_idx = 0;
	
	static const char *time_opts[] = {
		"1m", "3m", "5m", "15m",
		"30m", "45m", "1h", "2h",
		"3h", "4h", "6h", "8h",
		"12h", "16h", "18h", "24h"
	};
	
	// Create once
	if (!lbl_subpage_times) {
		// Create and format text labels
		lbl_subpage_times = lv_label_create(ACTIVE_SCR);
	    lcd_format_label(lbl_subpage_times, "ON time:\nOFF time:", user_secondary_color,
				 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, -15, 20); // +y = down, +x = right
				 
		lbl_subpage_ins = lv_label_create(ACTIVE_SCR);		 
		lcd_format_label(lbl_subpage_ins, "- Right/left to adjust time.\n- Press select to confirm.\n- Down to exit.", user_secondary_color,
				 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -15);
		
		lbl_selected_icon = lv_label_create(ACTIVE_SCR);		 
		lcd_format_label(lbl_selected_icon, LV_SYMBOL_PLAY, user_secondary_color,
			 &lv_font_montserrat_12, LV_ALIGN_TOP_MID, -75, LOOP_Y_SEL_POS);
			 
		lbl_top_time = lv_label_create(ACTIVE_SCR);		 
		lcd_format_label(lbl_top_time, "", user_secondary_color,
			 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 55, 20);
			 
		lbl_bot_time = lv_label_create(ACTIVE_SCR);		 
		lcd_format_label(lbl_bot_time, "", user_secondary_color, 
			 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 55, LOOP_Y_SEL_POS - 2);
		
		char buf[4];
		snprintf(buf, sizeof(buf), "%s", time_opts[0]);
		lv_label_set_text(lbl_top_time, buf);
		lv_label_set_text(lbl_bot_time, buf);
	}
	
	// Move up
	if (ui_btns->up_btn == 1) {
		if (selected_index == 1) {
			// Move pointer up
			lv_obj_set_y(lbl_selected_icon, 22);
			selected_index = 0;
		}
		else if (selected_index == 0) {
			// Move pointer up
			lv_obj_set_y(lbl_selected_icon, LOOP_Y_SEL_POS);
			selected_index = 1;
		}
	}
	// Move down
	else if (ui_btns->down_btn == 1 && selected_index == 0) {
		// Move pointer down
		lv_obj_set_y(lbl_selected_icon, LOOP_Y_SEL_POS);
			
		selected_index = 1;
	}
	// Shift time of selected right
	else if (ui_btns->right_btn == 1) {
		// Changing top time
		if (selected_index == 0) {
			on_idx = (on_idx  + 1) % LOOP_TIME_OPT_COUNT;
			char buf[4];
			snprintf(buf, sizeof(buf), "%s", time_opts[on_idx]);
			lv_label_set_text(lbl_top_time, buf);
		}
		// Changing bot time
		else {
			off_idx = (off_idx + 1) % LOOP_TIME_OPT_COUNT;
			char buf[4];
			snprintf(buf, sizeof(buf), "%s", time_opts[off_idx]);
			lv_label_set_text(lbl_bot_time, buf);
		}
	}
	// Shift time of selected left
	else if (ui_btns->left_btn == 1) {
		// Changing top time
		if (selected_index == 0) {
			on_idx = (on_idx - 1 + LOOP_TIME_OPT_COUNT) % LOOP_TIME_OPT_COUNT;
			char buf[4];
			snprintf(buf, sizeof(buf), "%s", time_opts[on_idx]);
			lv_label_set_text(lbl_top_time, buf);
		}
		// Changing bot time
		else {
			off_idx = (off_idx - 1 + LOOP_TIME_OPT_COUNT) % LOOP_TIME_OPT_COUNT;
			char buf[4];
			snprintf(buf, sizeof(buf), "%s", time_opts[off_idx]);
			lv_label_set_text(lbl_bot_time, buf);
		}
	}
	// Confirm
	else if (ui_btns->select_btn == 1) {				 
		// Reset objects
		lv_obj_delete(lbl_subpage_times);
		lv_obj_delete(lbl_selected_icon);
		lv_obj_delete(lbl_top_time);
		lv_obj_delete(lbl_bot_time);
		lbl_subpage_times = NULL;
		lbl_selected_icon = NULL;
		lbl_top_time = NULL;
		lbl_bot_time = NULL;
		
		// Send the data to lora_task
		lora_cmd.index = lora_menu->submenu.index;
		memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_ENC_KEY_LEN);
		snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "on %s off %s", time_opts[on_idx], time_opts[off_idx]);
		xQueueSend(xLoraSendEncQueue, &lora_cmd, 0);

		// Confirmation text
		lcd_format_label(lbl_subpage_ins, "Sending to PolyPlug...", user_secondary_color,
				 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(500));
		
		// Reset confirmation lbl
		lv_obj_delete(lbl_subpage_ins);
		lbl_subpage_ins = NULL;
		
		// Refresh statics 
		selected_index = 1;
		on_idx = 0;
		off_idx = 0;
		
		// Show LoRa submenu cont
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
			
		// Go back
		ui_menu->page = LORA_SUBPAGE;
	}
	// Go back
	else if (ui_btns->down_btn == 1) {
		// Show LoRa submenu cont
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Reset objects
		lv_obj_delete(lbl_subpage_times);
		lv_obj_delete(lbl_selected_icon);
		lv_obj_delete(lbl_subpage_ins);
		lv_obj_delete(lbl_top_time);
		lv_obj_delete(lbl_bot_time);
		lbl_subpage_times = NULL;
		lbl_selected_icon = NULL;
		lbl_subpage_ins = NULL;
		lbl_top_time = NULL;
		lbl_bot_time = NULL;
		
		// Refresh statics 
		selected_index = 1;
		on_idx = 0;
		off_idx = 0;
			
		// Go back
		ui_menu->page = LORA_SUBPAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {			
		// Reset objects
		lv_obj_delete(lbl_subpage_times);
		lv_obj_delete(lbl_selected_icon);
		lv_obj_delete(lbl_subpage_ins);
		lv_obj_delete(lbl_top_time);
		lv_obj_delete(lbl_bot_time);
		lbl_subpage_times = NULL;
		lbl_selected_icon = NULL;
		lbl_subpage_ins = NULL;
		lbl_top_time = NULL;
		lbl_bot_time = NULL;
		
		// Refresh statics 
		selected_index = 1;
		on_idx = 0;
		off_idx = 0;
			
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_lora_setup_plan_page(ui_menu_t *ui_menu, lora_plan_menu_t *lora_plan_menu)
{
    static const char *options[LORA_PLAN_SUBMENU_COUNT] = {
        LV_SYMBOL_CLOSE "\nMON",
        LV_SYMBOL_CLOSE "\nTUE",
        LV_SYMBOL_CLOSE "\nWED",
        LV_SYMBOL_CLOSE "\nTHU",
        LV_SYMBOL_CLOSE "\nFRI",
        LV_SYMBOL_CLOSE "\nSAT",
        LV_SYMBOL_CLOSE "\nSUN",
        LV_SYMBOL_TRASH "\nREM",
    };

    // Assign options
    for (int i = 0; i < LORA_PLAN_SUBMENU_COUNT; i++) {
        lora_plan_menu->plan_options[i] = options[i];
    }

    // Create instruction label
    lora_plan_menu->lbl_days_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS, user_secondary_color,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 10);
    
    /* Initialize plan submenu */
    // Create container
    lora_plan_menu->plan_cont = lv_obj_create(ACTIVE_SCR);
    
    // Format
    lv_obj_set_size(lora_plan_menu->plan_cont, 230, 92);
    lv_obj_align(lora_plan_menu->plan_cont, LV_ALIGN_CENTER, 0, 7);
    lv_obj_set_style_bg_color(lora_plan_menu->plan_cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(lora_plan_menu->plan_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(lora_plan_menu->plan_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(lora_plan_menu->plan_cont, LV_DIR_VER);
    
    // Set flow
    lv_obj_set_flex_flow(lora_plan_menu->plan_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(lora_plan_menu->plan_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Set padding
    lv_obj_set_style_pad_gap(lora_plan_menu->plan_cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Prepare styles 
    // Normal button style
    lv_style_init(&lora_plan_menu->plan_btn_style);
    lv_style_set_radius(&lora_plan_menu->plan_btn_style, 8);
    lv_style_set_bg_color(&lora_plan_menu->plan_btn_style, user_primary_color);
    lv_style_set_border_width(&lora_plan_menu->plan_btn_style, 2);
    lv_style_set_border_color(&lora_plan_menu->plan_btn_style, user_secondary_color);
    lv_style_set_border_side(&lora_plan_menu->plan_btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&lora_plan_menu->plan_btn_style, &lv_font_montserrat_14);
    lv_style_set_text_color(&lora_plan_menu->plan_btn_style, user_secondary_color);
    lv_style_set_text_align(&lora_plan_menu->plan_btn_style, LV_TEXT_ALIGN_CENTER);

    // Selected button style
    lv_style_init(&lora_plan_menu->plan_sel_style);
    lv_style_set_radius(&lora_plan_menu->plan_sel_style, 8);
    lv_style_set_bg_color(&lora_plan_menu->plan_sel_style, user_secondary_color);
    lv_style_set_border_width(&lora_plan_menu->plan_sel_style, 2);
    lv_style_set_border_color(&lora_plan_menu->plan_sel_style, user_secondary_color);
    lv_style_set_border_side(&lora_plan_menu->plan_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&lora_plan_menu->plan_sel_style, &lv_font_montserrat_14);
    lv_style_set_text_color(&lora_plan_menu->plan_sel_style, user_primary_color);
    lv_style_set_text_align(&lora_plan_menu->plan_sel_style, LV_TEXT_ALIGN_CENTER);

    // Create button per option
    for (int i = 0; i < LORA_PLAN_SUBMENU_COUNT; i++) {
        lora_plan_menu->plan_btns[i] = lv_btn_create(lora_plan_menu->plan_cont);
        lv_obj_set_size(lora_plan_menu->plan_btns[i], 48, 43);
        
        // Add style
        if (i == lora_plan_menu->plan_index) {
            lv_obj_add_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_btn_style, 0);
        }
        else {
            lv_obj_add_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_sel_style, 0);
        }
    
        // Create child label
        lv_obj_t *lbl = lv_label_create(lora_plan_menu->plan_btns[i]);
        lv_label_set_text(lbl, lora_plan_menu->plan_options[i]);
        
        // Format
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    }
    
    // Put arrows after container
    lv_obj_move_foreground(ui_menu->arrow_top);
    lv_obj_move_foreground(ui_menu->arrow_bot);
    lv_obj_move_foreground(ui_menu->arrow_left);
    lv_obj_move_foreground(ui_menu->arrow_right);
    
    // Update plan menu
	lcd_lora_update_plan_menu(lora_plan_menu);
    
    // Hide the container and label
    lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
}

void lcd_lora_plan_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu)
{
	#define PLAN_COLUMNS 4 // 4x2 grid to fit 8 buttons
	
	// Statics for toggling
	static const char *days[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
	static bool days_selected[7] = {false};
	
	// Confirm
	if (ui_btns->right_btn == 1 && lora_plan_menu->plan_index == 7) {
		// Save selected days into global
		int pos = 0;
	    for (int i = 0; i < 7; i++) {
			// If day selected
	        if (days_selected[i]) {
	            // '1' + i gives '1' for Monday (i==0), '2' for Tuesday, ... '7' for Sunday
	            plan_selected_days[pos++] = '1' + i;
	        }
	    }
	    plan_selected_days[pos] = '\0'; // Terminate
		
		#ifdef POLYCAST5_DEBUG
		    ESP_LOGI(TAG, "Days selected = '%s'", plan_selected_days);
	    #endif
		
		// Reset all labels and days
		for (int i = 0; i < 7; i++) {
			days_selected[i] = false;
			lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
			char buf[20];
			snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
			lv_label_set_text(lbl, buf);
		}
		
		lora_plan_menu->plan_index = 0;
		
		// Reset text
		lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
		
		// Hide plan items
		lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
		
		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = LORA_PLAN_CONFIRM_SUBPAGE;
	}
	// Go right
	else if (ui_btns->right_btn == 1) {		
		// Update selection right
		lora_plan_menu->plan_index = (lora_plan_menu->plan_index + 1) % LORA_PLAN_SUBMENU_COUNT;
		
		// Update plan menu
		lcd_lora_update_plan_menu(lora_plan_menu);
	}
	// Back
	else if (ui_btns->left_btn == 1 && lora_plan_menu->plan_index == 0) {
		// Reset all labels and days
		for (int i = 0; i < 7; i++) {
			days_selected[i] = false;
			lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
			char buf[20];
			snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
			lv_label_set_text(lbl, buf);
		}
		
		// Reset text
		lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
		
		// Hide plan items
		lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
		
		// Show LoRa submenu
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = LORA_SUBPAGE;
	}
	// Go left
	else if (ui_btns->left_btn == 1) {
		// Update selection left
		lora_plan_menu->plan_index = (lora_plan_menu->plan_index - 1 + LORA_PLAN_SUBMENU_COUNT) % LORA_PLAN_SUBMENU_COUNT;
		
		// Update plan menu
		lcd_lora_update_plan_menu(lora_plan_menu);
	}
	// Go up
	else if (ui_btns->up_btn == 1) {
		// Update selection up (for 4 columns: if > 3 subtract 4, else add 4)
		if (lora_plan_menu->plan_index > (PLAN_COLUMNS - 1)) {
			lora_plan_menu->plan_index -= PLAN_COLUMNS;
		}
		else {
			lora_plan_menu->plan_index += PLAN_COLUMNS;
		}
		
		// Wrap if needed
		if (lora_plan_menu->plan_index >= LORA_PLAN_SUBMENU_COUNT) {
			lora_plan_menu->plan_index -= LORA_PLAN_SUBMENU_COUNT;
		}
		
		// Update plan menu
		lcd_lora_update_plan_menu(lora_plan_menu);
	}
	// Go down
	else if (ui_btns->down_btn == 1) {
		// Update selection down (for 4 columns: if < 4 add 4, else subtract 4)
		if (lora_plan_menu->plan_index < PLAN_COLUMNS) {
			lora_plan_menu->plan_index += PLAN_COLUMNS;
		}
		else {
			lora_plan_menu->plan_index -= PLAN_COLUMNS;
		}
		
		// Wrap if needed
		if (lora_plan_menu->plan_index < 0) {
			lora_plan_menu->plan_index += LORA_PLAN_SUBMENU_COUNT;
		}
		
		// Update plan menu
		lcd_lora_update_plan_menu(lora_plan_menu);
    }
    // Select option
	else if (ui_btns->select_btn == 1) {
		// If a day, toggle it and update symbol
		if (lora_plan_menu->plan_index < 7) {
			days_selected[lora_plan_menu->plan_index] = !days_selected[lora_plan_menu->plan_index]; // Toggle day
			lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[lora_plan_menu->plan_index], 0);
			
			// Format new text into label
			char buf[20];
			snprintf(buf, sizeof(buf), "%s\n%s", days_selected[lora_plan_menu->plan_index] ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, days[lora_plan_menu->plan_index]);
			lv_label_set_text(lbl, buf);
			
			// Set confirmation text
			lv_label_set_text(lora_plan_menu->lbl_days_ins, "Hold right to confirm");
		}
		// Else "REM" selected (remove)
		else {
			// Reset all labels and days
			for (int i = 0; i < 7; i++) {
				days_selected[i] = false;
				lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
				char buf[20];
				snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
				lv_label_set_text(lbl, buf);
			}
			
			// Reset text
			lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
			
			// Hide and go back
			lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
			
			// Show LoRa submenu
			lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

			// Switch pages
			ui_menu->page = LORA_SUBPAGE;
		}
	}
	// Home or power off
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Reset all labels and days
		for (int i = 0; i < 7; i++) {
			days_selected[i] = false;
			lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
			char buf[20];
			snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
			lv_label_set_text(lbl, buf);
		}
		
		// Reset text
		lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
			
		// Hide plan items
		lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
		
		// Go home or power off
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu);
	}
}

void lcd_lora_plan_confirm_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_plan_menu_t *lora_plan_menu)
{
	static bool init = false;
	static lv_obj_t *lbl_ins_top = NULL;
	static lv_obj_t *lbl_ins_bot = NULL;
	static lv_obj_t *lbl_conf = NULL;
	
	if (!init) {
		lbl_ins_top = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins_top, "Please make sure the\n targeted PolyPlug is\n  connected to Wi-Fi\n  before proceeding!", user_secondary_color,
	    		&lv_font_montserrat_14, LV_ALIGN_CENTER, 0, -25);
	    		
	    lbl_ins_bot = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins_bot, "If not, please do so in the\n   Wi-Fi menu via 'sync'.", user_secondary_color,
	    		&lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 27);
	    		
	    lbl_conf = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_conf, "Press select to confirm", user_secondary_color,
	    		&lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -5);
	    
	    init = true;
	}
    
	// Back
    if (ui_btns->left_btn == 1) {
		// Reset objects
		lv_obj_del(lbl_ins_top);
		lv_obj_del(lbl_ins_bot);
		lv_obj_del(lbl_conf);
		
		// Reset statics
		lbl_ins_top = lbl_ins_bot = lbl_conf = NULL;
		init = false;
		
		// Show arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Reveal the plan container and label
		lv_obj_remove_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
		
		lcd_lora_update_plan_menu(lora_plan_menu); // Refresh
		
		// Go back
		ui_menu->page = LORA_PLAN_SUBPAGE;
	}
	// Home or power off
    else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Reset objects
		lv_obj_del(lbl_ins_top);
		lv_obj_del(lbl_ins_bot);
		lv_obj_del(lbl_conf);
		
		// Reset statics
		lbl_ins_top = lbl_ins_bot = lbl_conf = NULL;
		init = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// Confirm
    else if (ui_btns->select_btn == 1) {
		// Reset objects
		lv_obj_del(lbl_ins_top);
		lv_obj_del(lbl_ins_bot);
		lv_obj_del(lbl_conf);
		
		// Reset statics
		lbl_ins_top = lbl_ins_bot = lbl_conf = NULL;
		init = false;
		
		// Show arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = LORA_PLAN_TIMES_SUBPAGE;
	}
}

/* lcd_lora_plan_times_subpage HELPERS */
// Helper to update the full time range label using individual char labels
static void update_time_label(lv_obj_t *time_labels[], char *start_time, char *end_time) {
    const char *full_str = "00:00:00-00:00:00";  // Template for positions
    for (int i = 0; i < 17; i++) {  // Full string length without null
        char ch[2] = { full_str[i], '\0' };  // Single char
        if (i < 8) {
            ch[0] = start_time[i];  // Override with actual start
        } else if (i == 8) {
            ch[0] = '-';
        } else {
            ch[0] = end_time[i - 9];  // Override with actual end (skip "-")
        }

        lv_label_set_text(time_labels[i], ch);
    }
}
// Helper to get digit value at position (0-5) in time string
static int get_digit(char *time_str, uint8_t pos) {
    uint8_t str_pos[] = {0, 1, 3, 4, 6, 7}; // Positions in "HH:MM:SS"
    return time_str[str_pos[pos]] - '0';
}
// Helper to set digit value at position
static void set_digit(char *time_str, uint8_t pos, int val) {
    uint8_t str_pos[] = {0, 1, 3, 4, 6, 7};
    time_str[str_pos[pos]] = '0' + val;
}

void lcd_lora_plan_times_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu)
{
	#define PLAN_TIME_DIGITS 12 // HHMMSS (ignoring :) for both times
	#define PLAN_TIME_STR_LEN 9 // "HH:MM:SS\0"
	#define PLAN_TIME_DIGIT_WIDTH 12 // Fixed width per digit/colon (adjust based on font metrics)
	#define PLAN_TIME_X_BASE -96

	// Create statics
	static lv_obj_t *lbl_subpage_times = NULL;
	static lv_obj_t *lbl_selected_icon = NULL;
	static lv_obj_t *lbl_ins = NULL;
	static lv_obj_t *time_labels[17]; // 8 for start ("HH:MM:SS") + 1 for "-" + 8 for end

	static int cursor_x_offsets[12];
	static uint8_t selected_digit = 0; // 0-11: positions across both HHMMSS
	static char start_time[PLAN_TIME_STR_LEN] = "00:00:00";
	static char end_time[PLAN_TIME_STR_LEN] = "00:00:00";
	
	static bool init = false;

	// Do once
	if (!init) {
		// Create and format text labels
		lbl_subpage_times = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_subpage_times, "ON time - OFF time", user_secondary_color,
				&lv_font_montserrat_20, LV_ALIGN_CENTER, 0, -25);
				
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "* Time is 24h format *", user_secondary_color,
				&lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -15);

		// Create individual char label for each digit
		for (int i = 0; i < 17; i++) {
			time_labels[i] = lv_label_create(ACTIVE_SCR);
			lcd_format_label(time_labels[i], "0", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_CENTER, PLAN_TIME_X_BASE + (i * PLAN_TIME_DIGIT_WIDTH), 0);
			
			// Set fixed width to prevent shifting
			lv_obj_set_width(time_labels[i], PLAN_TIME_DIGIT_WIDTH);
			lv_obj_set_style_text_align(time_labels[i], LV_TEXT_ALIGN_CENTER, 0);
		}
		
		update_time_label(time_labels, start_time, end_time); // Initial update

		// Tracking arrow
		lbl_selected_icon = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_selected_icon, LV_SYMBOL_EJECT, user_secondary_color,
				&lv_font_montserrat_14, LV_ALIGN_CENTER, cursor_x_offsets[0], 20);
		
		// Calculate digit offsets
		cursor_x_offsets[0] = PLAN_TIME_X_BASE + (0 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[1] = PLAN_TIME_X_BASE + (1 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[2] = PLAN_TIME_X_BASE + (3 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[3] = PLAN_TIME_X_BASE + (4 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[4] = PLAN_TIME_X_BASE + (6 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[5] = PLAN_TIME_X_BASE + (7 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[6] = PLAN_TIME_X_BASE + (9 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[7] = PLAN_TIME_X_BASE + (10 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[8] = PLAN_TIME_X_BASE + (12 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[9] = PLAN_TIME_X_BASE + (13 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[10] = PLAN_TIME_X_BASE + (15 * PLAN_TIME_DIGIT_WIDTH);
		cursor_x_offsets[11] = PLAN_TIME_X_BASE + (16 * PLAN_TIME_DIGIT_WIDTH);
		
		// Set cursor X position
		lv_obj_set_x(lbl_selected_icon, cursor_x_offsets[selected_digit]);
		
		init = true;
	}
	
	// Confirm
	if (ui_btns->right_btn == 1 && selected_digit == 11) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "Confirmed: start_time = '%s', end_time = '%s'", start_time, end_time);
		#endif
		
		// Send the data to lora_task
		lora_cmd.index = lora_menu->submenu.index;
		memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_ENC_KEY_LEN);
		// Remove colons
		int h1, m1, s1, h2, m2, s2;
		sscanf(start_time, "%2d:%2d:%2d", &h1,&m1,&s1);
		sscanf(end_time, "%2d:%2d:%2d", &h2,&m2,&s2);
		snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "d %s o %02d%02d%02d f %02d%02d%02d",
				plan_selected_days, h1, m1, s1, h2, m2, s2);
		xQueueSend(xLoraSendEncQueue, &lora_cmd, 0);
		
		// Reset objects
		lv_obj_delete(lbl_subpage_times);
		lv_obj_delete(lbl_selected_icon);
		lv_obj_delete(lbl_ins);
		for (int i = 0; i < 17; i++) {
			lv_obj_delete(time_labels[i]);
		}

		// Reset statics
		lbl_subpage_times = lbl_selected_icon = lbl_ins = NULL;
		selected_digit = 0;
		strcpy(start_time, "00:00:00");
		strcpy(end_time, "00:00:00");
		init = false;
		
		// Confirmation text
		lv_obj_t *lbl_send_conf = lv_label_create(ACTIVE_SCR); // Create and format label
		lcd_format_label(lbl_send_conf, "Sending to PolyPlug...", user_secondary_color,
				 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1000ms
		lv_obj_del(lbl_send_conf); // Delete label
		lcd_clear_pending_inputs = true;

		// Show LoRa submenu cont
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

		// Go back
		ui_menu->page = LORA_SUBPAGE;
	}
	// Go back
	else if (ui_btns->left_btn == 1 && selected_digit == 0) {
		// Reset objects
		lv_obj_delete(lbl_subpage_times);
		lv_obj_delete(lbl_selected_icon);
		lv_obj_delete(lbl_ins);
		for (int i = 0; i < 17; i++) {
			lv_obj_delete(time_labels[i]);
		}

		// Reset statics
		lbl_subpage_times = lbl_selected_icon = lbl_ins = NULL;
		selected_digit = 0;
		strcpy(start_time, "00:00:00");
		strcpy(end_time, "00:00:00");
		init = false;

		// Reveal the plan container and label
		lv_obj_remove_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);

		lcd_lora_update_plan_menu(lora_plan_menu);

		// Go back
		ui_menu->page = LORA_PLAN_SUBPAGE;
	}
	// Digit right
	else if (ui_btns->right_btn == 1) {
		// Increment with wrap
		selected_digit = (selected_digit + 1) % PLAN_TIME_DIGITS;
		
		// Set cursor X position
		lv_obj_set_x(lbl_selected_icon, cursor_x_offsets[selected_digit]);
		
		// Update
		update_time_label(time_labels, start_time, end_time);
	}
	// Digit left
	else if (ui_btns->left_btn == 1) {
		// Decrement with wrap
		selected_digit = (selected_digit + PLAN_TIME_DIGITS - 1) % PLAN_TIME_DIGITS;
		
		// Set cursor X position
		lv_obj_set_x(lbl_selected_icon, cursor_x_offsets[selected_digit]);
		
		// Update
		update_time_label(time_labels, start_time, end_time);
	}
	// Digit up
	else if (ui_btns->up_btn == 1) {
		// Select time string and adjusted pos (0-5)
		char *current_time_str = (selected_digit < 6) ? start_time : end_time; // Which half
		uint8_t adj_pos = selected_digit % 6;
		int digit_val = get_digit(current_time_str, adj_pos);
		int max_val = 9;
		int min_val = 0;

		// Range limits based on position
		if (adj_pos == 0) { // Tens of hours (0-2)
			max_val = 2;
		}
		else if (adj_pos == 1) { // Hours
			max_val = (get_digit(current_time_str, 0) == 2) ? 3 : 9;
		}
		else if (adj_pos == 2 || adj_pos == 4) { // Tens of min/sec (0-5)
			max_val = 5;
		}

		// Increment with wrap
		digit_val = (digit_val + 1 > max_val) ? min_val : digit_val + 1;
		
		// Update
		set_digit(current_time_str, adj_pos, digit_val);
		update_time_label(time_labels, start_time, end_time);
	}
	// Digit down
	else if (ui_btns->down_btn == 1) {
		// Select time string and adjusted pos (0-5)
		char *current_time_str = (selected_digit < 6) ? start_time : end_time; // Which half
		uint8_t adj_pos = selected_digit % 6;
		int digit_val = get_digit(current_time_str, adj_pos);
		int max_val = 9;
		int min_val = 0;

		// Range limits based on position
		if (adj_pos == 0) { // Tens of hours (0-2)
			max_val = 2;
		}
		else if (adj_pos == 1) { // Hours
			max_val = (get_digit(current_time_str, 0) == 2) ? 3 : 9;
		}
		else if (adj_pos == 2 || adj_pos == 4) { // Tens of min/sec (0-5)
			max_val = 5;
		}

		// Decrement with wrap
		digit_val = (digit_val - 1 < min_val) ? max_val : digit_val - 1;
		
		// Update
		set_digit(current_time_str, adj_pos, digit_val);
		update_time_label(time_labels, start_time, end_time);
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Reset objects
		lv_obj_delete(lbl_subpage_times);
		lv_obj_delete(lbl_selected_icon);
		lv_obj_delete(lbl_ins);
		for (int i = 0; i < 17; i++) {
			lv_obj_delete(time_labels[i]);
		}

		// Reset statics
		lbl_subpage_times = lbl_selected_icon = lbl_ins = NULL;
		selected_digit = 0;
		strcpy(start_time, "00:00:00");
		strcpy(end_time, "00:00:00");
		init = false;

		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_lora_update_plan_menu(lora_plan_menu_t *lora_plan_menu)
{
	/* Update container */
	// Wrap index
	if (lora_plan_menu->plan_index >= LORA_PLAN_SUBMENU_COUNT) {
		lora_plan_menu->plan_index = 0;
	}
	else if (lora_plan_menu->plan_index < 0) {
		lora_plan_menu->plan_index = LORA_PLAN_SUBMENU_COUNT - 1;
	}

	// Reset every button to unselected
	for (int i = 0; i < LORA_PLAN_SUBMENU_COUNT; i++) {
	    lv_obj_remove_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_sel_style, 0);
	    lv_obj_add_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_btn_style, 0);
	}

	// Highlight only the current index
	lv_obj_remove_style(lora_plan_menu->plan_btns[lora_plan_menu->plan_index], &lora_plan_menu->plan_btn_style, 0);
	lv_obj_add_style(lora_plan_menu->plan_btns[lora_plan_menu->plan_index], &lora_plan_menu->plan_sel_style, 0);
}

void lcd_lora_away_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
	// Create statics
	static lora_menu_t *away_menu;
	static bool do_once = false;
	
	if (!do_once) {		
		// Allocate for away_menu
	    away_menu = malloc(sizeof(lora_menu_t));
	    if (!away_menu) {
	        ESP_LOGE(TAG, "Failed to allocate away_menu");
	        return;
	    }
	    
	    // Zero out the struct
	    memset(away_menu, 0, sizeof(*away_menu));

		// Fill entries
	    away_menu->size = 6;
	    away_menu->index = 0;
	    away_menu->options[0] = "Add Custom";
		away_menu->options[1] = "10-60m ON/OFF";
		away_menu->options[2] = "5-30m ON/OFF";
		away_menu->options[3] = "1-15m ON/OFF";
		away_menu->options[4] = "1-5m ON/OFF";
		away_menu->options[5] = "0-1m ON/OFF";
	    
	    // Create everything
	    lcd_lora_setup_page(away_menu);
	    
	    // Show and assign to first element
	    away_menu->index = 0;
	    lcd_lora_update_menu(away_menu);
	    
	    do_once = true;
	}
	
	// Back selected
	if (ui_btns->left_btn == 1) {
		// Delete away_menu lv_obj
		lv_obj_del(away_menu->main_list);
		
		// Free the styles
		lv_style_reset(&away_menu->btn_style);
		lv_style_reset(&away_menu->sel_style);
		
		// Free what was allocated
		free(away_menu);
		
		// Reset statics
		do_once = false;
		away_menu = NULL;
		
		// Show right arrow
		lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Show LoRa submenu cont
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
		
		ui_menu->page = LORA_SUBPAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete away_menu lv_obj
		lv_obj_del(away_menu->main_list);
		
		// Free the styles
		lv_style_reset(&away_menu->btn_style);
		lv_style_reset(&away_menu->sel_style);
		
		// Free what was allocated
		free(away_menu);
		
		// Reset statics
		do_once = false;
		away_menu = NULL;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// Scroll up pressed
	else if (ui_btns->up_btn == 1) {
		// Update selection
		away_menu->index--;
		lcd_lora_update_menu(away_menu);
	}
	// Scroll down pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		away_menu->index++;
		lcd_lora_update_menu(away_menu);
	}
	// Specific option selected
	else if (ui_btns->select_btn == 1 && away_menu->index != 0) {
		// Hide away_menu
		lv_obj_add_flag(away_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Send the data to lora_task
		lora_cmd.index = lora_menu->submenu.index;
		memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_ENC_KEY_LEN);
		snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "away %s", away_menu->options[away_menu->index]);
		xQueueSend(xLoraSendEncQueue, &lora_cmd, 0);

		// Confirmation text
		lv_obj_t *lbl_send_conf = lv_label_create(ACTIVE_SCR); // Create and format label
		lcd_format_label(lbl_send_conf, "Sending to PolyPlug...", user_secondary_color,
				 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(500)); // Wait 500ms
		lv_obj_del(lbl_send_conf); // Delete label
		lcd_clear_pending_inputs = true;
		
		// Delete away_menu lv_obj
		lv_obj_del(away_menu->main_list);

		// Free the styles
		lv_style_reset(&away_menu->btn_style);
		lv_style_reset(&away_menu->sel_style);

		// Free what was allocated
		free(away_menu);

		// Reset statics
		do_once = false;
		away_menu = NULL;

		// Show LoRa submenu cont
		lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

		ui_menu->page = LORA_SUBPAGE;
	}
}

esp_err_t lcd_lora_menu_nvs_save(const lora_menu_t *menu)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(LORA_OPTIONS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    	return err;

    // menu->options[0] is default "Add New"
    // If menu->size == 1 there are no user names, otherwise there are menu->size - 1 names
    uint8_t user_cnt = (menu->size > 1) ? menu->size - 1 : 0;
    err = nvs_set_u8(h, LORA_OPTIONS_KEY_COUNT, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
    	goto out;

	// Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt; i++) {
        char key[16];
        snprintf(key, sizeof(key), LORA_OPTIONS_KEY_FMT, i);
        
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

esp_err_t lcd_lora_key_nvs_save(const lora_menu_t *menu)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    	return err;

    // menu->options[0] is default "Add New"
    // If menu->size == 1 there are no user names, otherwise there are menu->size - 1 names
    uint8_t user_cnt = (menu->size > 1) ? menu->size - 1 : 0;
    err = nvs_set_u8(h, LORA_ENC_KEY_COUNT, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
    	goto out;

	// Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt; i++) {
        char key[16];
        snprintf(key, sizeof(key), LORA_ENC_KEY_FMT, i);
        
        // Store the key string at each key starting at index 1 to match user options
        err = nvs_set_blob(h, key, menu->keys[i + 1], LORA_ENC_KEY_LEN);
        
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

esp_err_t lcd_lora_menu_nvs_load(lora_menu_t *menu)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(LORA_OPTIONS_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
    	return err;

	// Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_OPTIONS_KEY_COUNT, &user_cnt);
    if (err != ESP_OK) {
		nvs_close(h);
		return err;
	}

    menu->size = 1; // Don't change first option
    menu->index = 0;

	// Loop through all keys
    for (uint8_t i = 0; i < user_cnt; i++) {
		
        char key[16];
        snprintf(key, sizeof(key), LORA_OPTIONS_KEY_FMT, i);
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

esp_err_t lcd_lora_key_nvs_load(lora_menu_t *menu)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
    	return err;

	// Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_ENC_KEY_COUNT, &user_cnt);
    if (err != ESP_OK) {
		nvs_close(h);
		return err;
	}

    menu->size = 1; // Don't change first option
    menu->index = 0;

	// Loop through all keys
    for (uint8_t i = 0; i < user_cnt; i++) {
        
        char key[16];
        snprintf(key, sizeof(key), LORA_ENC_KEY_FMT, i);
        
        // Read exactly LORA_ENC_KEY_LEN bytes
        size_t blob_len = LORA_ENC_KEY_LEN;
        
        // First check existence & size
        err = nvs_get_blob(h, key, NULL, &blob_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK || blob_len != LORA_ENC_KEY_LEN) {
            break;
        }

        uint8_t *buf = malloc(LORA_ENC_KEY_LEN);
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

esp_err_t lcd_lora_menu_nvs_delete(uint8_t del_idx)
{
	// Open NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(LORA_OPTIONS_NS, NVS_READWRITE, &h);
    
    // Error check
    if (err != ESP_OK)
    	return err;

    // Get current number of items in menu
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_OPTIONS_KEY_COUNT, &user_cnt);
    
    // Error check/if out of range
    if (err != ESP_OK || del_idx >= user_cnt + 1) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    }

    // Shift every key above del_idx down one slot
    for (uint8_t i = del_idx; i < user_cnt; i++) {
        char key_src[16], key_dst[16];
        
        // Format key
        snprintf(key_src, sizeof key_src, LORA_OPTIONS_KEY_FMT, i);
        snprintf(key_dst, sizeof key_dst, LORA_OPTIONS_KEY_FMT, i - 1);

		// Get key length
        size_t len = 0;
        if ((err = nvs_get_str(h, key_src, NULL, &len)) != ESP_OK)
        	break;
		
		// Store
        char *buf = malloc(len);
        if (!buf) {
			err = ESP_ERR_NO_MEM; 
			break;
		}

		// Get the string
        err = nvs_get_str(h, key_src, buf, &len);
        
        // Set it to new destination
        if (err == ESP_OK)
        	err = nvs_set_str(h, key_dst, buf);
		
		// Free buffer
        free(buf);
        
        if (err != ESP_OK)
        	break;
    }

    // Erase the dangling last slot
    if (err == ESP_OK) {
        char key_last[16];
        snprintf(key_last, sizeof key_last, LORA_OPTIONS_KEY_FMT, user_cnt - 1);
        err = nvs_erase_key(h, key_last);
    }

    // Update count and commit changes to NVS
    if (err == ESP_OK) {
        err = nvs_set_u8(h, LORA_OPTIONS_KEY_COUNT, user_cnt - 1);
        if (err == ESP_OK)
        	err = nvs_commit(h);
    }
	
	// Close NVS
    nvs_close(h);
    return err;
}

esp_err_t lcd_lora_key_nvs_delete(uint8_t del_idx)
{
	// Open NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READWRITE, &h);
    
    // Error check
    if (err != ESP_OK)
    	return err;

	// Get number of keys
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_ENC_KEY_COUNT, &user_cnt);
    
    // Error check
    if (err != ESP_OK || del_idx >= user_cnt + 1) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    }

	// Buffer
    uint8_t tmp[LORA_ENC_KEY_LEN];

	// Shift all keys down one
    for (uint8_t i = del_idx; i < user_cnt; i++) {
        char src[16], dst[16];
        
        // Format key
        snprintf(src, sizeof src, LORA_ENC_KEY_FMT, i);
        snprintf(dst, sizeof dst, LORA_ENC_KEY_FMT, i - 1);

        size_t len = LORA_ENC_KEY_LEN;
        // Get key from src
        err = nvs_get_blob(h, src, tmp, &len);
        if (err != ESP_OK || len != LORA_ENC_KEY_LEN) break;

		// Set key to new dst
        err = nvs_set_blob(h, dst, tmp, LORA_ENC_KEY_LEN);
        if (err != ESP_OK) break;
    }

	// Erase dangling key
    if (err == ESP_OK) {
        char last[16];
        snprintf(last, sizeof last, LORA_ENC_KEY_FMT, user_cnt - 1);
        err = nvs_erase_key(h, last);
    }
	
	// Set new count
    if (err == ESP_OK) {
        err = nvs_set_u8(h, LORA_ENC_KEY_COUNT, user_cnt - 1);
        if (err == ESP_OK) err = nvs_commit(h);
    }
	
	// Close NVS
    nvs_close(h);
    return err;
}