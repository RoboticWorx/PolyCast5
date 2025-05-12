#include "esp_log.h"

#include "lcd_infrared_funcs.h"
#include "lcd_funcs.h"
#include "lcd_task.h"
#include "infrared_task.h"

ir_menu_t ir_menu = {
    .options = {"Add New"},
    .size = 1,
    .index = 0,
    .cont = NULL,
};

static const char* TAG = "LCD_LR_FUNCS";

static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};
static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};

/* Expose final result when done */
const char *lcd_infrared_get_saved_name(void) {
    return saved_name[0] ? saved_name : NULL;
}

void lcd_infrared_create_custom_name(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns) {
	
    // Declare statics
    static int cur_pos = 0; // User position
    static char cur_char = '_';
    static lv_obj_t *lbl_ins = NULL;
    static lv_obj_t *lbl_name = NULL;
    char display[MAX_CUSTOM_NAME_LEN + 2];
    
    // Create initial label
    if (!lbl_name) {
		
        lbl_name = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_name, "", user_secondary_color,
                         &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 20);
                         
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "Enter name with \n  u/d/l/r buttons:", user_secondary_color,
                         &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, -30);
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
    // If left
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
        lv_obj_delete(lbl_name);
        lv_obj_delete(lbl_ins);
        
        // Reset statics for next time
        lbl_name = NULL;
	    lbl_ins  = NULL;
	    cur_pos  = 0;
	    cur_char = '_';
	    memset(name_buf, 0, sizeof name_buf);
        
        // Update options
        if (ui_menu->page == INFRARED_REMOTE_NAME_PAGE) {
			ir_menu->size++;
			
			char *name_copy = strdup(saved_name);
			ir_menu->options[ir_menu->size - 1] = name_copy;
			
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

    lv_label_set_text(lbl_name, display);
}

void lcd_infrared_setup_page(ir_menu_t *menu)
{

	// Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 105, 209);
    
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

void lcd_infrared_update_menu(ir_menu_t *menu)
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

    // Remember previous selection across calls
    static int prev = -1;
    if (prev == menu->index) // If the same, return
        return;

    // Remove highlight from previous button
    if (prev >= 0 && prev < menu->size) {
        lv_obj_remove_style(menu->btns[prev], &menu->sel_style, 0);
        lv_obj_add_style(menu->btns[prev], &menu->btn_style, 0);
    }

    // Add highlight to new button
    lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);

    prev = menu->index;
}



void lcd_infrared_save_new_signal(ir_menu_t *menu)
{
	if (menu->index == 0)
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
}

void lcd_infrared_create_new_remote(ui_menu_t *ui_menu, ir_menu_t *ir_menu)
{
	// Hide IR menu
	lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
	ui_menu->page = INFRARED_REMOTE_NAME_PAGE;
}