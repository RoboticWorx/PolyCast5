#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "nvs.h"

#include "lcd_ir_funcs.h"
#include "lcd_funcs.h"
#include "lcd_task.h"
#include "infrared_task.h"

ir_menu_t ir_menu = {
    .options = {"Add New", "Edit"},
    .size = 2,
    .index = 0,
    .cont = NULL,
};

static const char* TAG = "LCD_IR_FUNCS";

static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};
static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};

static bool ir_menu_overwrite = false;
static uint8_t ir_index_overwrite = 0;


void lcd_ir_edit_remotes(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns)
{
    // Declare statics
    static int edit_idx = 2;
    static lv_obj_t *lbl_title = NULL;
    static lv_obj_t *lbl_name = NULL;
    static lv_obj_t *lbl_hint = NULL;

	// If not already done
    if (!lbl_title) {
		
		// If no user remotes
        if (ir_menu->size <= 2) {
			
			// Help text
			lbl_title = lv_label_create(ACTIVE_SCR);

			lcd_format_label(lbl_title, "No signals to edit!", user_secondary_color, &lv_font_montserrat_18,
							 LV_ALIGN_CENTER, 0, 0);

			// Show and wait
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(1000));
			
			// Clean up
			lv_obj_delete(lbl_title);
			lbl_title = NULL;
			
			// Go back
            ui_menu->page = INFRARED_PAGE;
            	
            return;
        }
        
        // Reset index to beginning
        edit_idx = 2;

		// Initial text
		lbl_title = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_title, "Edit Signal:", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 10);

		lbl_name = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_name, ir_menu->options[edit_idx],
						 user_secondary_color, &lv_font_montserrat_24,
						 LV_ALIGN_CENTER, 0, -10);

		lbl_hint = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_hint, "U/D select BACK delete\nR rename L exit",
						 user_secondary_color, &lv_font_montserrat_14,
						 LV_ALIGN_BOTTOM_MID, 0, -20);
	}

	// Rename
    if (ui_btns->right_btn) {
		// Pass index to edit as the active then switch pages
		ir_index_overwrite = edit_idx;
		ir_menu_overwrite = true;
        ui_menu->page = INFRARED_REMOTE_NAME_PAGE;

        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_hint);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_hint = NULL;
        return;
       
    }
	// Delete menu option
    else if (ui_btns->back_btn) {
		// Deletion
        int to_delete = edit_idx;
	    if (lcd_ir_ir_menu_nvs_delete(ir_menu, to_delete, A_IR_REMOTE_NS, A_REMOTE_KEY_COUNT, A_REMOTE_KEY_FMT) == ESP_OK) {
	        int q = -to_delete;
	        xQueueSend(xSignalToTXQueue, &q, 0);
	    }
	
	    // Now that ir_menu->size has shrunk, wrap
	    if (edit_idx >= ir_menu->size) {
	        edit_idx = 2;
	    }
        
        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_hint);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_hint = NULL;
        
        // Switch pages
        ui_menu->page = INFRARED_PAGE;
        return;
    }
	// Exit
    else if (ui_btns->left_btn) {
        // Reset for next time
        lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_hint);
        lbl_title = NULL;
        lbl_name = NULL;
        lbl_hint = NULL;
        
        // Switch pages
        ui_menu->page = INFRARED_PAGE;
        return;
    }
	// Iterate up
    else if (ui_btns->up_btn) {
        edit_idx++;
        
        // Wrap
        if (edit_idx >= ir_menu->size)
        	edit_idx = 2;
        	
        lv_label_set_text(lbl_name, ir_menu->options[edit_idx]);
        return;
    }
	// Iterate down
    else if (ui_btns->down_btn) {
        edit_idx--;

		// Wrap
        if (edit_idx < 2)
        	edit_idx = ir_menu->size - 1;

        lv_label_set_text(lbl_name, ir_menu->options[edit_idx]);
        return;
    }
}

void lcd_ir_create_custom_name(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns)
{
	
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
		lcd_format_label(lbl_dirs, "Enter signal name\nwith arrow buttons:", user_secondary_color,
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
    /*else if (ui_btns->left_btn && cur_pos == 0) {
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
	    
 		ui_menu->page = INFRARED_PAGE;
		return;
    }*/
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
		if (ir_menu_overwrite) {
			// ir_menu->index is passed as edit_idx:
			// Release old string then reallocate
			free(ir_menu->options[ir_index_overwrite]);
			ir_menu->options[ir_index_overwrite] = strdup(saved_name);

			// Persist to NVS
			lcd_ir_ir_menu_nvs_save(ir_menu, A_IR_REMOTE_NS, A_REMOTE_KEY_COUNT, A_REMOTE_KEY_FMT);

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
			lcd_ir_ir_menu_nvs_save(ir_menu, A_IR_REMOTE_NS, A_REMOTE_KEY_COUNT, A_REMOTE_KEY_FMT);

			// Create new button for new option
			ir_menu->btns[ir_menu->size - 1] = lv_list_add_btn(ir_menu->main_list, NULL, ir_menu->options[ir_menu->size - 1]);
			lv_obj_set_size(ir_menu->btns[ir_menu->size - 1], 100, 28);
			lv_obj_add_style(ir_menu->btns[ir_menu->size - 1],
							 &ir_menu->btn_style, 0);

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
	
	// Adjust rotation
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
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    }

    // Format buttons as container
    menu->cont = lv_obj_get_parent(menu->btns[0]);
    lv_obj_set_flex_flow (menu->cont, LV_FLEX_FLOW_COLUMN);
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
    for (int i = 0; i < menu->size; ++i) {
        lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
        lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);
    
    // Enable scrolling if list gets too long
    lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_OFF);
}

void lcd_ir_save_new_signal(ir_menu_t *menu)
{
	// Hide IR menu
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
	// Restart infrared RX
	xSemaphoreGive(xStartInfraredRXSemaphore);
		
	// Create texts
	lv_obj_t *text_label = lv_label_create(ACTIVE_SCR);
	lcd_format_label(text_label, "Present signal!", user_secondary_color,
				 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
	lv_timer_handler();
		
	// Wait until signal received and saved
	xSemaphoreTake(xSignalSavedSemaphore, portMAX_DELAY);
	lv_label_set_text(text_label, "Saving...");
	lv_timer_handler();
		
	// Conclude
	vTaskDelay(pdMS_TO_TICKS(500));
	lv_obj_delete(text_label);
}

esp_err_t lcd_ir_ir_menu_nvs_save(const ir_menu_t *menu, const char* ns, const char* count, const char* fmt)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK)
    	return err;

    // menu->options[0] is default "Add New", and [1] is "Edit":
    // If menu->size == 2 there are no user names, otherwise there are menu->size - 2 names
    uint8_t user_cnt = (menu->size > 2) ? menu->size - 2 : 0;
    err = nvs_set_u8(h, count, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
    	goto out;

	// Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt; ++i) {
        char key[8];
        sprintf(key, fmt, i);
        
        // Store the menu option string at each key starting at index 2
        err = nvs_set_str(h, key, menu->options[i + 2]);
        
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

esp_err_t lcd_ir_ir_menu_nvs_load(ir_menu_t *menu, const char* ns, const char* count, const char* fmt)
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

    menu->size = 2; // Don't change first two options
    menu->index = 0;

	// Loop through all keys
    for (uint8_t i = 0; i < user_cnt; ++i) {
        char key[8];
        sprintf(key, fmt, i);
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
		if (menu->size >= MAX_IR_OPTIONS) {
		    free(buf);
		    break;
		}
        menu->options[menu->size++] = buf;
    }
    
    // Close NVS
    nvs_close(h);
    
    return ESP_OK;
}

esp_err_t lcd_ir_ir_menu_nvs_delete(ir_menu_t *menu, uint8_t idx, const char* ns, const char* count, const char* fmt)
{
	// Make sure index is > 1 (not "Add New" or "Edit") and not larger than size
    if (idx <= 1 || idx >= menu->size) {
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
    return lcd_ir_ir_menu_nvs_save(menu, ns, count, fmt);
}

void lcd_ir_ir_menu_nvs_clear(void)
{
    nvs_handle_t h;
    
    // Clear all NVS
    if (nvs_open("ir_names", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h); // Wipes only keys in this namespace
        nvs_commit(h);
        nvs_close(h);
    }
}