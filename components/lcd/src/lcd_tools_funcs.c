#include <time.h>
#include <sys/param.h>

#include "font/lv_font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

#include "gpio_task.h"
#include "misc/lv_color.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "core/lv_obj_pos.h"
#include "font/lv_symbol_def.h"
#include "core/lv_obj.h"
#include "misc/lv_area.h"
#include "misc/lv_timer.h"
#include "widgets/label/lv_label.h"

#include "btc_web_portal.h"
#include "qrcodegen.h" // QR encoder
#include "lcd_asset_macros.h"
#include "lcd_utils.h"

#include "srs_memory.h"

#include "img_coin_heads.h"
#include "img_coin_tails.h"

#define TAG "LCD_TOOLS_FUNCS"

#define HIGH_SCORE_NS "tetris"
#define HIGH_SCORE_KEY "score"

tools_menu_t tools_menu = {
	.options = {"Coin Flipper", "Dice Roller", "Tetris", "Number Generator", "Read the Docs", "Bitcoin QR",
			"Pomodoro Timer", "SRS Planner"},
	.size = 8,
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
	for (int i = 0; i < menu->size; ++i) {

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
		for (int i = 0; i < (NUM_FLIPS + one_or_zero); ++i) {
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
	EXT_RAM_BSS_ATTR static char roll_log_buf[2048];
	
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
		for (int i = 0; i < (15 + zero_to_five); ++i) {
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
		
		for (int i = 0; i < dice; ++i) {
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
		// Remove styles
		lv_obj_remove_style_all(lbl_min);
		lv_obj_remove_style_all(lbl_max);
		
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_min);
		lv_obj_delete(lbl_max);
		lv_obj_delete(lbl_val_min);
		lv_obj_delete(lbl_val_max);
		lv_obj_delete(lbl_pointer);
		lv_obj_delete(lbl_result);

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
		// Remove styles
		lv_obj_remove_style_all(lbl_min);
		lv_obj_remove_style_all(lbl_max);
		
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_min);
		lv_obj_delete(lbl_max);
		lv_obj_delete(lbl_val_min);
		lv_obj_delete(lbl_val_max);
		lv_obj_delete(lbl_pointer);
		lv_obj_delete(lbl_result);

		// Reset statics
		do_once = false;
		lbl_ins = lbl_min = lbl_max = lbl_val_min = lbl_val_max = lbl_pointer = lbl_result = NULL;

		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_how_srs_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	#define HOW_Y_OFFSET 40
	
	// Statics
	static bool init = false;
	static lv_obj_t *cont = NULL;
	static lv_obj_t *title_lbl = NULL;
	static lv_obj_t *instr_lbl = NULL;
	
	if (!init) {
		// Reset long select semaphore to avoid false notebook reset
		xQueueReset(xSelectButtonLongSemaphore);
		
		// Create a scrollable container for the instructions
		cont = lv_obj_create(ACTIVE_SCR);
		lv_obj_set_size(cont, 210, 106);
		lv_obj_center(cont);
		lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
		lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
		lv_obj_set_scroll_dir(cont, LV_DIR_VER);
		lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content

		// Title label
		title_lbl = lv_label_create(cont);
		lv_label_set_text(title_lbl, "How It Works:");
		lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
		lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

		// Instructions label (scrollable if text is long)
		instr_lbl = lv_label_create(cont);
		lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(instr_lbl, lv_pct(100)); // Full width for wrapping
		lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
		lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
		lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

		// Set custom text based on hotkey index
		const char *instr_text = "Press RIGHT to skip. Hold SELECT to forget notebooks.\n\n"
								 "The SRS memory planner is a tool to help you remember new information based on the Ebbinghaus "
								 "forgetting curve (via Spaced Repetition System).\n\nBasically, the more you review things, the "
								 "better you remember them at increasingly impressive intervals.\n\n"
								 "This option will help you keep track of what you need to review on which days to "
								 "optimize your long-term memory retention.\n\nFor more info on how to get started, please see:\n\n"
								 "polycast5.com/blogs /tutorials/srs-memory-planner";
		
		lv_label_set_text(instr_lbl, instr_text);

		lv_timer_handler();

		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, HOW_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -HOW_Y_OFFSET, LV_ANIM_ON);
	}
	// Skip to TOOLS_SRS_PAGE
	else if (ui_btns->right_btn == 1) {
		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
		// Hide up/down arrow
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = TOOLS_SRS_PAGE;
	}
	// Reset notebook
	else if (xSemaphoreTake(xSelectButtonLongSemaphore, 0) == pdTRUE) {
		// Clear SRS NVS
		lcd_ns_nvs_clear(SRS_NS);
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
			
		// Show tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch back
		ui_menu->page = TOOLS_PAGE;
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
		// Show tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch back
		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_pomodoro_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	#define POMODORO_MODE_TXT "%dm work,\n%dm break"
	
	// Enums
	enum { POMODORO_25_5, POMODORO_50_10 };
	enum { POMODORO_PHASE_WORK, POMODORO_PHASE_BREAK };

	// Statics
	static bool init = false;
	static int mode = POMODORO_25_5;
	static int phase = POMODORO_PHASE_WORK;
	static bool running = false;

	static uint32_t work_time = 25 * 60; // sec
	static uint32_t break_time = 5 * 60; // sec
	static uint32_t remaining = 0; // Seconds remaining in current phase
	static TickType_t last_tick = 0;

	static lv_obj_t *lbl_mode = NULL;
	static lv_obj_t *lbl_phase = NULL;
	static lv_obj_t *lbl_time = NULL;
	static lv_obj_t *arc = NULL;

	// Do once
	if (!init) {
		// Arc (full -> empty)
		arc = lv_arc_create(ACTIVE_SCR);
		lv_obj_set_size(arc, 110, 110);
		lv_obj_align(arc, LV_ALIGN_CENTER, -44, 0);
		lv_arc_set_rotation(arc, 270); // Start at top
		lv_arc_set_bg_angles(arc, 0, 360);
		lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL); // Clockwise
		lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
		lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);

		// Hide track, show only indicator
		lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
		lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(arc, user_secondary_color, LV_PART_INDICATOR);
		lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);

		// Percent range
		lv_arc_set_range(arc, 0, 100);
		lv_arc_set_value(arc, 100); // Start full
		
		// Set defaults
		mode = POMODORO_25_5;
		work_time = 25 * 60;
		break_time = 5 * 60;
		phase = POMODORO_PHASE_WORK;
		remaining = work_time;

		// Mode
		lbl_mode = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_mode, POMODORO_MODE_TXT, user_secondary_color,
				&lv_font_montserrat_16, LV_ALIGN_RIGHT_MID, -18, 0);
		
		// Set current work/break time
		lv_label_set_text_fmt(lbl_mode, POMODORO_MODE_TXT, (int)(work_time / 60U), (int)(break_time / 60U));

		// Phase label
		lbl_phase = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_phase, "Work", user_secondary_color,
				&lv_font_montserrat_16, LV_ALIGN_CENTER, -44, -20);

		// Time label
		lbl_time = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_time, "25:00", user_secondary_color,
				&lv_font_montserrat_24, LV_ALIGN_CENTER, -44, 12);

		last_tick = xTaskGetTickCount();
		init = true;
	}

	// If running
	if (running) {
		TickType_t now = xTaskGetTickCount();
		
		// Update every second
		if ((now - last_tick) >= pdMS_TO_TICKS(1000)) {
			last_tick = now;

			// If ticking down
			if (remaining > 0U) {
				remaining--;

				// Update time text
				char buf[16];
				snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(remaining / 60U), (unsigned)(remaining % 60U));
				lv_label_set_text(lbl_time, buf);

				// Update arc as remaining percent (100 -> 0)
				uint32_t total = (phase == POMODORO_PHASE_WORK) ? work_time : break_time;
				uint32_t pct = (total > 0U) ? (remaining * 100U) / total : 0U;
				if (pct > 100U) {
					pct = 100U;
				}
				
				// Set value
				lv_arc_set_value(arc, (int32_t)pct);
			}
			// Else at 0
			else {
				// Auto-advance: flip phase and keep running
				phase = (phase == POMODORO_PHASE_WORK) ? POMODORO_PHASE_BREAK : POMODORO_PHASE_WORK;

				// Update phase
				lv_label_set_text(lbl_phase, (phase == POMODORO_PHASE_WORK) ? "Work" : "Break");
				
				// Set current work/break time
				lv_label_set_text_fmt(lbl_mode, POMODORO_MODE_TXT, (int)(work_time / 60U), (int)(break_time / 60U));

				// Reset timers for new phase and keep going
				uint32_t total = (phase == POMODORO_PHASE_WORK) ? work_time : break_time;
				remaining = total;

				// Arc back to full
				lv_arc_set_value(arc, 100);
				
				// Update time text
				char buf[16];
				snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(remaining / 60U), (unsigned)(remaining % 60U));
				lv_label_set_text(lbl_time, buf);			
			}
		}
	}

	/* User input */
	
	// Start or pause
	if (ui_btns->select_btn) {
		running = !running;
		
		if (running) {
			last_tick = xTaskGetTickCount();
		}
	}
	// Toggle 25/5 <-> 50/10 (if paused)
	else if (ui_btns->right_btn && !running) {
		// Switch to 50 if 25
		if (mode == POMODORO_25_5) {
			mode = POMODORO_50_10;
			work_time = 50 * 60;
			break_time = 10 * 60;
		}
		// Else switch 25 if 50
		else {
			mode = POMODORO_25_5;
			work_time = 25 * 60;
			break_time =  5 * 60;
		}
		
		// Always reset to work when switching presets
		phase = POMODORO_PHASE_WORK;
		remaining = work_time;

		// Update phase
		lv_label_set_text(lbl_phase, "Work");
		
		// Set current work/break time
		lv_label_set_text_fmt(lbl_mode, POMODORO_MODE_TXT, (int)(work_time / 60U), (int)(break_time / 60U));

		// Reset arc
		lv_arc_set_value(arc, 100);
		
		// Update time text
		char buf[16];
		snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(remaining / 60U), (unsigned)(remaining % 60U));
		lv_label_set_text(lbl_time, buf);
	}
	// Adjust current phase by +-1 min (if paused)
	else if ((ui_btns->up_btn || ui_btns->down_btn) && !running) {
		// If up +60s, else -60s
		int32_t delta = ui_btns->up_btn ? +60 : -60;
		
		// Pick which phase length to edit
		uint32_t *phase_ptr = (phase == POMODORO_PHASE_WORK) ? &work_time : &break_time;
		
		// Compute new duration for active phase
		int32_t next = (int32_t)(*phase_ptr) + delta;
		
		// Clamp
		if (next < 60) {
			next = 60;
		}
		if (next > 600 * 60) {
			next = 600 * 60;
		}

		// Commit the new phase length and reset countdown
		*phase_ptr = (uint32_t)next;
		remaining = (uint32_t)next;

		// Reset arc
		lv_arc_set_value(arc, 100);
		
		// Update time text
		char buf[16];
		snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(remaining / 60U), (unsigned)(remaining % 60U));
		lv_label_set_text(lbl_time, buf);
		
		// Set current work/break time
		lv_label_set_text_fmt(lbl_mode, POMODORO_MODE_TXT, (int)(work_time / 60U), (int)(break_time / 60U));
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Delete objects
		lv_obj_delete(lbl_mode);
		lv_obj_delete(lbl_phase);
		lv_obj_delete(lbl_time);
		lv_obj_delete(arc);

		// Reset statics
		lbl_mode = lbl_phase = lbl_time = arc = NULL;
		init = false;
		running = false;

		// Show tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Delete objects
		lv_obj_delete(lbl_mode);
		lv_obj_delete(lbl_phase);
		lv_obj_delete(lbl_time);
		lv_obj_delete(arc);

		// Reset statics
		lbl_mode = lbl_phase = lbl_time = arc = NULL;
		init = false;
		running = false;

 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}


void lcd_tools_srs_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	#define SRS_MAX_TO_SHOW 3
	//#define SRS_CALIBRATING 1
	
	#ifdef SRS_CALIBRATING // To easily add days to retrieve notebook entries
	static int calibrate = -24; // Initial offset (days since start date)
	// Add first then go to next and start clear cycle - make sure to disconnect from port and restart the device first for fresh Wi-Fi fetch
	#else
	static int calibrate = 0; // No offset - present day
	#endif
	
	// Statics
	static bool do_once = false;
	static lv_obj_t *lbl_title, *lbl_help, *lbl_list[SRS_MAX_TO_SHOW], *lbl_hint;
	static int sel = 0; // Selection cursor inside due list
	static int due_total = 0; // Total due today (not just displayed)
	static int due_vis = 0; // How many we're displaying (<= SRS_MAX_TO_SHOW)
	static int due_idx[SRS_NUM_STEPS]; // Workspace of indices (store more than shown)
	static uint32_t today = 0;
	
	// Initialize
	if (!do_once) {
		// Check time and sync if needed
		if (srs_sync_time_over_wifi() == false) {
			/* Exit */
			
			// Hide right arrow
			lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			
			// Show up/down arrow
			lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
	
			// Show tools menu
			lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
	
			ui_menu->page = TOOLS_PAGE;
			
			return;
		}

		// Load saved pages
		srs_nvs_load();

		// Build labels
		lbl_title = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_title, "SRS Planner", user_secondary_color,
				&lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 6);

		lbl_help = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_help, "SELECT - Done      RIGHT - Add", user_secondary_color, 
				&lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -6);

		// Create all lbl_list
		for (int i = 0; i < SRS_MAX_TO_SHOW; ++i) {
			lbl_list[i] = lv_label_create(ACTIVE_SCR);
			lcd_format_label(lbl_list[i], "", user_secondary_color,
					&lv_font_montserrat_16, LV_ALIGN_LEFT_MID, 18, -25 + (i * 18));
		}

		lbl_hint = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_hint, "", user_secondary_color,
				&lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -24);

		do_once = true;
		sel = 0;
	}

	// Recompute today and the due queue
	today = srs_days_since_epoch_local(calibrate);
	
	// Build entries due
	const int cap = (int)(sizeof(due_idx) / sizeof(due_idx[0]));
	due_total = srs_build_due_list(due_idx, cap, today);

	// Render list (SRS_MAX_TO_SHOW rows)
	due_vis = (due_total > SRS_MAX_TO_SHOW) ? SRS_MAX_TO_SHOW : due_total; // Cap at SRS_MAX_TO_SHOW
	for (int i = 0; i < SRS_MAX_TO_SHOW; ++i) {
		// If within visible window -> show
		if (i < due_vis) {
			int idx = due_idx[i]; // Entry to consider
			#ifdef POLYCAST5_DEBUG
			int current_step = (int)srs_days[srs_tbl[idx].step]; // Get the step the entry is on
			#endif
			int days_since_added = (int)(today - srs_tbl[idx].start_day); // How many days have elapsed since added
			
			#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "Pg. %u is on: interval %d | step %d | added %d days ago", srs_tbl[idx].page, current_step, srs_tbl[idx].step, days_since_added);
			#endif
			
			// Format label
			char line[32];
			snprintf(line, sizeof(line), "%c Pg. %u: %d day(s) ago", (i == sel) ? '>' : '<', srs_tbl[idx].page, days_since_added);
			lv_label_set_text(lbl_list[i], line); // Set text
			lv_obj_remove_flag(lbl_list[i], LV_OBJ_FLAG_HIDDEN); // Ensure visible
		}
		else {
			lv_obj_add_flag(lbl_list[i], LV_OBJ_FLAG_HIDDEN); // Else hide for now
		}
	}

	if (due_total == 0) {
		lv_label_set_text(lbl_hint, "You're all caught up!");
	}
	else {
		char msg[48];
		snprintf(msg, sizeof(msg), "%d Page(s) to review", due_total);
		lv_label_set_text(lbl_hint, msg);
	}
	
	#ifdef SRS_CALIBRATING
	if (ui_btns->up_btn == 1) {
		calibrate++; // Move day up to speed run entries
	}
	#endif

	/* User input */
	// Increment selected
	if (ui_btns->up_btn == 1 && due_vis > 0) {
		sel = (sel - 1 + due_vis) % due_vis;
	}
	// Decrement selected
	else if (ui_btns->down_btn == 1 && due_vis > 0) {
		sel = (sel + 1) % due_vis;
	}
	// Mark selected page
	else if (ui_btns->select_btn == 1 && due_vis > 0) {
		srs_mark_reviewed_index(due_idx[sel], today);
		
		// Keep selection sensible
		if (sel >= due_vis - 1) {
			sel = MAX(0, due_vis - 2);
		}
	}
	// Add page selected
	else if (ui_btns->right_btn == 1) {
		// Add the page
		uint16_t p = srs_next_default_page();
		srs_add_or_reset(p, today);

		// User confirmation
		char added[48];
		snprintf(added, sizeof(added), "Added Pg. %u", p);
		lv_label_set_text(lbl_hint, added);
		
		// Show
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(500));
		lcd_clear_pending_inputs = true; // Don't count input while waiting
	}
	// Back out
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_help);
		lv_obj_delete(lbl_hint);
		for (int i = 0; i < SRS_MAX_TO_SHOW; ++i) {
			lv_obj_delete(lbl_list[i]);
			lbl_list[i] = NULL;
		}
		
		// Reset statics
		lbl_title = lbl_help = lbl_hint = NULL;
		do_once = false;
		sel = 0;

		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Show up/down arrow
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

		// Show tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_title);
		lv_obj_delete(lbl_help);
		lv_obj_delete(lbl_hint);
		for (int i = 0; i < SRS_MAX_TO_SHOW; ++i) {
			lv_obj_delete(lbl_list[i]);
			lbl_list[i] = NULL;
		}
		
		// Reset statics
		lbl_title = lbl_help = lbl_hint = NULL;
		do_once = false;
		sel = 0;

		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu);
	}
}

/* =============== BTC Public Address Page =============== */

// Draw text as QR into an LVGL canvas (RGB565) -> returns 0 on success
static int btc_draw_qr(lv_obj_t *canvas, const char *text, int size_px, uint8_t **pbuf)
{
	// Validate args
	if (!canvas || !text || !*text || size_px <= 0 || !pbuf) {
		return -1;
	}

	// Allocate work buffers (heap, prefer PSRAM)
	uint8_t *tmp = (uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	if (!tmp) {
		tmp = (uint8_t*)malloc(qrcodegen_BUFFER_LEN_MAX);
	}
	
	uint8_t *qr = (uint8_t*)heap_caps_malloc(qrcodegen_BUFFER_LEN_MAX, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	if (!qr) {
		qr = (uint8_t*)malloc(qrcodegen_BUFFER_LEN_MAX);
	}
	
	if (!tmp || !qr) {
		free(tmp);
		free(qr);
		return -2;
	}

	// Encode QR
	bool ok = qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
			qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);
			
	// Free tmp buffer after encode
	free(tmp);
	if (!ok) {
		free(qr);
		return -3;
	}

	// Recreate canvas buffer every call (simple & safe)
	size_t bytes = (size_t)size_px * (size_px) * 2; // RGB565
	if (*pbuf) {
		// Free old buffer
		free(*pbuf);
		*pbuf = NULL;
	}
	
	*pbuf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	
	if (!*pbuf) {
		*pbuf = (uint8_t*)malloc(bytes);
	}
	
	if (!*pbuf) {
		free(qr);
		return -4;
	}

	// Bind buffer to canvas
	lv_canvas_set_buffer(canvas, *pbuf, size_px, size_px, LV_COLOR_FORMAT_RGB565);

	// Paint white background (0xFFFF)
	memset(*pbuf, 0xFF, bytes);

	// Compute scale (QR modules -> pixels)
	int qr_sz = qrcodegen_getSize(qr);
	int border = 2;
	int mods = qr_sz + border * 2;
	float scale = (float)size_px / (float)mods;

	// Draw black modules
	for (int my = 0; my < mods; ++my) {
		for (int mx = 0; mx < mods; ++mx) {
			// Get module
			bool dark = false;
			
			int qx = mx - border, qy = my - border;
			
			if (qx >= 0 && qx < qr_sz && qy >= 0 && qy < qr_sz) {
				dark = qrcodegen_getModule(qr, qx, qy);
			}
			
			if (!dark) {
				continue;
			}

			// Module -> pixel box
			int x0 = (int)(mx * scale);
			int y0 = (int)(my * scale);
			
			int x1 = (int)((mx + 1) * scale);
			if (x1 <= x0) {
				x1 = x0 + 1;
			}
			
			int y1 = (int)((my + 1) * scale);
			if (y1 <= y0) {
				y1 = y0 + 1;
			}

			// Fill box black (0x0000)
			for (int y = y0; y < y1 && y < size_px; ++y) {
				uint16_t *row = (uint16_t*)(*pbuf + (size_t)y * (size_px * 2));
				for (int x = x0; x < x1 && x < size_px; ++x) {
					row[x] = 0x0000;
				}
			}
		}
	}

	// Free QR map
	free(qr);
	return 0;
}

// Redraw address label + 80x80 QR on the canvas
static void btc_redraw_qr(lv_obj_t *canvas, uint8_t **pbuf)
{
	char addr[128] = "";

	// Get stored address
	esp_err_t err = btc_addr_get_nvs(addr, sizeof(addr));
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "btc_redraw_qr btc_addr_get failed: %s", esp_err_to_name(err));
	}

	// Draw address-only QR at 80x80
	btc_draw_qr(canvas, addr, 90, pbuf);
}

void lcd_tools_btc_addr_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	#define BTC_ADDR_Y_OFFSET 40
	
	// Statics
	static bool init = false;
	static uint8_t *qr_buf = NULL; // Canvas backing buffer
	
	static lv_obj_t *cont = NULL;
	static lv_obj_t *instr_lbl = NULL;
	static lv_obj_t *canvas	= NULL;
	
	if (!init) {		
		// Create a scrollable container for the instructions
		cont = lv_obj_create(ACTIVE_SCR);
		lv_obj_set_size(cont, 210, 106);
		lv_obj_center(cont);
		lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
		lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
		lv_obj_set_scroll_dir(cont, LV_DIR_VER);
		lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content
		
		// Get stored address
		char btc_addr[128] = ""; // Public address buffer
		esp_err_t err = btc_addr_get_nvs(btc_addr, sizeof(btc_addr));
		
		// If previous address exists
		if (err == ESP_OK) {
			// Create QR canvas
			canvas = lv_canvas_create(cont);
			lv_obj_set_size(canvas, 90, 90); // Fixed size: also change in btc_redraw_qr
			lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);
			// Initial QR draw
			btc_redraw_qr(canvas, &qr_buf);
	
			// Instructions label (scrollable if text is long)
			instr_lbl = lv_label_create(cont);
			lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
			lv_obj_set_width(instr_lbl, lv_pct(100)); // Full width for wrapping
			lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
			lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
			lv_obj_align_to(instr_lbl, canvas, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
	
			// Set custom text based on hotkey index
			const char *instr_text =
									"Current address:\n%s\n\n"
									"This option will allow you to display your public Bitcoin address as a QR code so others can send you "
									"on-chain payments directly.\n\n"
									"Don't worry, the only thing you ever need to show PolyCast5 is your public address which can't be used "
									"to sign transactions. Think of it like a house address to receive mail.\n\n"
									"To get started, click the right arrow button."
									;
	
			lv_label_set_text_fmt(instr_lbl, instr_text, btc_addr);
		}
		else {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGE(TAG, "btc_addr_get failed: %s", esp_err_to_name(err));
			#endif
	
			// Instructions label (scrollable if text is long)
			instr_lbl = lv_label_create(cont);
			lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
			lv_obj_set_width(instr_lbl, lv_pct(100)); // Full width for wrapping
			lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
			lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
			lv_obj_align(instr_lbl, LV_ALIGN_TOP_MID, 0, -2);
	
			// Set custom text based on hotkey index
			const char *instr_text =
									"This option will allow you to display your public Bitcoin address as a QR code so others can send you "
									"on-chain payments directly.\n\n"
									"Don't worry, the only thing you ever need to show PolyCast5 is your public address which can't be used "
									"to sign transactions. Think of it like a house address to receive mail.\n\n"
									"To get started, click the right arrow button."
									;
	
			lv_label_set_text(instr_lbl, instr_text);
		}
		
		lv_timer_handler();

		init = true;
	}
	
	// Scroll up
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, BTC_ADDR_Y_OFFSET, LV_ANIM_ON);
	}
	// Scroll down
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -BTC_ADDR_Y_OFFSET, LV_ANIM_ON);
	}
	// Go to setup
	else if (ui_btns->right_btn == 1) {
		// Free canvas buffer
		if (qr_buf) {
			free(qr_buf);
			qr_buf = NULL;
		}
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		instr_lbl = canvas = NULL;
		init = false;
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch back
		ui_menu->page = TOOLS_BTC_ADDR_SETUP_PAGE;
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Free canvas buffer
		if (qr_buf) {
			free(qr_buf);
			qr_buf = NULL;
		}
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		instr_lbl = canvas = NULL;
		init = false;
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Show tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch back
		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Free canvas buffer
		if (qr_buf) {
			free(qr_buf);
			qr_buf = NULL;
		}
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		instr_lbl = canvas = NULL;
		init = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_tools_btc_addr_setup_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	#define BTC_ADDR_SETUP_Y_OFFSET 40
	
	// Statics
	static bool init = false;
	
	static lv_obj_t *cont = NULL;
	static lv_obj_t *title_lbl = NULL;
	static lv_obj_t *instr_lbl = NULL;
	
	if (!init) {
		// Create a scrollable container for the instructions
		cont = lv_obj_create(ACTIVE_SCR);
		lv_obj_set_size(cont, 210, 106);
		lv_obj_center(cont);
		lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
		lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
		lv_obj_set_scroll_dir(cont, LV_DIR_VER);
		lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content

		// Title label
		title_lbl = lv_label_create(cont);
		lv_label_set_text(title_lbl, "How It Works:");
		lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
		lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

		// Instructions label (scrollable if text is long)
		instr_lbl = lv_label_create(cont);
		lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(instr_lbl, lv_pct(100)); // Full width for wrapping
		lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
		lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
		lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

		// Set custom text based on hotkey index
		const char *instr_text = 
				"How to add your public Bitcoin address:\n\nFirst, grab your phone or other device and navigate to Wi-Fi settings."
				"\n\nThere, you should see a joinable Wi-Fi network named '%s'. Click on it and enter the password '%s'."
				"\n\nIf you don't see it, please wait a minute or try refreshing."
				"\n\nOnce connected, open up your internet browser of choice and search:\n\n%s\n\nFrom there, follow the on-screen instructions. "
				"DO NOT exit this page until you're done entering what you want into the web portal.";
		
		lv_label_set_text_fmt(instr_lbl, instr_text, btc_portal_get_ssid(), btc_portal_get_pass(), btc_portal_get_ip());

		lv_timer_handler();
		
		// Start portal
		esp_err_t err = btc_portal_start();
			
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "btc_portal_start failed: %s", esp_err_to_name(err));
		}

		init = true;
	}
	
	// Scroll up
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, BTC_ADDR_SETUP_Y_OFFSET, LV_ANIM_ON);
	}
	// Scroll down
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -BTC_ADDR_SETUP_Y_OFFSET, LV_ANIM_ON);
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
		// Stop portal
		btc_portal_stop();
		
		// Show tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch back
		ui_menu->page = TOOLS_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
		// Stop portal
		btc_portal_stop();
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

/* =============== TETRIS IMPLEMENTATION =============== */

// Macros for game config
#define ORTHO_SIZE 10 // Number of rows (vertical when held sideways)
#define FALL_SIZE 25 // Total fall length (horizontal on screen)
#define VISIBLE_FALL 21	// Visible fall length
#define HIDDEN_FALL 4 // Hidden start columns (left on screen)
#define CELL_SIZE 10 // Pixel size per cell (210/21 ≈10, 106/10=10.6 ≈10)
#define FALL_DELAY_MS 500 // Base fall speed (decreases with level)
#define SOFT_DROP_MULTIPLIER 5 // Faster fall when down pressed
#define LEVEL_SPEED_INCREASE 50 // MS decrease per level (every 1000 points)

// Game colors
#define COLOR_PIECE_FALLING (lv_color_make(0, 139, 0)) // 8B Green #008B00
#define COLOR_PIECE_RESTING (lv_color_make(0, 71, 171)) // Cobalt blue #0047AB
#define COLOR_PIECE_OUTLINE (lv_color_white())
#define COLOR_BOARD_FILL (lv_color_black())

// Tetromino shapes: 4 rotations each per shape, 4x4 grid
static const int tetrominoes[7][4][16] = {
	// I
 {{0,0,0,0, 1,1,1,1, 0,0,0,0, 0,0,0,0},
	 {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0},
	 {0,0,0,0, 1,1,1,1, 0,0,0,0, 0,0,0,0},
	 {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0}},
	// O
	{{0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0},
	 {0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0},
	 {0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0},
	 {0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0}},
	// T
	{{0,0,0,0, 0,1,0,0, 1,1,1,0, 0,0,0,0},
	 {0,0,1,0, 0,1,1,0, 0,0,1,0, 0,0,0,0},
	 {0,0,0,0, 1,1,1,0, 0,1,0,0, 0,0,0,0},
	 {0,1,0,0, 0,1,1,0, 0,1,0,0, 0,0,0,0}},
	// S
	{{0,0,0,0, 0,1,1,0, 1,1,0,0, 0,0,0,0},
	 {0,1,0,0, 0,1,1,0, 0,0,1,0, 0,0,0,0},
	 {0,0,0,0, 0,1,1,0, 1,1,0,0, 0,0,0,0},
	 {0,1,0,0, 0,1,1,0, 0,0,1,0, 0,0,0,0}},
	// Z
	{{0,0,0,0, 1,1,0,0, 0,1,1,0, 0,0,0,0},
	 {0,0,1,0, 0,1,1,0, 0,1,0,0, 0,0,0,0},
	 {0,0,0,0, 1,1,0,0, 0,1,1,0, 0,0,0,0},
	 {0,0,1,0, 0,1,1,0, 0,1,0,0, 0,0,0,0}},
	// J
	{{0,0,0,0, 1,0,0,0, 1,1,1,0, 0,0,0,0},
	 {0,1,1,0, 0,1,0,0, 0,1,0,0, 0,0,0,0},
	 {0,0,0,0, 1,1,1,0, 0,0,1,0, 0,0,0,0},
	 {0,0,1,0, 0,0,1,0, 0,1,1,0, 0,0,0,0}},
	// L
	{{0,0,0,0, 0,0,1,0, 1,1,1,0, 0,0,0,0},
	 {0,1,0,0, 0,1,0,0, 0,1,1,0, 0,0,0,0},
	 {0,0,0,0, 1,1,1,0, 1,0,0,0, 0,0,0,0},
	 {0,1,1,0, 0,0,1,0, 0,0,1,0, 0,0,0,0}}
};

typedef struct {
	int x, y; // x: fall position (horizontal), y: orthogonal position (vertical)
	int type; // 0-6
	int rotation; // 0-3
} tetris_piece_t;

// Game state
static int tetris_board[ORTHO_SIZE][FALL_SIZE] = {0}; // board[row][col], col increases right (fall dir)
static tetris_piece_t tetris_current_piece;
static tetris_piece_t tetris_next_piece;
static uint32_t tetris_score = 0;
static bool tetris_game_over = false;
static TickType_t tetris_last_fall_time = 0;

// LVGL elements
static lv_obj_t *tetris_canvas = NULL;
static lv_obj_t *tetris_score_label = NULL;
static lv_obj_t *tetris_game_over_label = NULL;

static void *tetris_canvas_pixels = NULL; // Raw pixel buffer in PSRAM

// Save score as high score
static void tetris_high_score_nvs_save(uint32_t score)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t err = nvs_open(HIGH_SCORE_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "tetris_high_score_nvs_save nvs_open failed");
		goto out;
	}

	// Store pin_attempts as a uint32
	err = nvs_set_u32(h, HIGH_SCORE_KEY, score);
	if (err == ESP_OK) {
		// Commit to flash
		err = nvs_commit(h);
		
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Saved Tetris high score: %" PRIu32, score);
		#endif
	}
	else {
		ESP_LOGE(TAG, "Failed to tetris_high_score_nvs_save nvs_set_u32: %" PRIu32, score);
	}
	
	// Close NVS
	out:
	nvs_close(h);
}

// Load Tetris high score
static uint32_t tetris_high_score_nvs_load(void)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t err = nvs_open(HIGH_SCORE_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGW(TAG, "tetris_high_score_nvs_load NS DNE");
		#endif
	}
	
	// Get the uint32
	uint32_t score = 0;
	err = nvs_get_u32(h, HIGH_SCORE_KEY, &score);
	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "Failed tetris_high_score_nvs_load nvs_get_u32");
		#endif
	}
	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Loaded Tetris high score: %" PRIu32, score);
		#endif
	}
	
	// Close NVS
	nvs_close(h);
	
	return score;
}

// Helper: Check if piece collides at given pos/rot
static bool check_collision(int x, int y, int rotation)
{
	// Get shape and given rotation
	const int *shape = tetrominoes[tetris_current_piece.type][rotation];
	
	// Loops over 4x4: If cell occupied, calculates board position
	for (int i = 0; i < 4; i++) { // i: ortho (row)
		for (int j = 0; j < 4; j++) { // j: fall (col)
			// Checks out-of-bounds or overlap with existing board cell
			if (shape[i * 4 + j]) { // i * 4 + j converts 2D coordinates (row i, col j) to a 1D index in the flattened array
				int board_col = x + j;
				int board_row = y + i;
				
				// Returns true if collision
				if (board_col < 0 || board_col >= FALL_SIZE || board_row < 0 || board_row >= ORTHO_SIZE || tetris_board[board_row][board_col]) {
					return true;
				}
			}
		}
	}
	return false;
}

// Helper: Place piece on board
static void place_piece()
{
	// Get shape and given rotation
	const int *shape = tetrominoes[tetris_current_piece.type][tetris_current_piece.rotation];
	
	// Loops over 4x4: If occupied, sets board cell at piece position to 1 (locked)
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			if (shape[i * 4 + j]) { // i * 4 + j converts 2D coordinates (row i, col j) to a 1D index in the flattened array
				tetris_board[tetris_current_piece.y + i][tetris_current_piece.x + j] = 1;
			}
		}
	}
}

// Helper: Clear full lines (now full columns in fall dir), return lines cleared
static int clear_lines()
{
	int lines = 0;
	
	// Scans columns from right (bottom in rotated view) to left
	for (int col = FALL_SIZE - 1; col >= 0; col--) {
		// For each column: Check if all rows occupied (full=true)
		
		bool full = true;
		
		for (int row = 0; row < ORTHO_SIZE; row++) {
			if (!tetris_board[row][col]) {
				full = false;
				break;
			}
		}
		
		// If full: Increment lines, shift all columns left (copy from c-1 to c), clear leftmost (new empty)
		if (full) {
			lines++;
			
			// Shift left (decrease col)
			for (int c = col; c > 0; c--) {
				for (int r = 0; r < ORTHO_SIZE; r++) {
					tetris_board[r][c] = tetris_board[r][c - 1];
				}
			}
			
			// Clear leftmost col
			for (int r = 0; r < ORTHO_SIZE; r++) {
				tetris_board[r][0] = 0;
			}
			
			// Re-scan the now-shifted column (might be full again after multi-clear)
			col++;
		}
	}
	
	return lines;
}

// Helper: Spawn new piece
static void spawn_piece()
{
	// Copies next to current
	tetris_current_piece = tetris_next_piece;
	
	// Sets spawn: x=0 (hidden left), y=3 (middle of 10 rows, centered for 4-cell height)
	tetris_current_piece.x = 0;
	tetris_current_piece.y = ORTHO_SIZE / 2 - 2;
	
	// Random next type (0-6), rotation=0
	tetris_next_piece.type = esp_random() % 7;
	tetris_next_piece.rotation = 0;
	
	// If collides at spawn (stack too high), set game_over
	if (check_collision(tetris_current_piece.x, tetris_current_piece.y, tetris_current_piece.rotation)) {
		tetris_game_over = true;
	}
}

// Helper: Draw board on canvas (only visible cols)
static void draw_board()
{
	lv_canvas_fill_bg(tetris_canvas, COLOR_BOARD_FILL, LV_OPA_COVER); // Clear background

	// Inits a draw layer for batched rendering
	lv_layer_t layer;
	lv_canvas_init_layer(tetris_canvas, &layer);

	// Draw board cells: For each occupied cell, draw filled rect (primary color) + border (secondary color)
	for (int col = HIDDEN_FALL; col < HIDDEN_FALL + VISIBLE_FALL; col++) { // Visible only
		for (int row = 0; row < ORTHO_SIZE; row++) {
			if (tetris_board[row][col]) {
				int px = (col - HIDDEN_FALL) * CELL_SIZE; // x increases right (fall)
				int py = row * CELL_SIZE; // y increases down

				// Fill
				lv_draw_rect_dsc_t fill_dsc;
				lv_draw_rect_dsc_init(&fill_dsc);
				fill_dsc.bg_color = COLOR_PIECE_RESTING;
				fill_dsc.bg_opa = LV_OPA_COVER;
				fill_dsc.radius = 0;
				lv_area_t fill_area;
				lv_area_set(&fill_area, px, py, px + CELL_SIZE - 1, py + CELL_SIZE - 1);
				lv_draw_rect(&layer, &fill_dsc, &fill_area);

				// Border
				lv_draw_rect_dsc_t border_dsc;
				lv_draw_rect_dsc_init(&border_dsc);
				border_dsc.border_color = COLOR_PIECE_OUTLINE;
				border_dsc.border_width = 1;
				border_dsc.border_opa = LV_OPA_COVER;
				border_dsc.bg_opa = LV_OPA_TRANSP; // Transparent fill for border only
				border_dsc.radius = 0;
				lv_draw_rect(&layer, &border_dsc, &fill_area);
			}
		}
	}

	// Draws current piece similarly
	const int *shape = tetrominoes[tetris_current_piece.type][tetris_current_piece.rotation];
	for (int i = 0; i < 4; i++) { // i: ortho offset
		for (int j = 0; j < 4; j++) { // j: fall offset
			if (shape[i * 4 + j]) { // i * 4 + j converts 2D coordinates (row i, col j) to a 1D index in the flattened array
				int board_col = tetris_current_piece.x + j;
				int board_row = tetris_current_piece.y + i;
				
				if (board_col >= HIDDEN_FALL && board_col < HIDDEN_FALL + VISIBLE_FALL) { // Visible
					int px = (board_col - HIDDEN_FALL) * CELL_SIZE;
					int py = board_row * CELL_SIZE;

					// Fill
					lv_draw_rect_dsc_t fill_dsc;
					lv_draw_rect_dsc_init(&fill_dsc);
					fill_dsc.bg_color = COLOR_PIECE_FALLING;
					fill_dsc.bg_opa = LV_OPA_COVER;
					fill_dsc.radius = 0;
					lv_area_t fill_area;
					lv_area_set(&fill_area, px, py, px + CELL_SIZE - 1, py + CELL_SIZE - 1);
					lv_draw_rect(&layer, &fill_dsc, &fill_area);

					// Border
					lv_draw_rect_dsc_t border_dsc;
					lv_draw_rect_dsc_init(&border_dsc);
					border_dsc.border_color = COLOR_PIECE_OUTLINE;
					border_dsc.border_width = 1;
					border_dsc.border_opa = LV_OPA_COVER;
					border_dsc.bg_opa = LV_OPA_TRANSP;
					border_dsc.radius = 0;
					lv_draw_rect(&layer, &border_dsc, &fill_area);
				}
			}
		}
	}

	// Draw whole board outline
	lv_draw_rect_dsc_t board_border_dsc;
	lv_draw_rect_dsc_init(&board_border_dsc);
	board_border_dsc.border_color = COLOR_PIECE_OUTLINE;
	board_border_dsc.border_width = 1;
	board_border_dsc.border_opa = LV_OPA_COVER;
	board_border_dsc.bg_opa = LV_OPA_TRANSP;
	board_border_dsc.radius = 0;
	lv_area_t board_area;
	lv_area_set(&board_area, 0, 0, VISIBLE_FALL * CELL_SIZE - 1, ORTHO_SIZE * CELL_SIZE - 1);
	lv_draw_rect(&layer, &board_border_dsc, &board_area);

	// Finishes layer, invalidates canvas to trigger screen update
	lv_canvas_finish_layer(tetris_canvas, &layer);
	lv_obj_invalidate(tetris_canvas); // Refresh
}

void lcd_tools_tetris_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
{
	static bool init = false;
    static lv_draw_buf_t canvas_buf; // Metadata struct (small, internal SRAM)

    if (!init) {
        // Reset game state
        memset(tetris_board, 0, sizeof(tetris_board));
        tetris_score = 0;
        tetris_game_over = false;
        tetris_next_piece.type = esp_random() % 7;
        spawn_piece();
        tetris_last_fall_time = xTaskGetTickCount();

        // Allocate pixel buffer in PSRAM
        size_t buf_size = VISIBLE_FALL * CELL_SIZE * ORTHO_SIZE * CELL_SIZE * 2; // RGB565: 2 bytes/pixel
        tetris_canvas_pixels = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tetris_canvas_pixels) {
            ESP_LOGE("TETRIS", "Failed to alloc PSRAM for canvas");
            
            // Fallback or exit to menu
            ui_menu->page = TOOLS_PAGE;
            return;
        }

        // Init draw buf metadata (small struct in internal SRAM)
        lv_draw_buf_init(&canvas_buf, VISIBLE_FALL * CELL_SIZE, ORTHO_SIZE * CELL_SIZE, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, tetris_canvas_pixels, buf_size);

        // Create canvas (wide horizontally for fall left->right)
        tetris_canvas = lv_canvas_create(ACTIVE_SCR);
        lv_canvas_set_draw_buf(tetris_canvas, &canvas_buf);
        lv_obj_set_size(tetris_canvas, VISIBLE_FALL * CELL_SIZE, ORTHO_SIZE * CELL_SIZE);
        lv_obj_align(tetris_canvas, LV_ALIGN_CENTER, 0, 0); // Center, adjust if needed for 210x100

        // Score label (position adjusted for layout)
        tetris_score_label = lv_label_create(ACTIVE_SCR);
        lv_label_set_text(tetris_score_label, "Score: 0");
        lv_obj_set_style_text_color(tetris_score_label, user_secondary_color, 0);
        lv_obj_align(tetris_score_label, LV_ALIGN_TOP_MID, 0, -20); // Above canvas, adjust

        // Game over label (hidden initially)
        tetris_game_over_label = lv_label_create(ACTIVE_SCR);
        lv_label_set_text(tetris_game_over_label, "");
        lv_obj_set_style_text_font(tetris_game_over_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(tetris_game_over_label, user_secondary_color, 0);
        lv_obj_align(tetris_game_over_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(tetris_game_over_label, LV_OBJ_FLAG_HIDDEN);

        draw_board();
        init = true;
    }

    // Handle game over
    if (tetris_game_over) {
        // Clear the board visually
        lv_canvas_fill_bg(tetris_canvas, user_primary_color, LV_OPA_COVER);
        lv_obj_invalidate(tetris_canvas);
        
        // Get old high score and compare to current
        uint32_t high_score = tetris_high_score_nvs_load();
        if (tetris_score > high_score) {
			// If new high score, save
			high_score = tetris_score;
			tetris_high_score_nvs_save(high_score);
		}

        char buf[41];
        snprintf(buf, sizeof(buf), "Game Over!\nScore: %" PRIu32 "\nHigh Score: %" PRIu32, tetris_score, high_score);
        lv_label_set_text(tetris_game_over_label, buf);
        lv_obj_remove_flag(tetris_game_over_label, LV_OBJ_FLAG_HIDDEN);

        // Any button to exit
        if (ui_btns->up_btn || ui_btns->down_btn || ui_btns->left_btn || ui_btns->right_btn || ui_btns->select_btn || ui_btns->home_btn) {
            // Cleanup
            lv_obj_del(tetris_canvas);
            lv_obj_del(tetris_score_label);
            lv_obj_del(tetris_game_over_label);
            heap_caps_free(tetris_canvas_pixels); // Free PSRAM
            
            tetris_canvas = tetris_score_label = tetris_game_over_label = NULL;
            tetris_canvas_pixels = NULL;
            init = false;

            // Back to tools menu
            lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = TOOLS_PAGE;
        }
        return;
    }

	/* Input handling */
	bool moved = false;
	// Move piece right
	if (ui_btns->up_btn) {
		if (!check_collision(tetris_current_piece.x, tetris_current_piece.y - 1, tetris_current_piece.rotation)) {
			tetris_current_piece.y--;
			moved = true;
		}
	}
	// Move piece left
	else if (ui_btns->down_btn) {
		if (!check_collision(tetris_current_piece.x, tetris_current_piece.y + 1, tetris_current_piece.rotation)) {
			tetris_current_piece.y++;
			moved = true;
		}
	}
	// Rotate piece
	else if (ui_btns->left_btn) {
		int new_rot = (tetris_current_piece.rotation + 1) % 4;
		if (!check_collision(tetris_current_piece.x, tetris_current_piece.y, new_rot)) {
			tetris_current_piece.rotation = new_rot;
			moved = true;
		}
	}
	// Hard drop (fast forward x)
	else if (ui_btns->select_btn) {
		while (!check_collision(tetris_current_piece.x + 1, tetris_current_piece.y, tetris_current_piece.rotation)) {
			tetris_current_piece.x++;
		}
		
		place_piece();
		int lines = clear_lines();
		tetris_score += 100 * lines * lines; // Bonus for multi-lines
		spawn_piece();
		moved = true;
	}
	// Exit to menu
	else if (ui_btns->home_btn) {
		// Delete objects
		lv_obj_del(tetris_canvas);
		lv_obj_del(tetris_score_label);
		lv_obj_del(tetris_game_over_label);
		heap_caps_free(tetris_canvas_pixels); // Free PSRAM
		
		// Reset statics
		tetris_canvas = tetris_score_label = tetris_game_over_label = tetris_canvas_pixels = NULL;
		init = false;
		
		// Show tools menu
		lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		ui_menu->page = TOOLS_PAGE;
		return;
	}
	// Sleep
	else if (ui_btns->pwr_btn) {
		// Delete objects
		lv_obj_del(tetris_canvas);
		lv_obj_del(tetris_score_label);
		lv_obj_del(tetris_game_over_label);
		heap_caps_free(tetris_canvas_pixels); // Free PSRAM
		
		// Reset statics
		tetris_canvas = tetris_score_label = tetris_game_over_label = tetris_canvas_pixels = NULL;
		init = false;
		
		lcd_funcs_transition_back(false, ui_menu); // False = sleep
		return;
	}

	// Time-based fall (increase x)
	TickType_t now = xTaskGetTickCount();
	uint32_t fall_delay = FALL_DELAY_MS - (tetris_score / 1000) * LEVEL_SPEED_INCREASE; // Speed up with level
	
	if (fall_delay < 100) {
		fall_delay = 100; // Min speed
	}
	
	// Soft drop with right button
	if (ui_btns->right_btn) {
		fall_delay /= SOFT_DROP_MULTIPLIER;
	}

	if (now - tetris_last_fall_time >= pdMS_TO_TICKS(fall_delay)) {
		if (!check_collision(tetris_current_piece.x + 1, tetris_current_piece.y, tetris_current_piece.rotation)) {
			tetris_current_piece.x++;
			moved = true;
		}
		else {
			place_piece();
			int lines = clear_lines();
			tetris_score += 100 * lines * lines;
			spawn_piece();
			moved = true;
		}
		
		tetris_last_fall_time = now;
	}

	// Update UI if changed
	if (moved) {
		draw_board();
		char buf[16];
		snprintf(buf, sizeof(buf), "Score: %" PRIu32, tetris_score);
		lv_label_set_text(tetris_score_label, buf);
	}
}