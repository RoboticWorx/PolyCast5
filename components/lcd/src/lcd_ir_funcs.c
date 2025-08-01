#include "polycast5_macros.h"

#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

#include "nvs.h"
#include "esp_log.h"

#include "core/lv_obj_pos.h"
#include "misc/lv_area.h"
#include "widgets/label/lv_label.h"
#include "widgets/list/lv_list.h"

#include "lcd_ir_funcs.h"
#include "lcd_utils.h"
#include "infrared_funcs.h"
#include "infrared_task.h"
#include "gpio_task.h"

#include "img_save_new_remote.h"

ir_menu_t ir_menu = {
	.size = 0,
	.index = 0,
	.cont = NULL,
};

static const char* TAG = "LCD_IR_FUNCS";

static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};

static bool ir_menu_overwrite = false;
static uint8_t ir_index_overwrite = 0;
static int edit_idx = 0;
static bool new_remote = false;

void lcd_ir_edit_remotes(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t *ir_menu)
{
	#define REMOTE_TXT "Remote option:"
	#define SIG_TXT "Signal selected:"
	
	#define SELECT_TXT "Select to delete signal."
	#define EDIT_TXT "Press edit to rename."
	
	#define CREATE_REMOTE_TXT "Select to add a remote."
	#define DELETE_REMOTE_TXT "Select to delete remote."
	
	#define ADD_NEW_LABEL "Add REM"
	#define DELETE_LABEL "Delete REM"
	
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

		xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
		lbl_name = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_name, remotes[current_remote].name, user_secondary_color, 
				 &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, -1);
		xSemaphoreGive(xInfraredDataMutex); // Release IR

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
		lbl_title = lbl_name = lbl_back = lbl_edit = lbl_select = NULL;
	}
	// Actions on select
	else if (ui_btns->select_btn) {
		// Create new remote
		if (edit_idx == 1) {
			new_remote = true;
			ui_menu->page = INFRARED_REMOTE_NAME_PAGE;

			// Reset
			lv_obj_delete(lbl_title);
			lv_obj_delete(lbl_name);
			lv_obj_delete(lbl_back);
			lv_obj_delete(lbl_edit);
			lv_obj_delete(lbl_select);
			lbl_title = lbl_name = lbl_back = lbl_edit = lbl_select = NULL;
		}
		// Delete remote
		else if (edit_idx == 2) {
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			infrared_nvs_delete_remote(current_remote);
			xSemaphoreGive(xInfraredDataMutex); // Release IR
			
			// Back to main page
			ui_menu->page = INFRARED_PAGE;

			// Reset
			lv_obj_delete(lbl_title);
			lv_obj_delete(lbl_name);
			lv_obj_delete(lbl_back);
			lv_obj_delete(lbl_edit);
			lv_obj_delete(lbl_select);
			lbl_title = lbl_name = lbl_back = lbl_edit = lbl_select = NULL;
		}
		// Delete signal
		else if (edit_idx > 2) {
			int to_delete = edit_idx;
			int q = -to_delete;
			xQueueSend(xInfraredSignalToTxQueue, &q, portMAX_DELAY);

			// Back to main
			ui_menu->page = INFRARED_PAGE;

			// Reset
			lv_obj_delete(lbl_title);
			lv_obj_delete(lbl_name);
			lv_obj_delete(lbl_back);
			lv_obj_delete(lbl_edit);
			lv_obj_delete(lbl_select);
			lbl_title = lbl_name = lbl_back = lbl_edit = lbl_select = NULL;
		}
	}
	// Exit
	else if (ui_btns->left_btn) {
		// Reset
		lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_back);
		lv_obj_delete(lbl_edit);
		lv_obj_delete(lbl_select);
		lbl_title = lbl_name = lbl_back = lbl_edit = lbl_select = NULL;
		
		// Switch pages
		ui_menu->page = INFRARED_PAGE;
	}
	// Go home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Reset
		lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_name);
		lv_obj_delete(lbl_back);
		lv_obj_delete(lbl_edit);
		lv_obj_delete(lbl_select);
		lbl_title = lbl_name = lbl_back = lbl_edit = lbl_select = NULL;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu);
	}
	// Iterate up
	else if (ui_btns->up_btn) {
		edit_idx++;
		
		// Wrap
		if (edit_idx >= ir_menu->size) {
			edit_idx = 0;
		}
		
		// Remote name
		if (edit_idx == 0) {
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			lv_label_set_text(lbl_name, remotes[current_remote].name);
			xSemaphoreGive(xInfraredDataMutex); // Release IR
		}
		// New remote
		else if (edit_idx == 1) {
			lv_label_set_text(lbl_name, ADD_NEW_LABEL);
		}
		// Delete remote
		else if (edit_idx == 2) {
			lv_label_set_text(lbl_name, DELETE_LABEL);
		}
		// Editing signal
		else {
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			lv_label_set_text(lbl_name, remotes[current_remote].signal_names[edit_idx - 3]); // Default option offset
			xSemaphoreGive(xInfraredDataMutex); // Release IR
		}
		
		// Editing remote
		if (edit_idx <= 2) {
			lv_label_set_text(lbl_title, REMOTE_TXT);
			if (edit_idx == 0) {
				lv_label_set_text(lbl_select, EDIT_TXT);
			}
			else if (edit_idx == 1) {
				lv_label_set_text(lbl_select, CREATE_REMOTE_TXT);
			}
			else {
				lv_label_set_text(lbl_select, DELETE_REMOTE_TXT);
			}
		}
		// Editing signal
		else {
			lv_label_set_text(lbl_title, SIG_TXT);
			lv_label_set_text(lbl_select, SELECT_TXT);
		}
	}
	// Iterate down
	else if (ui_btns->down_btn) {	
		// Wrap	
		if (edit_idx == 0) {
			edit_idx = (ir_menu->size > 3) ? (ir_menu->size - 1) : 0;
		}
		// Else decrement
		else {
			edit_idx--;
		}
		
		// Remote name
		if (edit_idx == 0) {
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			lv_label_set_text(lbl_name, remotes[current_remote].name);
			xSemaphoreGive(xInfraredDataMutex); // Release IR
		}
		// New remote
		else if (edit_idx == 1) {
			lv_label_set_text(lbl_name, ADD_NEW_LABEL);
		}
		// Delete remote
		else if (edit_idx == 2) {
			lv_label_set_text(lbl_name, DELETE_LABEL);
		}
		// Editing signal
		else {
			xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
			lv_label_set_text(lbl_name, remotes[current_remote].signal_names[edit_idx - 3]); // Default option offset
			xSemaphoreGive(xInfraredDataMutex); // Release IR
		}
		
		// Editing remote
		if (edit_idx <= 2) {
			lv_label_set_text(lbl_title, REMOTE_TXT);
			if (edit_idx == 0) {
				lv_label_set_text(lbl_select, EDIT_TXT);
			}
			else if (edit_idx == 1) {
				lv_label_set_text(lbl_select, CREATE_REMOTE_TXT);
			}
			else {
				lv_label_set_text(lbl_select, DELETE_REMOTE_TXT);
			}
		}
		// Editing signal
		else {
			lv_label_set_text(lbl_title, SIG_TXT);
			lv_label_set_text(lbl_select, SELECT_TXT);
		}
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

void lcd_ir_create_custom_name(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t *ir_menu)
{
	// Declare statics
	static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};
	static int cur_pos = 0; // User position
	static char cur_char = '_';
	static lv_obj_t *lbl_dirs = NULL;
	static lv_obj_t *lbl_chars = NULL;
	static lv_obj_t *lbl_user_in = NULL;
	
	// Do once
	if (!lbl_user_in) {
		// If renaming, autofill what was there previously
		if (ir_menu_overwrite) {
			// Renaming remote
			if (ir_index_overwrite == 0) {
				// Copy the old remote name into buffer
				xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
				strncpy(name_buf, remotes[current_remote].name, MAX_CUSTOM_NAME_LEN);
				xSemaphoreGive(xInfraredDataMutex); // Release IR
			}
			// Renaming signal
			else {
				// Copy the old signal name into buffer
				size_t sig_idx = ir_index_overwrite - 3; // Offset by default options
				xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
				strncpy(name_buf, remotes[current_remote].signal_names[sig_idx], MAX_CUSTOM_NAME_LEN);
				xSemaphoreGive(xInfraredDataMutex); // Release IR
			}

			// Place cursor at the end
			cur_pos = strlen(name_buf);
			
			// Start with '_'
			cur_char = '_';
		}
		// Else adding new signal
		else {
			// Zero out
			memset(name_buf, 0, sizeof name_buf);
			cur_pos = 0;
			cur_char = '_';
			
			// Hide left arrow: can't go back if adding signal
			lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		}
		
		// Create labels
		lbl_user_in = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_user_in, "", user_secondary_color,
					&lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
		
		lbl_dirs = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_dirs, "Enter signal name\nwith arrow buttons:", user_secondary_color,
					&lv_font_montserrat_18, LV_ALIGN_CENTER, 0, -30);
		
		// Adjust label
		if (ir_menu_overwrite) {
			// Changing remote name
			if (ir_index_overwrite == 0) {
				lv_label_set_text(lbl_dirs, "Enter new remote name\n   with arrow buttons:");
			}
			// Changing signal name
			else {
				lv_label_set_text(lbl_dirs, "Enter new signal name\n   with arrow buttons:");
			}
		}
		// If adding new remote
		else if (new_remote) {
			lv_label_set_text(lbl_dirs, "Enter new remote name\n	  with arrow buttons:");
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
		// Delete objects
		lv_obj_delete(lbl_user_in);
		lv_obj_delete(lbl_dirs);
		lv_obj_delete(lbl_chars);
		
		// Reset statics
		lbl_user_in = lbl_dirs = lbl_chars = NULL;
		cur_pos = 0;
		cur_char = '_';
		memset(name_buf, 0, sizeof name_buf);
		
		ir_menu_overwrite = false;
		
 		ui_menu->page = INFRARED_REMOTE_EDIT_PAGE;
		return;
	}
	// If go home or power off and overwriting
	else if ((ui_btns->home_btn || ui_btns->pwr_btn) && ir_menu_overwrite) {
		// Delete objects
		lv_obj_delete(lbl_user_in);
		lv_obj_delete(lbl_dirs);
		lv_obj_delete(lbl_chars);
		
		// Reset statics
		lbl_user_in = lbl_dirs = lbl_chars = NULL;
		cur_pos = 0;
		cur_char = '_';
		memset(name_buf, 0, sizeof name_buf);
		
		ir_menu_overwrite = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
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
		
		// Delete objects
		lv_obj_delete(lbl_user_in);
		lv_obj_delete(lbl_dirs);
		lv_obj_delete(lbl_chars);
		
		// Reset statics
		lbl_user_in = lbl_dirs = lbl_chars = NULL;
		cur_pos = 0;
		cur_char = '_';
		memset(name_buf, 0, sizeof name_buf);

		/* Update options */
		// If overwriting an existing as a rename
		if (ir_menu_overwrite) {
			// Rename remote
			if (ir_index_overwrite == 0) {
				xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
				free(remotes[current_remote].name);
				remotes[current_remote].name = strdup(saved_name);
				infrared_nvs_save_remote_name(current_remote);
				xSemaphoreGive(xInfraredDataMutex); // Release IR
			}
			// Rename signal
			else {
				size_t sig_idx = ir_index_overwrite - 3; // Offset by default options
				xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
				free(remotes[current_remote].signal_names[sig_idx]);
				remotes[current_remote].signal_names[sig_idx] = strdup(saved_name);
				infrared_nvs_save_signal_to_remote(current_remote, sig_idx, remotes[current_remote].signals[sig_idx], saved_name);
				xSemaphoreGive(xInfraredDataMutex); // Release IR
			}
			
			// Update the button’s label in-place
			lv_obj_t *btn = ir_menu->btns[ir_index_overwrite];
			lv_obj_t *child_lbl = lv_obj_get_child(btn, 0);
			lv_label_set_text(child_lbl, saved_name);

			// Clean up
			ir_menu_overwrite = false;
		}
		else {
			// Adding new remote
			if (new_remote) {
				// Limit check
				if (num_remotes >= MAX_REMOTES) {
					ESP_LOGW(TAG, "Max remotes reached");
					return;
				}
				
				// Add the remote
				xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
				size_t new_idx = num_remotes;
				remotes[new_idx].name = strdup(saved_name);
				remotes[new_idx].num_signals = 0;
				remotes[new_idx].signals = NULL;
				remotes[new_idx].signal_names = NULL;
				num_remotes++;
				
				// Current is new
				current_remote = new_idx;
				
				// Save new remote to NVS
				infrared_nvs_save_all_remotes();
				xSemaphoreGive(xInfraredDataMutex); // Release IR

				new_remote = false;
			}
			// Adding new signal name (signal data already saved when received)
			else {
				xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
				size_t ns = remotes[current_remote].num_signals - 1; // num_signals
				
				// Save to remote
				remotes[current_remote].signal_names[ns] = strdup(saved_name);
				infrared_nvs_save_signal_to_remote(current_remote, ns, remotes[current_remote].signals[ns], saved_name);
				xSemaphoreGive(xInfraredDataMutex); // Release IR
			}
			
			// Create new button for new option
			ir_menu->btns[ir_menu->size] = lv_list_add_btn(ir_menu->main_list, NULL, saved_name);
			lv_obj_set_size(ir_menu->btns[ir_menu->size], 100, 28);
			lv_obj_add_style(ir_menu->btns[ir_menu->size], &ir_menu->btn_style, 0);

			// Create and format text label
			lv_obj_t *lbl = lv_obj_get_child(ir_menu->btns[ir_menu->size], 0);
			lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
			lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);

			ir_menu->size++;
		}
		
		// Go back
		ui_menu->page = INFRARED_PAGE;
	}
}

void lcd_ir_setup_page(ir_menu_t *menu)
{
	// Create list
	menu->main_list = lv_list_create(ACTIVE_SCR);
	lv_obj_set_size(menu->main_list, 105, 208);
	
	// Format
	lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF);	 // never draw bars
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
	
	
	// Hide for now
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_ir_build_current_menu(ir_menu_t *menu, size_t c)
{
	// Already only called within xInfraredDataMutex
	
	// Clear existing buttons
	lv_obj_clean(menu->main_list); // Deletes children

	// Reset to basic
	menu->index = 0;
	menu->size = 3 + remotes[c].num_signals;
	
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Building '%u' signals for remote '%u'", remotes[c].num_signals, c);
	#endif

	// Set remote name button
	menu->btns[0] = lv_list_add_btn(menu->main_list, NULL, remotes[c].name);
	lv_obj_set_size(menu->btns[0], 100, 28);
	lv_obj_add_style(menu->btns[0], &menu->name_sel_style, 0); // Selected
	
	lv_obj_t *lbl = lv_obj_get_child(menu->btns[0], 0);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
	lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);

	// Add New button
	menu->btns[1] = lv_list_add_btn(menu->main_list, NULL, "Edit");
	lv_obj_set_size(menu->btns[1], 100, 28);
	lv_obj_add_style(menu->btns[1], &menu->btn_style, 0);
	
	lbl = lv_obj_get_child(menu->btns[1], 0);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
	lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);

	// Edit button
	menu->btns[2] = lv_list_add_btn(menu->main_list, NULL, "Add New");
	lv_obj_set_size(menu->btns[2], 100, 28);
	lv_obj_add_style(menu->btns[2], &menu->btn_style, 0);
	
	lbl = lv_obj_get_child(menu->btns[2], 0);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
	lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);

	// Create signal buttons
	for (size_t i = 0; i < remotes[c].num_signals; i++) {
		// Add button with signal name
		menu->btns[3 + i] = lv_list_add_btn(menu->main_list, NULL, remotes[c].signal_names[i]); // Start after default options
		
		// Style
		lv_obj_set_size(menu->btns[3 + i], 100, 28);
		lv_obj_add_style(menu->btns[3 + i], &menu->btn_style, 0);
		
		// Style label
		lbl = lv_obj_get_child(menu->btns[3 + i], 0);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
		lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
	}

	// Format as container
	menu->cont = lv_obj_get_parent(menu->btns[0]);
	lv_obj_set_flex_flow(menu->cont, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Scroll to top
	lv_obj_scroll_to_view(menu->btns[0], LV_ANIM_OFF);
}

void lcd_ir_update_menu(ir_menu_t *ir_menu)
{	
	// Reveal
	lv_obj_remove_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);

	// Wrap index
	if (ir_menu->index >= ir_menu->size) {
		ir_menu->index = 0;
	}
	else if (ir_menu->index < 0) {
		ir_menu->index = ir_menu->size - 1;
	}

	// Reset every button to unselected
	for (int i = 0; i < ir_menu->size; i++) {
		// If remote name
		if (i == 0) {
			lv_obj_remove_style(ir_menu->btns[i], &ir_menu->name_sel_style, 0);
			lv_obj_add_style(ir_menu->btns[i], &ir_menu->name_style, 0);
		}
		// Else signal name
		else {
			lv_obj_remove_style(ir_menu->btns[i], &ir_menu->sel_style, 0);
			lv_obj_add_style(ir_menu->btns[i], &ir_menu->btn_style, 0);
		}
	}
	
	/* Highlight only the current index */
	// If remote name
	if (ir_menu->index == 0) {
		lv_obj_remove_style(ir_menu->btns[ir_menu->index], &ir_menu->name_style, 0);
		lv_obj_add_style(ir_menu->btns[ir_menu->index], &ir_menu->name_sel_style, 0);
	}
	// Else signal name
	else {
		lv_obj_remove_style(ir_menu->btns[ir_menu->index], &ir_menu->btn_style, 0);
		lv_obj_add_style(ir_menu->btns[ir_menu->index], &ir_menu->sel_style, 0);
	}

	// Enable scrolling
	lv_obj_scroll_to_view(ir_menu->btns[ir_menu->index], LV_ANIM_OFF); // LV_ANIM_ON
}

void lcd_ir_save_new_signal(ui_menu_t *ui_menu, ir_menu_t *ir_menu)
{
	// Hide IR menu
	lv_obj_add_flag(ir_menu->main_list, LV_OBJ_FLAG_HIDDEN);
	
	// Hide arrows
	lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
	// Restart infrared RX
	xSemaphoreGive(xInfraredStartRxSemaphore);
	
	// Create label
	lv_obj_t *lbl_ins = lv_label_create(ACTIVE_SCR);
	lcd_format_label(lbl_ins, "Point your device at the\nIR lens and send the signal.", user_secondary_color,
			 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 13);
				 
	// Create present signal img
	lv_obj_t *img_save_remote = lv_img_create(ACTIVE_SCR);
	lv_img_set_src(img_save_remote, &img_save_new_remote);
	lv_obj_align(img_save_remote, LV_ALIGN_CENTER, 0, 25);
	
	lv_timer_handler(); // Show
	
	// Wait until signal received and saved	
	while (1) {
		// Signal received and saved successfully
		if (xSemaphoreTake(xInfraredSignalSavedSemaphore, 0) == pdTRUE) {
			lv_obj_delete(img_save_remote); // Delete img
			
			// "Saving.." text
			lv_obj_center(lbl_ins);
			lv_obj_set_style_text_font(lbl_ins, &lv_font_montserrat_24, 0);
			lv_label_set_text(lbl_ins, "Saving...");
			lv_timer_handler(); // Show
			
			// Wait then clear
			vTaskDelay(pdMS_TO_TICKS(500));
			lv_obj_delete(lbl_ins);
			
			// Show arrows
			lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			lcd_clear_pending_inputs = true; // Clear any false inputs
			
			// Switch to naming page
			ui_menu->page = INFRARED_REMOTE_NAME_PAGE;
			
			break;
		}
		
		// User hit cancel
		if (xSemaphoreTake(xLeftButtonSemaphore, 0)) {
			xSemaphoreGive(xInfraredDisableSemaphore); // Disable IR
			
			// Delete objects
			lv_obj_delete(lbl_ins);
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
		
		vTaskDelay(pdMS_TO_TICKS(20));
		lv_timer_handler();
	}
}