#include "font/lv_symbol_def.h"
#include "misc/lv_timer.h"
#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"

#include "esp_log.h"
#include "esp_err.h"

#include "core/lv_obj_pos.h"
#include "core/lv_obj.h"
#include "core/lv_obj_scroll.h"
#include "misc/lv_style.h"
#include "misc/lv_area.h"
#include "widgets/label/lv_label.h"

#include "lcd_utils.h"
#include "lcd_bluetooth_funcs.h"
#include "bluetooth_funcs.h"
#include "bluetooth_task.h"

bluetooth_menu_t bluetooth_menu = {
	.options = {"How it works", "Media Controller", "Keyboard"},
	.size = 3,
	.index = 1,
	.cont = NULL,
};

static void lcd_bluetooth_setup_keyboard_page(bluetooth_keyboard_menu_t *menu)
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
		menu->index = 1;
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

void lcd_bluetooth_setup_page(bluetooth_menu_t *menu)
{
	// Setup bluetooth keyboard menu once
	bluetooth_keyboard_menu_t *km = &menu->bluetooth_keyboard_menu;
	if (km->size <= 0) {
		km->options[0] = "Add Script";
		km->options[1] = "Test";
		km->size = 2;
		km->index = 1; // Default index
	}
	lcd_bluetooth_setup_keyboard_page(km);

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
		menu->index = 1;
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

void lcd_bluetooth_update_menu(bluetooth_menu_t *menu)
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

void lcd_bluetooth_update_keyboard_menu(bluetooth_keyboard_menu_t *menu)
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

void lcd_bluetooth_pair_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	// Statics
	static bool init = false;
	
	static lv_obj_t *lbl_ins;
	static lv_obj_t *lbl_pin;
	
	// Only execute once
	if (!init) {
		// Create labels
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "Bluetooth is now\n   advertising as\n'PolyCast5' for PC", user_secondary_color,
				&lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

		lbl_pin = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_pin, "Pin: 123456", user_secondary_color,
				&lv_font_montserrat_24, LV_ALIGN_BOTTOM_MID, 0, -15);
				
		lv_timer_handler();
		
		// Initialize bluetooth
		uint8_t cmd = BLUETOOTH_CMD_INIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		
		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		
	}
	else if (ui_btns->down_btn == 1) {
		
	}
	else if (ui_btns->select_btn == 1) {
		
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_pin);
		
		// Reset statics
		lbl_ins = lbl_pin = NULL;
		init = false;
		
		// Show bluetooth list
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = BLUETOOTH_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_pin);
		
		// Reset statics
		lbl_ins = lbl_pin = NULL;
		init = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_bluetooth_media_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	// Macros
	#define MEDIA_OUTER_SZ 130 // Diameter of big ring
	#define MEDIA_OUTER_BW 2 // Ring border width
	#define MEDIA_BTN_SZ 35 // Diameter of each small circle
	#define MEDIA_BTN_RAD (MEDIA_BTN_SZ / 2)
	#define MEDIA_OUTER_RAD (MEDIA_OUTER_SZ / 2)
	#define MEDIA_MARGIN 8 // Extra gap from inner edge of outer ring
	#define MEDIA_R (MEDIA_OUTER_RAD - MEDIA_BTN_RAD - MEDIA_MARGIN) // Offset for outer buttons
	#define MEDIA_X_OFFSET 35

	// Statics
	static bool init = false;
	static lv_obj_t *lbl_home = NULL;

	// Outer ring
	static lv_obj_t *ring = NULL;

	// Inner circles
	static lv_obj_t *circ_up = NULL, *circ_right = NULL, *circ_down = NULL, *circ_left = NULL, *circ_center = NULL;

	// Labels
	static lv_obj_t *lbl_up = NULL, *lbl_right = NULL, *lbl_down = NULL, *lbl_left = NULL, *lbl_center = NULL;

	// Styles
	static lv_style_t style_ring; // Big outer circle border
	static lv_style_t style_circle; // Small circles border
	static lv_style_t style_circle_pressed; // Small circle pressed

	// Do once
	if (!init) {
		// Create home label
		lbl_home = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_home, "HOME", user_secondary_color,
				&lv_font_montserrat_16, LV_ALIGN_LEFT_MID, 17, 0);

		// Create styles
		lv_style_init(&style_ring);
		lv_style_set_radius(&style_ring, LV_RADIUS_CIRCLE);
		lv_style_set_bg_opa(&style_ring, LV_OPA_TRANSP);
		lv_style_set_border_width(&style_ring, MEDIA_OUTER_BW);
		lv_style_set_border_color(&style_ring, user_secondary_color);

		lv_style_init(&style_circle);
		lv_style_set_radius(&style_circle, LV_RADIUS_CIRCLE);
		lv_style_set_bg_opa(&style_circle, LV_OPA_TRANSP);
		lv_style_set_border_width(&style_circle, 2);
		lv_style_set_border_color(&style_circle, user_secondary_color);

		lv_style_init(&style_circle_pressed);
		lv_style_set_radius(&style_circle_pressed, LV_RADIUS_CIRCLE);
		lv_style_set_bg_opa(&style_circle_pressed, LV_OPA_COVER);
		lv_style_set_bg_color(&style_circle_pressed, user_secondary_color);
		lv_style_set_border_width(&style_circle_pressed, 2);
		lv_style_set_border_color(&style_circle_pressed, user_secondary_color);

		// Outer ring (acts as container for all 5 small circles)
		ring = lv_obj_create(ACTIVE_SCR);
		lv_obj_add_style(ring, &style_ring, 0);
		lv_obj_set_size(ring, MEDIA_OUTER_SZ, MEDIA_OUTER_SZ);
		lv_obj_align(ring, LV_ALIGN_CENTER, MEDIA_X_OFFSET, 0);
		// No scrollbar
		lv_obj_set_scrollbar_mode(ring, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

		// Small circles + labels
		// UP
		circ_up = lv_obj_create(ring);
		lv_obj_add_style(circ_up, &style_circle, 0);
		lv_obj_set_size(circ_up, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_up, LV_ALIGN_CENTER, 0, -MEDIA_R);
		lv_obj_set_style_radius(circ_up, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_up, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_up, LV_OBJ_FLAG_SCROLLABLE);
		lbl_up = lv_label_create(circ_up);
		lv_label_set_text(lbl_up, LV_SYMBOL_VOLUME_MAX); // Vol+
		lv_obj_set_style_text_font(lbl_up, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_up, user_secondary_color, 0);
		lv_obj_center(lbl_up);

		// RIGHT
		circ_right = lv_obj_create(ring);
		lv_obj_add_style(circ_right, &style_circle, 0);
		lv_obj_set_size(circ_right, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_right, LV_ALIGN_CENTER, MEDIA_R, 0);
		lv_obj_set_style_radius(circ_right, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_right, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_right, LV_OBJ_FLAG_SCROLLABLE);
		lbl_right = lv_label_create(circ_right);
		lv_label_set_text(lbl_right, LV_SYMBOL_NEXT); // Next
		lv_obj_set_style_text_font(lbl_right, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_right, user_secondary_color, 0);
		lv_obj_align(lbl_right, LV_ALIGN_CENTER, 1, 0);

		// DOWN
		circ_down = lv_obj_create(ring);
		lv_obj_add_style(circ_down, &style_circle, 0);
		lv_obj_set_size(circ_down, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_down, LV_ALIGN_CENTER, 0, MEDIA_R);
		lv_obj_set_style_radius(circ_down, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_down, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_down, LV_OBJ_FLAG_SCROLLABLE);
		lbl_down = lv_label_create(circ_down);
		lv_label_set_text(lbl_down, LV_SYMBOL_VOLUME_MID); // Vol-
		lv_obj_set_style_text_font(lbl_down, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_down, user_secondary_color, 0);
		lv_obj_center(lbl_down);

		// LEFT
		circ_left = lv_obj_create(ring);
		lv_obj_add_style(circ_left, &style_circle, 0);
		lv_obj_set_size(circ_left, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_left, LV_ALIGN_CENTER, -MEDIA_R, 0);
		lv_obj_set_style_radius(circ_left, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_left, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_left, LV_OBJ_FLAG_SCROLLABLE);
		lbl_left = lv_label_create(circ_left);
		lv_label_set_text(lbl_left, LV_SYMBOL_PREV); // Previous
		lv_obj_set_style_text_font(lbl_left, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_left, user_secondary_color, 0);
		lv_obj_align(lbl_left, LV_ALIGN_CENTER, 0, 0);

		// CENTER
		circ_center = lv_obj_create(ring);
		lv_obj_add_style(circ_center, &style_circle, 0);
		lv_obj_set_size(circ_center, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_center, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_radius(circ_center, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_center, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_center, LV_OBJ_FLAG_SCROLLABLE);
		lbl_center = lv_label_create(circ_center);
		lv_label_set_text(lbl_center, LV_SYMBOL_PLAY);
		lv_obj_set_style_text_font(lbl_center, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_center, user_secondary_color, 0);
		lv_obj_align(lbl_center, LV_ALIGN_CENTER, 2, 0);

		lv_timer_handler();

		// Active bluetooth
		uint8_t cmd = BLUETOOTH_CMD_INIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		init = true;
	}

	// Reset visuals if no button event this tick
	if (ui_btns->up_btn != 1 && ui_btns->right_btn != 1 && ui_btns->down_btn != 1 && ui_btns->left_btn != 1 && ui_btns->select_btn != 1)
	{
		if (circ_up) {
			lv_obj_remove_style(circ_up, &style_circle_pressed, 0);
		}
		if (circ_right)	{
			lv_obj_remove_style(circ_right, &style_circle_pressed, 0);
		}
		if (circ_down) {
			lv_obj_remove_style(circ_down, &style_circle_pressed, 0);
		}
		if (circ_left) {
			lv_obj_remove_style(circ_left, &style_circle_pressed, 0);
		}
		if (circ_center) {
			lv_obj_remove_style(circ_center, &style_circle_pressed, 0);
		}

		if (lbl_up) {
			lv_obj_set_style_text_color(lbl_up, user_secondary_color, 0);
		}
		if (lbl_right) {
			lv_obj_set_style_text_color(lbl_right, user_secondary_color, 0);
		}
		if (lbl_down) {
			lv_obj_set_style_text_color(lbl_down, user_secondary_color, 0);
		}
		if (lbl_left) {
			lv_obj_set_style_text_color(lbl_left, user_secondary_color, 0);
		}
		if (lbl_center)	{
			lv_obj_set_style_text_color(lbl_center, user_secondary_color, 0);
		}
	}

	/* Input handling */
	// Volume up
	if (ui_btns->up_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_up, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_up, user_primary_color, 0);

		// Send command
		uint8_t cmd = BLUETOOTH_CMD_VOLUME_UP;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Next track
	else if (ui_btns->right_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_right, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_right, user_primary_color, 0);

		// Send command
		uint8_t cmd = BLUETOOTH_CMD_NEXT_TRK;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Volume down
	else if (ui_btns->down_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_down, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_down, user_primary_color, 0);

		// Send command
		uint8_t cmd = BLUETOOTH_CMD_VOLUME_DOWN;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Previous track
	else if (ui_btns->left_btn == 1) {		
		// Invert circle
		lv_obj_add_style(circ_left, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_left, user_primary_color, 0);

		// Send command
		uint8_t cmd = BLUETOOTH_CMD_PREV_TRK;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Pause/play
	else if (ui_btns->select_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_center, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_center, user_primary_color, 0);

		// Send command
		uint8_t cmd = BLUETOOTH_CMD_PLAY_PAUSE;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Go back
	else if (ui_btns->home_btn == 1) {
		// Deinit bluetooth
		uint8_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Delete objects
		lv_obj_delete(ring); // Deletes children
		lv_obj_delete(lbl_home);
		
		// Reset styles
		lv_style_reset(&style_ring);
		lv_style_reset(&style_circle);
		lv_style_reset(&style_circle_pressed);
		
		// Reset statics
		circ_up = circ_right = circ_down = circ_left = circ_center = NULL;
		lbl_up = lbl_right = lbl_down = lbl_left = lbl_center = NULL;
		lbl_home = NULL;
		init = false;

		// Show arrows
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);

		// Show bluetooth list
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = BLUETOOTH_PAGE;
	}
	// Power off
	else if (ui_btns->pwr_btn == 1) {
		// Deinit bluetooth
		uint8_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Delete objects
		lv_obj_delete(ring); // Deletes children
		lv_obj_delete(lbl_home);
		
		// Reset styles
		lv_style_reset(&style_ring);
		lv_style_reset(&style_circle);
		lv_style_reset(&style_circle_pressed);
		
		// Reset statics
		circ_up = circ_right = circ_down = circ_left = circ_center = NULL;
		lbl_up = lbl_right = lbl_down = lbl_left = lbl_center = NULL;
		lbl_home = NULL;
		init = false;

		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
}

void lcd_bluetooth_keyboard_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	// Statics
	static bool do_once = false;
	
	// Only execute once
	if (!do_once) {
		// Show bluetooth keyboard menu
		lv_obj_remove_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);

		// Active bluetooth
		uint8_t cmd = BLUETOOTH_CMD_INIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		
		do_once = true;
	}

	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		bluetooth_menu->bluetooth_keyboard_menu.index--;
		lcd_bluetooth_update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		bluetooth_menu->bluetooth_keyboard_menu.index++;
		lcd_bluetooth_update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Deactivate bluetooth
		uint8_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Hide bluetooth keyboard menu
		lv_obj_add_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show bluetooth menu
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Deactivate bluetooth
		uint8_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Hide bluetooth keyboard menu
		lv_obj_add_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// Script one selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->bluetooth_keyboard_menu.index == 1) {
		// Send script one
		uint8_t cmd = BLUETOOTH_CMD_SCRIPT_ONE;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
}

