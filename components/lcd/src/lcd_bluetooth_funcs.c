#include "font/lv_symbol_def.h"
#include "misc/lv_timer.h"
#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"

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

#define TAG "LCD_BLUETOOTH_FUNCS"

#define KEYBOARD_SELECTED_IDX_NS "keyb_sel"
#define KEYBOARD_SELECTED_IDX_KEY "selected"

static char script_labels[MAX_KEYBOARD_SCRIPTS][BT_SCRIPT_LABEL_MAX_LEN + 1];

bluetooth_menu_t bluetooth_menu = {
	.options = {"How It Works", "Media Controller", "Keyboard"},
	.size = NUM_BLUETOOTH_OPTIONS,
	.index = 1,
	.cont = NULL,
};

static void keyboard_menu_rebuild_lvlist(bluetooth_keyboard_menu_t *km)
{
	// Remove all old buttons (if any)
	if (km->main_list != NULL) {
		lv_obj_clean(km->main_list);
	}

	// Create a button for each row we currently have
	for (int i = 0; i < km->size; i++) {
		km->btns[i] = lv_list_add_btn(km->main_list, NULL, km->options[i]);
		lv_obj_set_size(km->btns[i], 200, 30);

		// Apply selected or normal style
		if (i == km->index) {
			lv_obj_add_style(km->btns[i], &km->sel_style, 0);
		} else {
			lv_obj_add_style(km->btns[i], &km->btn_style, 0);
		}

		// Create and format text label
		lv_obj_t *lbl = lv_obj_get_child(km->btns[i], 0);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
		lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
	}

	// Format button container (list internal container is parent of first button)
	if (km->size > 0) {
		km->cont = lv_obj_get_parent(km->btns[0]);
		lv_obj_set_flex_flow (km->cont, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(km->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_gap(km->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Scroll
	lv_obj_scroll_to_view(km->btns[km->index], LV_ANIM_OFF);
}

// Pulls labels from NVS and rebuilds the LVGL list
static void keyboard_menu_refresh_from_nvs(bluetooth_keyboard_menu_t *km)
{
	// Base rows always present
	km->options[0] = "Add/Edit Script";
	km->options[1] = "Test";

	// Read how many user scripts are stored
	uint32_t count = bluetooth_script_count_get();

	// Cap
	if (count > MAX_KEYBOARD_SCRIPTS) {
		count = MAX_KEYBOARD_SCRIPTS;
	}

	// Pull labels for each user script i -> row (i + NUM_KEYBOARD_BASE)
	for (uint32_t i = 0; i < count; i++) {
		// Fill default label first
		script_labels[i][0] = '\0';

		// Read label from NVS (namespace/keys match the portal)
		size_t len = sizeof(script_labels[i]);
		esp_err_t err = bluetooth_script_label_get(i, script_labels[i], len);
		if (err != ESP_OK) {
			// On error, show a placeholder rather than leaving a blank
			snprintf(script_labels[i], sizeof(script_labels[i]), "Script %u", (unsigned)i);
		}

		km->options[NUM_KEYBOARD_BASE + i] = script_labels[i];
	}

	// New total = base + user
	km->size = NUM_KEYBOARD_BASE + (int)count;

	// Update index
	km->index = lcd_bluetooth_script_selected_get();
	if (km->index >= km->size) {
		km->index = (km->size > 1) ? 1 : 0;
	}
	else if (km->index < 0) {
		km->index = km->size - 1;
	}

	// Rebuild LVGL widgets to match new size
	keyboard_menu_rebuild_lvlist(km);
}

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
	
	// Create button for each option based on NVS
	keyboard_menu_refresh_from_nvs(menu);
	
	// Hide for now
	lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_bluetooth_setup_page(bluetooth_menu_t *menu)
{
	// Setup bluetooth keyboard menu once
	bluetooth_keyboard_menu_t *km = &menu->bluetooth_keyboard_menu;
	if (km->size <= 0) {
		km->options[0] = "Add/Edit Script";
		km->options[1] = "Test";
		km->size = NUM_KEYBOARD_BASE; // Final index + 1
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

void lcd_bluetooth_how_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	#define HOW_Y_OFFSET 40
	
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
		const char *instr_text = "Bluetooth is now advertising as 'PolyCast5'.\n\nTo connect to it, just go to settings on any Bluetooth device "
								 "such as a phone or PC, click on 'PolyCast5', and enter '123456' as the pin.\n\nAfter connecting once, PolyCast5 "
								 "will automatically reconnect to the last known device after selecting an option from the Bluetooth menu.\n\nYou "
								 "will also see the RGB LED turn blue to indicate PolyCast5 is currently connected to a device. If you don't wish to "
								 "see this, it can be disabled in settings by setting 'Blink every' to 0 for 'Adjust RGB LED'.";
		
		lv_label_set_text(instr_lbl, instr_text);

		lv_timer_handler();
		// Active bluetooth
		uint16_t cmd = BLUETOOTH_CMD_INIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by(cont, 0, HOW_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by(cont, 0, -HOW_Y_OFFSET, LV_ANIM_ON);
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
			
		// Show bluetooth menu
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch back
		ui_menu->page = BLUETOOTH_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
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
		uint16_t cmd = BLUETOOTH_CMD_INIT;
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
		uint16_t cmd = BLUETOOTH_CMD_VOLUME_UP;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Next track
	else if (ui_btns->right_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_right, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_right, user_primary_color, 0);

		// Send command
		uint16_t cmd = BLUETOOTH_CMD_NEXT_TRK;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Volume down
	else if (ui_btns->down_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_down, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_down, user_primary_color, 0);

		// Send command
		uint16_t cmd = BLUETOOTH_CMD_VOLUME_DOWN;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Previous track
	else if (ui_btns->left_btn == 1) {		
		// Invert circle
		lv_obj_add_style(circ_left, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_left, user_primary_color, 0);

		// Send command
		uint16_t cmd = BLUETOOTH_CMD_PREV_TRK;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Pause/play
	else if (ui_btns->select_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_center, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_center, user_primary_color, 0);

		// Send command
		uint16_t cmd = BLUETOOTH_CMD_PLAY_PAUSE;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Go back
	else if (ui_btns->home_btn == 1) {
		// Deinit bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
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
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
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
		// Update based on NVS
		keyboard_menu_refresh_from_nvs(&bluetooth_menu->bluetooth_keyboard_menu);

		// Show bluetooth keyboard menu
		lv_obj_remove_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);

		lv_timer_handler();

		// Active bluetooth
		uint16_t cmd = BLUETOOTH_CMD_INIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		
		do_once = true;
	}

	// Up button pressed
	if (ui_btns->up_btn == 1) {
		// Update selection
		bluetooth_menu->bluetooth_keyboard_menu.index--;
		lcd_bluetooth_update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);

		// Save to NVS
		lcd_bluetooth_script_selected_set(bluetooth_menu->bluetooth_keyboard_menu.index);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		bluetooth_menu->bluetooth_keyboard_menu.index++;
		lcd_bluetooth_update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);

		// Save to NVS
		lcd_bluetooth_script_selected_set(bluetooth_menu->bluetooth_keyboard_menu.index);
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
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
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Hide bluetooth keyboard menu
		lv_obj_add_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// Add script selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->bluetooth_keyboard_menu.index == 0) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Hide bluetooth keyboard menu
		lv_obj_add_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		// Switch pages
		ui_menu->page = BLUETOOTH_SCRIPT_ADD_PAGE;
	}
	// Script selected
	else if (ui_btns->select_btn == 1 && bluetooth_menu->bluetooth_keyboard_menu.index > 0) {
		// Send script
		uint16_t cmd = bluetooth_menu->bluetooth_keyboard_menu.index + BLUETOOTH_SCRIPT_OFFSET; // E.g. Send script index 1 => send 1001
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
}

void lcd_bluetooth_add_script_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	#define SCRIPT_ADD_Y_OFFSET 40
	
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
		lv_label_set_text(title_lbl, "Adding a Script:");
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


		// Start SoftAP and web portal
		char msg[64];
	    if (bluetooth_web_portal_start() == ESP_OK) {
			// Get portal IP
	        snprintf(msg, sizeof(msg), "http://%s", bluetooth_web_portal_get_ip());
	    }

		// Set custom text based on hotkey index
		const char *instr_text = "How to quickly add a new Bluetooth autotype text script:\n\nFirst, grab your phone or other device and navigate to Wi-Fi settings."
				"\n\nThere, you should see a joinable Wi-Fi network named '" PORTAL_SSID "'. Click on it and enter the password '" PORTAL_PASS "'."
				"\n\nOnce connected, open up your internet browser of choice and search:\n\n%s\n\nFrom there, follow the on-screen instructions. "
				"DO NOT exit this page until you're done entering what you want into the web portal.";
		
		lv_label_set_text_fmt(instr_lbl, instr_text, msg);
	
		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by(cont, 0, SCRIPT_ADD_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by(cont, 0, -SCRIPT_ADD_Y_OFFSET, LV_ANIM_ON);
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Turn off web portal
		bluetooth_web_portal_stop();
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
			
		// Show bluetooth menu
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Switch back
		ui_menu->page = BLUETOOTH_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Turn off web portal
		bluetooth_web_portal_stop();

		// Delete objects
		lv_obj_del(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

// Save the last-selected user script index (for convenience on LCD)
esp_err_t lcd_bluetooth_script_selected_set(uint8_t idx)
{
 	nvs_handle_t h;
 	
 	// Open NVS
 	esp_err_t err = nvs_open(KEYBOARD_SELECTED_IDX_NS, NVS_READWRITE, &h);
 	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGE(TAG, "lcd_bluetooth_script_selected_set nvs_open failed: %s", esp_err_to_name(err));
		#endif
		
 	 	return err;
 	}
 	
 	// Set selected key
 	err = nvs_set_u8(h, KEYBOARD_SELECTED_IDX_KEY, idx);
 	if (err == ESP_OK) {
		// Commit changes on success
 	 	err = nvs_commit(h);
 	}
 	else {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGE(TAG, "lcd_bluetooth_script_selected_set nvs_set_u8 failed: %s", esp_err_to_name(err));
		#endif
	}
	
	// Close NVS
 	nvs_close(h);
 	return err;
}

// Read the previously selected index
uint8_t lcd_bluetooth_script_selected_get(void)
{
 	nvs_handle_t h;
 	uint8_t sel = 0;
 	
 	// Open NVS
 	esp_err_t err = nvs_open(KEYBOARD_SELECTED_IDX_NS, NVS_READONLY, &h);
 	if (err == ESP_OK) {
		// Get count
 	 	if (nvs_get_u8(h, KEYBOARD_SELECTED_IDX_KEY, &sel) != ESP_OK) {
			// 0 if DNE
 	 	 	sel = 0;
 	 	 	
 	 	 	#ifdef POLYCAST5_DEBUG
				ESP_LOGE(TAG, "lcd_bluetooth_script_selected_set nvs_get_u8 failed: %s", esp_err_to_name(err));
			#endif
 	 	}
 	 	
 	 	// Close NVS
 	 	nvs_close(h);
 	}
 	else {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "lcd_bluetooth_script_selected_set nvs_open failed: %s", esp_err_to_name(err));
		#endif
	}
	
	return sel;
}