#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"

#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"

#include "core/lv_obj_pos.h"
#include "font/lv_symbol_def.h"
#include "core/lv_obj.h"
#include "misc/lv_area.h"
#include "misc/lv_timer.h"
#include "widgets/label/lv_label.h"

#include "lcd_asset_macros.h"
#include "lcd_utils.h"

#include "srs_memory.h"

#include "img_coin_heads.h"
#include "img_coin_tails.h"

#define TAG "LCD_TOOLS_FUNCS"


tools_menu_t tools_menu = {
	.options = {"Coin flipper", "Dice roller", "Read the docs", "Number generator", "Memory Assistant"},
	.size = 5,
	.index = 0,
	.cont = NULL,
};

void lcd_tools_setup_page(tools_menu_t *menu)
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

void lcd_tools_update_menu(tools_menu_t *menu)
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

void lcd_tools_coin_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	#define NUM_FLIPS 17
	#define FLIP_DELAY 30
	
	// Statics
	static bool do_once = false;
	
	static lv_obj_t *lbl_ins;
	static lv_obj_t *lbl_result;
	static lv_obj_t *coin_heads;
	static lv_obj_t *coin_tails;
	
	// Only execute once
	if (!do_once) {
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "Press select to flip!", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);
					 
		lbl_result = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_result, "Ready", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_CENTER, 62, 16);
		
		// Create coin image
		coin_heads = lv_img_create(ACTIVE_SCR);
		lv_img_set_src(coin_heads, &img_coin_heads);
		lv_obj_align(coin_heads, LV_ALIGN_CENTER, -40, 16);
		
		coin_tails = lv_img_create(ACTIVE_SCR);
		lv_img_set_src(coin_tails, &img_coin_tails);
		lv_obj_align(coin_tails, LV_ALIGN_CENTER, -40, 16);
		lv_obj_add_flag(coin_tails, LV_OBJ_FLAG_HIDDEN); // Hide for now
		
		do_once = true;
	}
	
	// Flip the coin
	if (ui_btns->select_btn == 1) {		
		lv_label_set_text(lbl_result, "Flipping...");
		
		uint32_t one_or_zero = esp_random() % 2;
		
		// Animate
		for (int i = 0; i < (NUM_FLIPS + one_or_zero); i++) {
			if (i % 2 == 0) {
				lv_obj_add_flag(coin_heads, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(coin_tails, LV_OBJ_FLAG_HIDDEN);
			}
			else {
				lv_obj_remove_flag(coin_heads, LV_OBJ_FLAG_HIDDEN);
				lv_obj_add_flag(coin_tails, LV_OBJ_FLAG_HIDDEN);
			}
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(FLIP_DELAY));
		}
		
		if (one_or_zero == 0) {
			lv_label_set_text(lbl_result, "Tails!");
		}
		else {
			lv_label_set_text(lbl_result, "Heads!");
		}
		lv_timer_handler();
		
		lcd_clear_pending_inputs = true; // In case button pressed while looping
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_result);
		lv_obj_delete(coin_heads);
		lv_obj_delete(coin_tails);
		
		// Reset statics
		do_once = false;
		lbl_ins = NULL;
		lbl_result = NULL;
		coin_heads = NULL;
		coin_tails = NULL;
		
		// Show tools list
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_result);
		lv_obj_delete(coin_heads);
		lv_obj_delete(coin_tails);
		
		// Reset statics
		do_once = false;
		lbl_ins = NULL;
		lbl_result = NULL;
		coin_heads = NULL;
		coin_tails = NULL;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_docs_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	#define MAX_QRS 2
	#define QR_COM_TXT "Homepage:"
	#define QR_DOCS_TXT "How to docs:"
	
	// Statics
	static bool do_once = false;
	static uint8_t qr_idx = 0;
	
	static lv_obj_t *lbl_ins;
	static lv_obj_t *qr_active;
	
	// Only execute once
	if (!do_once) {
		qr_idx = 0;
		
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, QR_COM_TXT, user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 7);
		
		// Create QR
		qr_active = lv_img_create(ACTIVE_SCR);
		lv_img_set_src(qr_active, QR_PC5_COM);
		lv_obj_align(qr_active, LV_ALIGN_CENTER, 0, 13);
		
		do_once = true;
	}
	
	// Go right a QR
	if (ui_btns->right_btn == 1) {	
		// Increment with wrap
		qr_idx = (qr_idx + 1) % MAX_QRS;
		
		if (qr_idx == 0) {
			lv_label_set_text(lbl_ins, QR_COM_TXT);
			lv_img_set_src(qr_active, QR_PC5_COM);
		}
		else if (qr_idx == 1) {
			lv_label_set_text(lbl_ins, QR_DOCS_TXT);
			lv_img_set_src(qr_active, QR_PC5_DOCS);
		}
	}
	// Go left a QR
	else if (ui_btns->left_btn == 1 && qr_idx != 0) {	
		// De-increment with wrap
		qr_idx = (qr_idx + MAX_QRS - 1) % MAX_QRS;
		
		if (qr_idx == 0) {
			lv_label_set_text(lbl_ins, QR_COM_TXT);
			lv_img_set_src(qr_active, QR_PC5_COM);
		}
		else if (qr_idx == 1) {
			lv_label_set_text(lbl_ins, QR_DOCS_TXT);
			lv_img_set_src(qr_active, QR_PC5_DOCS);
		}
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(qr_active);
		
		// Reset statics
		do_once = false;
		lbl_ins = NULL;
		qr_active = NULL;
		
		// Show tools list
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show up/down arrow
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(qr_active);
		
		// Reset statics
		do_once = false;
		lbl_ins = NULL;
		qr_active = NULL;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_dice_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{	
	#define X_POS 68
	#define Y_POS 40
	#define BUF_SIZE 4
	#define NUM_IMGS 6
	#define ANIM_DELAY 30
	#define DICE_SCROLL_DIS 40
	
	// Statics
	static bool do_once = false;
	static uint8_t user_idx = 0;
	static uint8_t dice = 1;
	static uint8_t sides = 6;
	
	static lv_obj_t *lbl_ins;
	static lv_obj_t *lbl_dice;
	static lv_obj_t *lbl_sides;
	static lv_obj_t *lbl_num_dice;
	static lv_obj_t *lbl_num_sides;
	static lv_obj_t *lbl_pointer;
	static lv_obj_t *lbl_result;
	static lv_obj_t *img_dice;
	
	static lv_obj_t *cont_roll_log;
	static lv_obj_t *lbl_roll_log;
	static char roll_log_buf[2048];
	
	static lv_style_t style_dice;
	
	// Only execute once
	if (!do_once) {
		user_idx = 0;
		roll_log_buf[0] = 0; // Write null terminator into first element
		
		// Create dice img
		img_dice = lv_img_create(ACTIVE_SCR);
		lv_img_set_src(img_dice, IMG_DICE_2);
		lv_obj_align(img_dice, LV_ALIGN_CENTER, -65, 0);
		
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "Press select to roll!", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);
					 
		lbl_dice = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_dice, "Dice\n", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, X_POS - 65, Y_POS);
					 
		lbl_sides = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_sides, "Sides\n", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, X_POS, Y_POS);
					 
		char buf[BUF_SIZE];
		snprintf(buf, sizeof(buf), "%u", dice);
		lbl_num_dice = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_num_dice, buf, user_secondary_color,
					 &lv_font_montserrat_24, LV_ALIGN_TOP_MID, X_POS - 65, Y_POS + 25);
					 
		snprintf(buf, sizeof(buf), "%u", sides);
		lbl_num_sides = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_num_sides, buf, user_secondary_color,
					 &lv_font_montserrat_24, LV_ALIGN_TOP_MID, X_POS, Y_POS + 25);
					 
		lbl_pointer = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_pointer, LV_SYMBOL_EJECT, user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, X_POS - 65, Y_POS + 58);
					 
		lbl_result = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_result, "", user_secondary_color,
					 &lv_font_montserrat_22, LV_ALIGN_BOTTOM_LEFT, 15, -14);		 
		
		// Create a style for dice boxes
		lv_style_reset(&style_dice); // Reset
		lv_style_init(&style_dice); // Init
		
		lv_style_set_radius(&style_dice, 8);
		lv_style_set_bg_color(&style_dice, user_primary_color);
		lv_style_set_border_width(&style_dice, 2);
		lv_style_set_border_color(&style_dice, user_secondary_color);
		lv_style_set_border_side(&style_dice, LV_BORDER_SIDE_FULL);
		lv_style_set_text_color(&style_dice, user_secondary_color);
		
		lv_style_set_pad_left(&style_dice, 6);
		lv_style_set_pad_right(&style_dice, 6);
		lv_style_set_pad_top(&style_dice, 4);
		lv_style_set_pad_bottom(&style_dice, 4);
		
		lv_obj_add_style(lbl_sides, &style_dice, 0);
		lv_obj_add_style(lbl_dice, &style_dice, 0);
		
		// Create a scrollable log container for each roll result
		cont_roll_log = lv_obj_create(ACTIVE_SCR);
		lv_obj_set_size(cont_roll_log, 210, 106);
		lv_obj_set_style_bg_color(cont_roll_log, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_align(cont_roll_log, LV_ALIGN_CENTER, 0, 0);
		// Format
		lv_obj_set_scroll_dir(cont_roll_log, LV_DIR_VER);
		lv_obj_set_scrollbar_mode(cont_roll_log, LV_SCROLLBAR_MODE_AUTO);
		lv_obj_set_style_pad_top(cont_roll_log, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_bottom(cont_roll_log, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_left(cont_roll_log, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_right(cont_roll_log, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
	
		// Create the history log
		lbl_roll_log = lv_label_create(cont_roll_log);
		lv_obj_set_style_text_font(lbl_roll_log, &lv_font_montserrat_16, 0);
		lv_obj_set_style_text_color(lbl_roll_log, user_secondary_color, 0); 
		lv_label_set_long_mode(lbl_roll_log, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(lbl_roll_log, 180);
		lv_label_set_text(lbl_roll_log, roll_log_buf);
		
		lv_obj_add_flag(cont_roll_log, LV_OBJ_FLAG_HIDDEN); // Hide history cont
						
		do_once = true;
	}
	
	// Roll the dice
	if (ui_btns->select_btn == 1) {		
		uint32_t zero_to_five = esp_random() % NUM_IMGS; // Random end frame
		
		// Animate
		for (int i = 0; i < (15 + zero_to_five); i++) {
			if (i % NUM_IMGS == 0) {
				lv_img_set_src(img_dice, IMG_DICE_1);
			}
			else if (i % NUM_IMGS == 1) {
				lv_img_set_src(img_dice, IMG_DICE_2);
			}
			else if (i % NUM_IMGS == 2) {
				lv_img_set_src(img_dice, IMG_DICE_3);
			}
			else if (i % NUM_IMGS == 3) {
				lv_img_set_src(img_dice, IMG_DICE_4);
			}
			else if (i % NUM_IMGS == 4) {
				lv_img_set_src(img_dice, IMG_DICE_5);
			}
			else if (i % NUM_IMGS == 5) {
				lv_img_set_src(img_dice, IMG_DICE_6);
			}
			
			lv_timer_handler();
			vTaskDelay(pdMS_TO_TICKS(ANIM_DELAY));
		}
		
		roll_log_buf[0] = 0; // Clear log
		
		uint16_t total = 0;
		
		for (int i = 0; i < dice; i++) {
			uint8_t roll = (esp_random() % sides) + 1; // 0 to (sides - 1) -> 1 to sides
			
			total += roll;
			
			// Combine roll results for log
			char tmp[12];
			if (i == dice - 1) { // Last one
				snprintf(tmp, sizeof(tmp), "%u = %u", roll, total);
			}
			else {
				snprintf(tmp, sizeof(tmp), "%u + ", roll);
			}
			strlcat(roll_log_buf, tmp, sizeof(roll_log_buf));
		}
		
		// Format and display new value
		char buf[8];
		snprintf(buf, sizeof(buf), "= %" PRIu16, total);
		lv_label_set_text(lbl_result, buf);
		
		// Set log text
		lv_label_set_text(lbl_roll_log, roll_log_buf);
		
		lcd_clear_pending_inputs = true; // In case button pressed while looping
	}
	// Go right
	else if (ui_btns->right_btn == 1) {	
		// If on dice, move to sides
		if (user_idx == 0) {
			lv_obj_set_x(lbl_pointer, X_POS);
			user_idx = 1;
		}
		else if (user_idx == 1) { // Sides to log
			lv_obj_remove_flag(cont_roll_log, LV_OBJ_FLAG_HIDDEN);
			user_idx = 2;
		}
		else { // user_idx == 2: Log to dice
			lv_obj_add_flag(cont_roll_log, LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_x(lbl_pointer, X_POS - 65);
			user_idx = 0;
		}
	}
	// Move left
	else if (ui_btns->left_btn == 1 && user_idx != 0) {	
		// If on cont, move to sides
		if (user_idx == 2) {
			lv_obj_add_flag(cont_roll_log, LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_x(lbl_pointer, X_POS);
			user_idx = 1;
		}
		else { // Sides to dice
			lv_obj_set_x(lbl_pointer, X_POS - 65);
			user_idx = 0;
		}
	}
	else if (ui_btns->up_btn == 1) {
		// If on dice
		if (user_idx == 0) {
			dice++;
			
			// Can't be 0
			if (dice == 0) {
				dice = 1;
			}
		
			// Format and display new value
			char buf[BUF_SIZE];
			snprintf(buf, sizeof(buf), "%u", dice);
			lv_label_set_text(lbl_num_dice, buf);
		}
		else if (user_idx == 1) { // If on sides
			sides++;
			
			// Can't be 0
			if (sides == 0) {
				sides = 1;
			}
		
			// Format and display new value
			char buf[BUF_SIZE];
			snprintf(buf, sizeof(buf), "%u", sides);
			lv_label_set_text(lbl_num_sides, buf);
		}
		else { // Log
			lv_obj_scroll_by_bounded(cont_roll_log, 0, DICE_SCROLL_DIS, LV_ANIM_ON);
		}
	}
	else if (ui_btns->down_btn == 1) {
		// If on dice
		if (user_idx == 0) {
			dice--;
			
			// Can't be 0
			if (dice == 0) {
				dice = 255;
			}
		
			// Format and display new value
			char buf[BUF_SIZE];
			snprintf(buf, sizeof(buf), "%u", dice);
			lv_label_set_text(lbl_num_dice, buf);
		}
		else if (user_idx == 1) { // If on sides
			sides--;
			
			// Can't be 0
			if (sides == 0) {
				sides = 255;
			}
		
			// Format and display new value
			char buf[BUF_SIZE];
			snprintf(buf, sizeof(buf), "%u", sides);
			lv_label_set_text(lbl_num_sides, buf);
		}
		else { // Log
			lv_obj_scroll_by_bounded(cont_roll_log, 0, -DICE_SCROLL_DIS, LV_ANIM_ON);
		}
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_dice);
		lv_obj_delete(lbl_sides);
		lv_obj_delete(lbl_num_dice);
		lv_obj_delete(lbl_num_sides);
		lv_obj_delete(lbl_pointer);
		lv_obj_delete(img_dice);
		lv_obj_delete(lbl_result);
		lv_obj_delete(lbl_roll_log);
		lv_obj_delete(cont_roll_log);
		
		// Remove styles
		lv_obj_remove_style_all(lbl_sides);
		lv_obj_remove_style_all(lbl_dice);
		
		// Reset statics
		lbl_ins = lbl_dice = lbl_sides = lbl_num_dice = lbl_num_sides = lbl_pointer = img_dice = lbl_result = lbl_roll_log = cont_roll_log = NULL;
		do_once = false;
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Show tools list
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_dice);
		lv_obj_delete(lbl_sides);
		lv_obj_delete(lbl_num_dice);
		lv_obj_delete(lbl_num_sides);
		lv_obj_delete(lbl_pointer);
		lv_obj_delete(img_dice);
		lv_obj_delete(lbl_result);
		lv_obj_delete(lbl_roll_log);
		lv_obj_delete(cont_roll_log);
		
		// Remove styles
		lv_obj_remove_style_all(lbl_sides);
		lv_obj_remove_style_all(lbl_dice);
		
		// Reset statics
		lbl_ins = lbl_dice = lbl_sides = lbl_num_dice = lbl_num_sides = lbl_pointer = img_dice = lbl_result = lbl_roll_log = cont_roll_log = NULL;
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_num_gen_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{	
	#define NUM_GEN_X_POS 54
	#define NUM_GEN_Y_POS 40
	#define NUM_GEN_BUF_SIZE 8
	#define NUM_GEN_X_OFFSET 110

	// Statics
	static bool do_once = false;
	static uint8_t user_idx = 0; // 0 = Min, 1 = Max
	static int16_t min_val = 1;
	static int16_t max_val = 10;

	static lv_obj_t *lbl_ins;
	static lv_obj_t *lbl_min;
	static lv_obj_t *lbl_max;
	static lv_obj_t *lbl_val_min;
	static lv_obj_t *lbl_val_max;
	static lv_obj_t *lbl_pointer;
	static lv_obj_t *lbl_result;

	static lv_style_t style_box;

	// Only execute once
	if (!do_once) {
		// Default values
		user_idx = 0;
		min_val = 1;
		max_val = 100;

		// Instruction label
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "Press select to generate!", user_secondary_color,
						 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

		// Headings
		lbl_min = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_min, "Min\n", user_secondary_color,
						 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, NUM_GEN_X_POS - NUM_GEN_X_OFFSET, NUM_GEN_Y_POS);

		lbl_max = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_max, "Max\n", user_secondary_color,
						 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, NUM_GEN_X_POS, NUM_GEN_Y_POS);

		// Values
		char buf[NUM_GEN_BUF_SIZE];
		snprintf(buf, sizeof(buf), "%d", min_val);
		lbl_val_min = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_val_min, buf, user_secondary_color,
				&lv_font_montserrat_24, LV_ALIGN_TOP_MID, NUM_GEN_X_POS - NUM_GEN_X_OFFSET, NUM_GEN_Y_POS + 25);

		snprintf(buf, sizeof(buf), "%d", max_val);
		lbl_val_max = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_val_max, buf, user_secondary_color,
				&lv_font_montserrat_24, LV_ALIGN_TOP_MID, NUM_GEN_X_POS, NUM_GEN_Y_POS + 25);

		// Pointer
		lbl_pointer = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_pointer, LV_SYMBOL_EJECT, user_secondary_color,
				&lv_font_montserrat_18, LV_ALIGN_TOP_MID, NUM_GEN_X_POS - NUM_GEN_X_OFFSET, NUM_GEN_Y_POS + 58);

		// Result
		lbl_result = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_result, "", user_secondary_color,
				&lv_font_montserrat_22, LV_ALIGN_BOTTOM_MID, 0, -14);

		// Box style for headings
		lv_style_reset(&style_box);
		lv_style_init(&style_box);
		lv_style_set_radius(&style_box, 8);
		lv_style_set_bg_color(&style_box, user_primary_color);
		lv_style_set_border_width(&style_box, 2);
		lv_style_set_border_color(&style_box, user_secondary_color);
		lv_style_set_border_side(&style_box, LV_BORDER_SIDE_FULL);
		lv_style_set_text_color(&style_box, user_secondary_color);
		lv_style_set_pad_left(&style_box, 25);
		lv_style_set_pad_right(&style_box, 25);
		lv_style_set_pad_top(&style_box, 4);
		lv_style_set_pad_bottom(&style_box, 4);

		lv_obj_add_style(lbl_min, &style_box, 0);
		lv_obj_add_style(lbl_max, &style_box, 0);

		do_once = true;
	}

	// Generate number
	if (ui_btns->select_btn == 1) {
		// Ensure min <= max (swap if needed)
		if (max_val < min_val) {
			int16_t tmp = min_val;
			min_val = max_val;
			max_val = tmp;

			char buf_a[NUM_GEN_BUF_SIZE], buf_b[NUM_GEN_BUF_SIZE];
			snprintf(buf_a, sizeof(buf_a), "%d", min_val);
			snprintf(buf_b, sizeof(buf_b), "%d", max_val);
			lv_label_set_text(lbl_val_min, buf_a);
			lv_label_set_text(lbl_val_max, buf_b);
		}

		uint32_t range = (uint32_t)(max_val - min_val + 1);
		uint32_t r = esp_random() % range;
		int32_t result = (int32_t)min_val + (int32_t)r;

		char out[NUM_GEN_BUF_SIZE + 2];
		snprintf(out, sizeof(out), "= %ld", (long)result);
		lv_label_set_text(lbl_result, out);
		lv_obj_align(lbl_result, LV_ALIGN_BOTTOM_MID, 0, -14);
	}
	// Move right (toggle Min/Max)
	else if (ui_btns->right_btn == 1) {
		// Point to max
		if (user_idx == 0) {
			lv_obj_set_x(lbl_pointer, NUM_GEN_X_POS);
			user_idx = 1;
		}
		// Back to min
		else {
			lv_obj_set_x(lbl_pointer, NUM_GEN_X_POS - NUM_GEN_X_OFFSET);
			user_idx = 0;
		}
	}
	// Move left inside page (only if currently on Max)
	else if (ui_btns->left_btn == 1 && user_idx != 0) {
		lv_obj_set_x(lbl_pointer, NUM_GEN_X_POS - NUM_GEN_X_OFFSET);
		user_idx = 0;
	}
	// Increment value
	else if (ui_btns->up_btn == 1) {
		if (user_idx == 0) {
			min_val++;
			char buf[NUM_GEN_BUF_SIZE];
			snprintf(buf, sizeof(buf), "%d", min_val);
			lv_label_set_text(lbl_val_min, buf);
		}
		else {
			max_val++;
			char buf[NUM_GEN_BUF_SIZE];
			snprintf(buf, sizeof(buf), "%d", max_val);
			lv_label_set_text(lbl_val_max, buf);
		}
	}
	// Decrement value
	else if (ui_btns->down_btn == 1) {
		if (user_idx == 0) {
			min_val--;	
			char buf[NUM_GEN_BUF_SIZE];
			snprintf(buf, sizeof(buf), "%d", min_val);
			lv_label_set_text(lbl_val_min, buf);
		}
		else {
			max_val--;
			char buf[NUM_GEN_BUF_SIZE];
			snprintf(buf, sizeof(buf), "%d", max_val);
			lv_label_set_text(lbl_val_max, buf);
		}
	}
	// Back selected and pointer is on min
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_min);
		lv_obj_delete(lbl_max);
		lv_obj_delete(lbl_val_min);
		lv_obj_delete(lbl_val_max);
		lv_obj_delete(lbl_pointer);
		lv_obj_delete(lbl_result);

		// Remove styles
		lv_obj_remove_style_all(lbl_min);
		lv_obj_remove_style_all(lbl_max);

		// Reset statics
		do_once = false;
		lbl_ins = lbl_min = lbl_max = lbl_val_min = lbl_val_max = lbl_pointer = lbl_result = NULL;

		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Show tools list and go back
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_min);
		lv_obj_delete(lbl_max);
		lv_obj_delete(lbl_val_min);
		lv_obj_delete(lbl_val_max);
		lv_obj_delete(lbl_pointer);
		lv_obj_delete(lbl_result);

		// Remove styles
		lv_obj_remove_style_all(lbl_min);
		lv_obj_remove_style_all(lbl_max);

		// Reset statics
		do_once = false;
		lbl_ins = lbl_min = lbl_max = lbl_val_min = lbl_val_max = lbl_pointer = lbl_result = NULL;

		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_memory_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	// Statics
	static bool do_once = false;
	static lv_obj_t *lbl_title, *lbl_help, *lbl_list[5], *lbl_hint;
	static int sel = 0; // Selection cursor inside due list
	static int due_total = 0; // Total due today (not just displayed)
	static int due_vis = 0; // How many we're displaying (<= 5)
	static int due_idx[8]; // Workspace of indices (store more than shown)
	static uint32_t today = 0;

	// Initialize
	if (!do_once) {
		// Check time and sync if needed
		srs_sync_time_over_wifi();

		// Load saved data
		srs_nvs_load();

		// Build UI
		lbl_title = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_title, "Memory Assistant", user_secondary_color,
				&lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 6);

		lbl_help = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_help, "SELECT - Mark Done    RIGHT - Add" LV_SYMBOL_RIGHT, user_secondary_color, 
				&lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -6);

		for (int i = 0; i < 5; ++i) {
			lbl_list[i] = lv_label_create(ACTIVE_SCR);
			lcd_format_label(lbl_list[i], "", user_secondary_color,
					&lv_font_montserrat_16, LV_ALIGN_LEFT_MID, 10, -28 + (i * 18));
		}

		lbl_hint = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_hint, "", user_secondary_color,
				&lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 8, -24);

		do_once = true;
		sel = 0;
	}

	// Every tick: recompute "today" and the due queue
	today = srs_days_since_epoch_local();
	due_total = srs_build_due_list(due_idx, MAX(due_vis, 8), today);

	// Render list (up to 5 rows)
	due_vis = (due_total > 5) ? 5 : due_total;
	for (int i = 0; i < 5; ++i) {
		if (i < due_vis) {
			int idx = due_idx[i];
			int need = (int)srs_days[srs_tbl[idx].step];
			int waited = (int)(today - srs_tbl[idx].last_day);
			char line[48];
			snprintf(line, sizeof(line), "%c Pg %u  (+%dd / need %dd)",
					 (i == sel) ? '>' : ' ', srs_tbl[idx].page, waited, need);
			lv_label_set_text(lbl_list[i], line);
			lv_obj_clear_flag(lbl_list[i], LV_OBJ_FLAG_HIDDEN);
		}
		else {
			lv_obj_add_flag(lbl_list[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (due_total == 0) {
		lv_label_set_text(lbl_hint, "Nothing due. Add today's page with >");
	}
	else {
		char msg[48];
		snprintf(msg, sizeof(msg), "%d due", due_total);
		lv_label_set_text(lbl_hint, msg);
	}

	// ---------- Input handling ----------
	if (ui_btns->up_btn == 1 && due_vis > 0) {
		sel = (sel - 1 + due_vis) % due_vis;
	}
	else if (ui_btns->down_btn == 1 && due_vis > 0) {
		sel = (sel + 1) % due_vis;
	}
	// Mark reviewed
	else if (ui_btns->select_btn == 1 && due_vis > 0) {
		srs_mark_reviewed_index(due_idx[sel], today);
		// Keep selection sensible
		if (sel >= due_vis - 1) sel = MAX(0, due_vis - 2);
	}
	// Add today’s page (auto-increment page number)
	else if (ui_btns->right_btn == 1) {
		uint16_t p = srs_next_default_page();
		srs_add_or_reset(p, today);

		char added[48];
		snprintf(added, sizeof(added), "Added Pg %u", p);
		lv_label_set_text(lbl_hint, added);
	}
	// Back out
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_help);
		lv_obj_delete(lbl_hint);
		for (int i = 0; i < 5; ++i) lv_obj_delete(lbl_list[i]);

		// Restore arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Reveal tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Reset and return
		do_once = false; sel = 0;
		ui_menu->page = TOOLS_PAGE;
	}
	// Home / power → fall back to your common transition
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_help);
		lv_obj_delete(lbl_hint);
		for (int i = 0; i < 5; ++i) lv_obj_delete(lbl_list[i]);

		do_once = false; sel = 0;
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu);
	}
}