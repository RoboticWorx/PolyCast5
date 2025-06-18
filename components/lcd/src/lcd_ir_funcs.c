#include "core/lv_obj_pos.h"
#include "polycast5_macros.h"

#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "img_ir_save_new_remote.h"
#include "misc/lv_area.h"

#include "nvs.h"
#include "esp_log.h"

#include "lcd_ir_funcs.h"
#include "lcd_funcs.h"
#include "lcd_task.h"
#include "infrared_task.h"
#include "gpio_task.h"

#include "img_ir_save_new_remote.h"
#include "widgets/label/lv_label.h"
#include "widgets/list/lv_list.h"

ir_menu_t ir_menu = {
    .options = {"REMOTE", "Add New", "Edit"},
    .size = 3,
    .index = 0,
    .cont = NULL,
};

static const char* TAG = "LCD_IR_FUNCS";

static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};

static bool ir_menu_overwrite = false;
static uint8_t ir_index_overwrite = 0;
static int edit_idx = 0;


void lcd_ir_edit_remotes(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns)
{
	#define REMOTE_TXT "Remote selected:"
	#define SIG_TXT "Signal selected:"
	#define SELECT_TXT "Press SELECT to delete."
	#define EDIT_TXT "Press EDIT to rename."
	
    // Declare statics
    static lv_obj_t *lbl_title = NULL;
    static lv_obj_t *lbl_name = NULL;
    static lv_obj_t *lbl_back = NULL;
    static lv_obj_t *lbl_edit = NULL;
    static lv_obj_t *lbl_select = NULL;

	// If not already done
    if (!lbl_title) {
        // Reset index to beginning
        edit_idx = 0;

		// Initial text
		lbl_title = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_title, REMOTE_TXT, user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

		lbl_name = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_name, ir_menu->options[edit_idx], user_secondary_color, 
					 &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, -1);

		lbl_back = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_back, "BACK", user_secondary_color, 
					 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 2, -17);
						 
		lbl_edit = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_edit, "EDIT", user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_RIGHT_MID, -3, -17);
		
		lbl_select = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_select, EDIT_TXT, user_secondary_color, 
					 &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 35);
	}

	// Rename
    if (ui_btns->right_btn) {
		// Pass index to edit then switch to name page
		ir_index_overwrite = edit_idx;
		ir_menu_overwrite = true;
        ui_menu->page = INFRARED_REMOTE_NAME_PAGE;

        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_back);
		lv_obj_delete(lbl_edit);
		lv_obj_delete(lbl_select);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_back = NULL;
        lbl_edit = NULL;
        lbl_select = NULL;
        
        return;
    }
	// Delete menu option
    else if (ui_btns->select_btn && edit_idx > 2) {
		// Deletion
        int to_delete = edit_idx;
	    if (lcd_ir_menu_nvs_delete(ir_menu, to_delete, A_IR_REMOTE_NS, A_IR_REMOTE_KEY_COUNT, A_IR_REMOTE_KEY_FMT) == ESP_OK) { // Delete menu item
	        int q = -to_delete;
	        xQueueSend(xInfraredSignalToTxQueue, &q, portMAX_DELAY); // Signal ir_task to delete
	    }
	
	    // Now that ir_menu->size has shrunk, wrap if needed
	    if (edit_idx >= ir_menu->size) {
	        edit_idx = 3;
	    }
        
        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_back);
		lv_obj_delete(lbl_edit);
		lv_obj_delete(lbl_select);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_back = NULL;
        lbl_edit = NULL;
        lbl_select = NULL;
        
        // Switch pages
        ui_menu->page = INFRARED_PAGE;
        return;
    }
	// Exit
    else if (ui_btns->left_btn) {
        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_back);
		lv_obj_delete(lbl_edit);
		lv_obj_delete(lbl_select);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_back = NULL;
        lbl_edit = NULL;
        lbl_select = NULL;
        
        // Switch pages
        ui_menu->page = INFRARED_PAGE;
        return;
    }
    // Go home
    else if (ui_btns->home_btn) {
        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_back);
		lv_obj_delete(lbl_edit);
		lv_obj_delete(lbl_select);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_back = NULL;
        lbl_edit = NULL;
        lbl_select = NULL;
        
        lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
    }
    // Power off
    else if (ui_btns->pwr_btn) {
        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_back);
		lv_obj_delete(lbl_edit);
		lv_obj_delete(lbl_select);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_back = NULL;
        lbl_edit = NULL;
        lbl_select = NULL;
        
        lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
    }
	// Iterate up
    else if (ui_btns->up_btn) {
		if (edit_idx == 0) {
			edit_idx = 2; 
		}
        edit_idx++; // Makes 3
        
        // Wrap
        if (edit_idx >= ir_menu->size) {
			edit_idx = 0;
		}
        
        // Change title label if needed
        if (edit_idx == 0) {
			lv_label_set_text(lbl_title, REMOTE_TXT);
			lv_label_set_text(lbl_select, EDIT_TXT);
		}
		else {
			lv_label_set_text(lbl_title, SIG_TXT);
			lv_label_set_text(lbl_select, SELECT_TXT);
		}
        	
        lv_label_set_text(lbl_name, ir_menu->options[edit_idx]);
        return;
    }
	// Iterate down
    else if (ui_btns->down_btn) {		
		if (edit_idx == 0) { // From remote to last signal
	        edit_idx = (ir_menu->size > 3) ? (ir_menu->size - 1) : 0;
	    }
	    else if (edit_idx == 3) { // From first signal to remote
	        edit_idx = 0;
	    }
	    else {
	        --edit_idx;
	    }
        
        // Change title label if needed
        if (edit_idx == 0) {
			lv_label_set_text(lbl_title, REMOTE_TXT);
			lv_label_set_text(lbl_select, EDIT_TXT);
		}
		else {
			lv_label_set_text(lbl_title, SIG_TXT);
			lv_label_set_text(lbl_select, SELECT_TXT);
		}

        lv_label_set_text(lbl_name, ir_menu->options[edit_idx]);
        return;
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

void lcd_ir_create_custom_name(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns)
{
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
        if (ir_menu_overwrite) {
            // Copy the old name into buffer
            strncpy(name_buf, ir_menu->options[ir_index_overwrite], MAX_CUSTOM_NAME_LEN);

            // Place cursor at the end
            cur_pos = strlen(name_buf);
            
            // Start with '_'
            cur_char = '_';
        }
        else { // Else blank slate
            memset(name_buf, 0, sizeof name_buf);
            cur_pos = 0;
            cur_char = '_';
            
            if (!ir_menu_overwrite) { // Can't go back if adding signal
				lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
			}
        }
		
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                         &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_dirs, "Enter signal name\nwith arrow buttons:", user_secondary_color,
                         &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, -30);
        if (ir_menu_overwrite) {
			if (edit_idx == 0) {
				lv_label_set_text(lbl_dirs, "Enter new remote name\n    with arrow buttons:");
			}
			else {
				lv_label_set_text(lbl_dirs, "Enter new signal name\n   with arrow buttons:");
			}
		}
        
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
        
        // If left arrow hidden -> remove
        if (lv_obj_has_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN)) {
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		}
		else if (!ir_menu_overwrite && cur_pos == 0 && cur_char == '_') {
			lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		}
        
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
        
        // If left arrow hidden -> remove
        if (lv_obj_has_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN)) {
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		}
		else if (!ir_menu_overwrite && cur_pos == 0 && cur_char == '_') {
			lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		}
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    }
    // If left pressed and at start and overwriting
    else if (ui_btns->left_btn && cur_pos == 0 && ir_menu_overwrite) {
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
	    
	    ir_menu_overwrite = false;
	    
 		ui_menu->page = INFRARED_REMOTE_EDIT_PAGE;
		return;
    }
    // If go home and overwriting
    else if (ui_btns->home_btn && ir_menu_overwrite) {
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
	    
	    ir_menu_overwrite = false;
	    
 		lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
    }
    // If power off and overwriting
    else if (ui_btns->pwr_btn && ir_menu_overwrite) {
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
	    
	    ir_menu_overwrite = false;
	    
 		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
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
	    
	    // If not able to go back and on first position
	    if (cur_pos == 0 && cur_char == '_' && !ir_menu_overwrite) {
			lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		}
	    
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
        
        // If left arrow hidden -> remove
        if (lv_obj_has_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN)) {
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
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
		if (ir_menu_overwrite) {
			// ir_menu->index is passed as edit_idx:
			// Release old string then reallocate
			if (ir_index_overwrite > 2) { // Can't free what wasn't allocated
				free(ir_menu->options[ir_index_overwrite]);
			}
			ir_menu->options[ir_index_overwrite] = strdup(saved_name);

			// Persist to NVS
			lcd_ir_menu_nvs_save(ir_menu, A_IR_REMOTE_NS, A_IR_REMOTE_KEY_COUNT, A_IR_REMOTE_KEY_FMT);

			// Update the button’s label in-place
			lv_obj_t *btn = ir_menu->btns[ir_index_overwrite];
			lv_obj_t *child_lbl = lv_obj_get_child(btn, 0);
			lv_label_set_text(child_lbl, ir_menu->options[ir_index_overwrite]);

			// Clean up
			ir_menu_overwrite = false;
		}
		// Else adding a whole new remote
		else {
			ir_menu->size++;

			// Save to options, then to NVS
			char *name_copy = strdup(saved_name);
			ir_menu->options[ir_menu->size - 1] = name_copy;
			lcd_ir_menu_nvs_save(ir_menu, A_IR_REMOTE_NS, A_IR_REMOTE_KEY_COUNT, A_IR_REMOTE_KEY_FMT);

			// Create new button for new option
			ir_menu->btns[ir_menu->size - 1] = lv_list_add_btn(ir_menu->main_list, NULL, ir_menu->options[ir_menu->size - 1]);
			lv_obj_set_size(ir_menu->btns[ir_menu->size - 1], 100, 28);
			lv_obj_add_style(ir_menu->btns[ir_menu->size - 1], &ir_menu->btn_style, 0);

			// Create and format text label
			lv_obj_t *lbl = lv_obj_get_child(ir_menu->btns[ir_menu->size - 1], 0);
			lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
			lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
		}
		
		// Switch to previous page
		ui_menu->page = INFRARED_PAGE;
        return;
    }
}

void lcd_ir_setup_page(ir_menu_t *menu)
{
	// Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 105, 208);
    
    // Format
    lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF);     // never draw bars
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(menu->main_list, LV_DIR_VER);
    
    // Set rotation pivot
	lv_obj_set_style_transform_pivot_x(menu->main_list, 120, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_transform_pivot_y(menu->main_list, 67, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	// Rotate
	lv_obj_set_style_transform_angle(menu->main_list, 2700, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	// Adjust position
	lv_obj_set_x(menu->main_list, -105);
	lv_obj_set_y(menu->main_list, -31); // More pos = left
	
	// Adjust spacing
	lv_obj_set_style_pad_row(menu->main_list, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	
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
	
	
	// Create remote name style
	lv_style_init(&menu->name_style);
	
	lv_style_set_radius(&menu->name_style, 8);
	lv_style_set_bg_color(&menu->name_style, user_primary_color);
	
	//lv_style_set_bg_grad_color(&menu->name_style, lv_color_darken(user_primary_color, 60));
	//lv_style_set_bg_grad_dir(&menu->name_style, LV_GRAD_DIR_VER);

	// Add outline for uniqueness
	lv_style_set_outline_width(&menu->name_style, 2);
	lv_style_set_outline_color(&menu->name_style, user_secondary_color);
	lv_style_set_outline_pad(&menu->name_style, 1);
	
	lv_style_set_border_width(&menu->name_style, 2);
	lv_style_set_border_color(&menu->name_style, user_secondary_color);
	lv_style_set_border_side(&menu->name_style, LV_BORDER_SIDE_FULL);
	
	lv_style_set_pad_top(&menu->name_style, 3);
	lv_style_set_pad_bottom(&menu->name_style, 3);
	
	lv_style_set_text_font(&menu->name_style, &lv_font_montserrat_16);
	lv_style_set_text_color(&menu->name_style, user_secondary_color);
	lv_style_set_text_align(&menu->name_style, LV_TEXT_ALIGN_CENTER);
	
	
	// Create selected remote name style
	lv_style_init(&menu->name_sel_style);
	
	lv_style_set_radius(&menu->name_sel_style, 8);
	lv_style_set_bg_color(&menu->name_sel_style, user_secondary_color);

	// Add outline for uniqueness
	lv_style_set_outline_width(&menu->name_sel_style, 2);
	lv_style_set_outline_color(&menu->name_sel_style, user_secondary_color);
	lv_style_set_outline_pad(&menu->name_sel_style, 1);
	
	lv_style_set_border_width(&menu->name_sel_style, 2);
	lv_style_set_border_color(&menu->name_sel_style, user_secondary_color);
	lv_style_set_border_side(&menu->name_sel_style, LV_BORDER_SIDE_FULL);
	
	lv_style_set_pad_top(&menu->name_sel_style, 3);
	lv_style_set_pad_bottom(&menu->name_sel_style, 3);
	
	lv_style_set_text_font(&menu->name_sel_style, &lv_font_montserrat_16);
	lv_style_set_text_color(&menu->name_sel_style, user_primary_color);
	lv_style_set_text_align(&menu->name_sel_style, LV_TEXT_ALIGN_CENTER);
	
	
	// Create buttons
	// Wrap index
	if (menu->index >= menu->size) {
		menu->index = 0;
	}
	else if (menu->index < 0) {
		menu->index = menu->size - 1;
	}
	
	// Create button for each option
    for (int i = 0; i < menu->size; i++) {

        menu->btns[i] = lv_list_add_btn(menu->main_list, NULL, menu->options[i]);
        lv_obj_set_size(menu->btns[i], 100, 28);

        // Style selected   
        if (i == menu->index) { // Index match
			if (i == 0) { // If remote name
				lv_obj_add_style(menu->btns[i], &menu->name_sel_style, 0);
			}
			else {
				lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
			}   
        }
        else {
			if (i == 0) { // If remote name
				lv_obj_add_style(menu->btns[i], &menu->name_style, 0);
			}
			else {
				lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
			}   
        }

        // Create and format text label
        lv_obj_t *lbl = lv_obj_get_child(menu->btns[i], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    }

    // Format buttons as container
    menu->cont = lv_obj_get_parent(menu->btns[0]);
    lv_obj_set_flex_flow(menu->cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	
	// Hide for now
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_ir_update_menu(ir_menu_t *menu)
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
		if (i == 0) { // If remote name
			lv_obj_remove_style(menu->btns[i], &menu->name_sel_style, 0);
    		lv_obj_add_style(menu->btns[i], &menu->name_style, 0);
		}
		else {
			lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
        	lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
		}
    }
    
    // Highlight only the current index
    if (menu->index == 0) { // If remote name
		lv_obj_remove_style(menu->btns[menu->index], &menu->name_style, 0);
		lv_obj_add_style(menu->btns[menu->index], &menu->name_sel_style, 0);
	}
	else {
		lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    	lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);
	}

    
    // Enable scrolling if list gets too long
    lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_OFF); // LV_ANIM_ON
}

void lcd_ir_save_new_signal(ui_menu_t *ui_menu, ir_menu_t *menu)
{
	// Hide IR menu
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
	
	// Hide arrows
	lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
	// Restart infrared RX
	xSemaphoreGive(xInfraredStartRxSemaphore);
		
	// Create texts
	lv_obj_t *text_label = lv_label_create(ACTIVE_SCR);
	lcd_format_label(text_label, "Point your device at the\nIR lens and send the signal.", user_secondary_color,
				 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 13);
				 
	// Create present signal img
    lv_obj_t *img_save_remote = lv_img_create(ACTIVE_SCR);
    lv_img_set_src(img_save_remote, &img_ir_save_new_remote);
    lv_obj_align(img_save_remote, LV_ALIGN_CENTER, 0, 25);
    
	lv_timer_handler();
	
	// Wait until signal received and saved	
	while (1) {
		
		lv_timer_handler();
		
        if (xSemaphoreTake(xInfraredSignalSavedSemaphore, 0) == pdTRUE) {
			lv_obj_delete(img_save_remote); // Delete img
			
            // Signal arrived
            lv_obj_center(text_label);
            lv_obj_set_style_text_font(text_label, &lv_font_montserrat_24, 0);
            lv_label_set_text(text_label, "Saving...");
			lv_timer_handler();
			
			vTaskDelay(pdMS_TO_TICKS(500));
			lv_obj_delete(text_label);
			
			// Show arrows
			lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			lcd_clear_pending_inputs = true; // Clear any false inputs
			
			// Switch to naming page
			ui_menu->page = INFRARED_REMOTE_NAME_PAGE;
			
            break;
        }
        if (xSemaphoreTake(xLeftButtonSemaphore, 0)) {
            // User hit cancel
            xSemaphoreGive(xInfraredDisableSemaphore);
            
            // Delete objects
            lv_obj_delete(text_label);
            lv_obj_delete(img_save_remote);
            
            // Show arrows
			lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Go back
			ui_menu->page = INFRARED_PAGE;
            
            break;
        }
    }
	
	vTaskDelay(pdMS_TO_TICKS(20));
}

esp_err_t lcd_ir_menu_nvs_save(const ir_menu_t *menu, const char* ns, const char* count, const char* fmt)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK)
    	return err;

    // menu->options[0] is the default name, next is "Add New", and [2] is "Edit":
    // If menu->size == 3 there are no user names, otherwise there are menu->size - 3 names
    uint8_t user_cnt = (menu->size > 3) ? menu->size - 3 : 0;
    err = nvs_set_u8(h, count, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
    	goto out;

	// Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt + 1; i++) { // + 1 for remote name
        char key[8];
        sprintf(key, fmt, i);
        
        // Store the menu option string at each key
        if (i == 0) { // Store remote name [0]
			err = nvs_set_str(h, key, menu->options[i]);
		}
		else { // All the signals [3+]
			err = nvs_set_str(h, key, menu->options[i + 2]); // 1 + 2 = 3 which is signal name offset
		}
        
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

esp_err_t lcd_ir_menu_nvs_load(ir_menu_t *menu, const char* ns, const char* count, const char* fmt)
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

    menu->size = 3; // Signals start at [3]
    menu->index = 0;

	// Loop through all keys
    for (uint8_t i = 0; i < user_cnt + 1; i++) { // + 1 for remote name
        char key[8];
        sprintf(key, fmt, i);
        size_t len = 0;
        
        // Check size
		if (menu->size >= MAX_IR_OPTIONS) {
		    break;
		}
        
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
		
		if (i == 0) { // Remote name [0]
			menu->options[0] = buf;
		}
		else { // All the signal names [3] which is starting menu->size
			menu->options[menu->size++] = buf;
		}
    }
    
    // Close NVS
    nvs_close(h);
    
    return ESP_OK;
}

esp_err_t lcd_ir_menu_nvs_delete(ir_menu_t *menu, uint8_t idx, const char* ns, const char* count, const char* fmt)
{
	// Make sure index is > 2 (not the name, "Add New" or "Edit") and not larger than size
    if (idx <= 2 || idx >= menu->size) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Update for list buttons
    lv_obj_del(menu->btns[idx]);
    for (uint8_t i = idx; i < menu->size - 1; ++i) {
        menu->btns[i] = menu->btns[i + 1];
    }
    menu->btns[menu->size - 1] = NULL;

    // Remove the string
    free(menu->options[idx]);
    for (uint8_t i = idx; i < menu->size - 1; ++i) {
        menu->options[i] = menu->options[i + 1]; // Shift all after left
    }
    menu->size--; // One less entry
    
    // Erase the stale key from flash
    nvs_handle_t h;
	esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
	if (err == ESP_OK) {
	    char stale_key[6];
	    sprintf(stale_key, fmt, menu->size);
	    nvs_erase_key(h, stale_key);
	    nvs_commit(h);
	    nvs_close(h);
	}

    // Rewrite flash with the new list
    return lcd_ir_menu_nvs_save(menu, ns, count, fmt);
}