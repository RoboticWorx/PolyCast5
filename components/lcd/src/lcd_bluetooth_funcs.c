#include "esp_hid_gap.h"
#include "font/lv_symbol_def.h"
#include "gpio_task.h"
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
#define BT_NUM_CHAR_ROWS 4
#define MAX_BT_NAME_LEN 12

extern char bt_wifi_portal_pass[];

static uint8_t current_category = 0;

static char bt_name_buf[MAX_BT_NAME_LEN + 1] = {0};

EXT_RAM_BSS_ATTR static char script_labels[MAX_KEYBOARD_SCRIPTS][BT_SCRIPT_LABEL_MAX_LEN + 1];

bluetooth_keyboard_menu_t bluetooth_keyboard_submenu = {
	.options = {NULL}, // Dynamically populated
	.btns = {NULL},
	.size = 0,
	.index = 0,
	.main_list = NULL,
	.cont = NULL,
	// Styles will be init in setup
	// cat_labels and script_indices zero-init
};

bluetooth_menu_t bluetooth_menu = {
	.options = {"How It Works", "Auto Keyboard", "Media Controller", "Page Scroller",
			"PowerPoint Clicker", "Camera Clicker", "Socials Scroller", "Known Devices"},
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

	// If no rows to show, render a disabled placeholder and return
	if (km->size <= 0) {
		// Create single disabled-looking row
		lv_obj_t *btn = lv_list_add_btn(km->main_list, NULL, "No scripts added");
		lv_obj_set_size(btn, 200, 30);

		// Apply base style so it still fits the UI
		lv_obj_add_style(btn, &km->btn_style, 0);

		// Center the label text and make it scroll if needed
		lv_obj_t *lbl = lv_obj_get_child(btn, 0);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
		lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

		// Set flex container formatting
		km->cont = lv_obj_get_parent(btn);
		lv_obj_set_flex_flow (km->cont, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(km->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_gap(km->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

		// Nothing to scroll; leave now
		return;
	}

	// Create a button for each row we currently have
	for (int i = 0; i < km->size; ++i) {
		km->btns[i] = lv_list_add_btn(km->main_list, NULL, km->options[i]);
		lv_obj_set_size(km->btns[i], 200, 30);

		// Style selected
		if (i == km->index) {
			lv_obj_add_style(km->btns[i], &km->sel_style, 0);
		}
		else {
			lv_obj_add_style(km->btns[i], &km->btn_style, 0);
		}

		// Create and format text label
		lv_obj_t *lbl = lv_obj_get_child(km->btns[i], 0);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
		lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
	}

	// Format button container (list internal container is parent of first button)
	km->cont = lv_obj_get_parent(km->btns[0]);
	lv_obj_set_flex_flow (km->cont, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(km->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(km->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

	// Clamp index for safety, then scroll selected into view
	if (km->index < 0) {
		km->index = 0;
	}
	else if (km->index >= km->size) {
		km->index = km->size - 1;
	}

	if (km->index + 1 >= km->size) {
		lv_obj_scroll_to_view(km->btns[km->index], LV_ANIM_OFF);
	}
	else {
		lv_obj_scroll_to_view(km->btns[km->index + 1], LV_ANIM_OFF); // Put selected near middle
	}
}

// Pulls labels from NVS and rebuilds the LVGL list
static void keyboard_menu_refresh_from_nvs(bluetooth_keyboard_menu_t *km)
{
	// Base rows always present
	km->options[0] = "Add/Edit Script";
	km->options[1] = "Test";

	// Read how many categories are stored
	uint8_t cat_count = bluetooth_category_count_get_nvs();

	// Cap
	if (cat_count > MAX_CATEGORIES) {
		cat_count = MAX_CATEGORIES;
	}

	// Pull names for each category i -> row (i + NUM_KEYBOARD_BASE)
	for (uint8_t i = 0; i < cat_count; ++i) {
		// Fill default name first
		km->cat_labels[i][0] = '\0';

		// Read name from NVS
		esp_err_t err = bluetooth_category_name_get_nvs(i, km->cat_labels[i], sizeof(km->cat_labels[i]));
		if (err != ESP_OK) {
			// On error, show a placeholder
			snprintf(km->cat_labels[i], sizeof(km->cat_labels[i]), "Category %u", (unsigned)i);
		}

		km->options[NUM_KEYBOARD_BASE + i] = km->cat_labels[i];
	}

	// New total = base + categories (all shown, even empty—user can delete if unwanted)
	km->size = NUM_KEYBOARD_BASE + (int)cat_count;

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

// Pulls labels from NVS and rebuilds the submenu LVGL list
static void keyboard_submenu_refresh_from_nvs(bluetooth_keyboard_menu_t *km, uint8_t category)
{
	// Read how many user scripts are stored
	uint32_t count = bluetooth_script_count_get_nvs();

	// Cap
	if (count > MAX_KEYBOARD_SCRIPTS) {
		count = MAX_KEYBOARD_SCRIPTS;
	}

	// Pull labels for each user script i that matches category
	int s = 0;
	memset(km->script_indices, 0, sizeof(km->script_indices));

	for (uint32_t i = 0; i < count; ++i) {
		uint8_t cat = 0;
		esp_err_t err = bluetooth_script_cat_get_nvs((uint8_t)i, &cat);

		if (err == ESP_OK && cat == category) {
			// Fill default label first
			script_labels[s][0] = '\0';

			// Read label from NVS (namespace/keys match the portal)
			size_t len = sizeof(script_labels[s]);
			err = bluetooth_script_label_get_nvs((uint8_t)i, script_labels[s], len);
			if (err != ESP_OK) {
				// On error, show a placeholder rather than leaving a blank
				snprintf(script_labels[s], sizeof(script_labels[s]), "Script %u", (unsigned)i);
			}

			km->options[s] = script_labels[s];
			km->script_indices[s] = (uint8_t)i;
			s++;
		}
	}

	// Handle empty submenu with placeholder
	if (s == 0) {
		snprintf(script_labels[0], sizeof(script_labels[0]), "No scripts");
		km->options[0] = script_labels[0];
		km->script_indices[0] = 255; // Invalid index to skip execution on select
		s = 1;
	}

	km->size = s;

	// Update index (clamp)
	if (km->index >= km->size) {
		km->index = (km->size > 0) ? km->size - 1 : 0;
	}
	else if (km->index < 0) {
		km->index = 0;
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
	lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lcd_apply_scrollbar_style(menu->main_list);
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

static void setup_keyboard_submenu_page(bluetooth_keyboard_menu_t *submenu)
{
	// Create list
	submenu->main_list = lv_list_create(ACTIVE_SCR);
	lv_obj_set_size(submenu->main_list, 210, 106);
	
	// Format
	lv_obj_set_style_bg_color(submenu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(submenu->main_list, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_border_width(submenu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lcd_apply_scrollbar_style(submenu->main_list);
	lv_obj_set_scroll_dir(submenu->main_list, LV_DIR_VER);

	// Create button style
	lv_style_init(&submenu->btn_style);
	
	lv_style_set_radius(&submenu->btn_style, 8);
	lv_style_set_bg_color(&submenu->btn_style, user_primary_color);
	
	lv_style_set_border_width(&submenu->btn_style, 2);
	lv_style_set_border_color(&submenu->btn_style, user_secondary_color);
	lv_style_set_border_side(&submenu->btn_style, LV_BORDER_SIDE_FULL);
	
	lv_style_set_pad_top(&submenu->btn_style, 3);
	lv_style_set_pad_bottom(&submenu->btn_style, 3);
	
	lv_style_set_text_font(&submenu->btn_style, &lv_font_montserrat_16);
	lv_style_set_text_color(&submenu->btn_style, user_secondary_color);
	lv_style_set_text_align(&submenu->btn_style, LV_TEXT_ALIGN_CENTER);
	
	// Create selected button style
	lv_style_init(&submenu->sel_style);
	
	lv_style_set_radius(&submenu->sel_style, 8);
	lv_style_set_bg_color(&submenu->sel_style, user_secondary_color);
	
	lv_style_set_border_width(&submenu->sel_style, 2);
	lv_style_set_border_color(&submenu->sel_style, user_secondary_color);
	lv_style_set_border_side(&submenu->sel_style, LV_BORDER_SIDE_FULL);
	
	lv_style_set_pad_top(&submenu->sel_style, 3);
	lv_style_set_pad_bottom(&submenu->sel_style, 3);
	
	lv_style_set_text_font(&submenu->sel_style, &lv_font_montserrat_16);
	lv_style_set_text_color(&submenu->sel_style, user_primary_color);
	lv_style_set_text_align(&submenu->sel_style, LV_TEXT_ALIGN_CENTER);

	// Wrap index
	if (submenu->index >= submenu->size) {
		submenu->index = 1;
	}
	else if (submenu->index < 0) {
		submenu->index = submenu->size - 1;
	}
}

static void setup_known_devices_page(bluetooth_peer_menu_t *menu)
{
	// Create list
	menu->main_list = lv_list_create(ACTIVE_SCR);
	lv_obj_set_size(menu->main_list, 210, 106);
	
	// Format
	lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lcd_apply_scrollbar_style(menu->main_list);
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
	
	// Set index
	menu->index = 0;
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
	lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lcd_apply_scrollbar_style(menu->main_list);
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
	for (int i = 0; i < menu->size; ++i) {
		lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
		lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
	}

	// Highlight only the current index
	lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
	lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);
	
	// Scroll to selected
	if (menu->size > 0) {
		lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
	}
}

static void update_keyboard_menu(bluetooth_keyboard_menu_t *menu)
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
	
	// Scroll to selected
	if (menu->size > 0) {
		lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
	}
}

static void update_keyboard_submenu(bluetooth_keyboard_menu_t *submenu)
{
	// Reveal
	lv_obj_remove_flag(submenu->main_list, LV_OBJ_FLAG_HIDDEN);

	// Wrap index
	if (submenu->index >= submenu->size) {
		submenu->index = 0;
	}
	else if (submenu->index < 0) {
		submenu->index = submenu->size - 1;
	}

	// Reset every button to unselected
	for (int i = 0; i < submenu->size; ++i) {
		lv_obj_remove_style(submenu->btns[i], &submenu->sel_style, 0);
		lv_obj_add_style(submenu->btns[i], &submenu->btn_style, 0);
	}

	// Highlight only the current index
	lv_obj_remove_style(submenu->btns[submenu->index], &submenu->btn_style, 0);
	lv_obj_add_style(submenu->btns[submenu->index], &submenu->sel_style, 0);
	
	// Scroll to selected
	if (submenu->size > 0) {
		lv_obj_scroll_to_view(submenu->btns[submenu->index], LV_ANIM_ON); // LV_ANIM_OFF
	}
}

static void update_known_devices_menu(bluetooth_peer_menu_t *menu)
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
	
	// Scroll to selected
	if (menu->size > 0) {
		lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
	}
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

		// Set custom text
		const char *instr_text =
				"Bluetooth is now advertising as 'PolyCast5'.\n\n"
				"Click the right arrow if you want to forget all devices.\n\n"
				"To connect a new device, just go to settings on any Bluetooth device such as a phone or PC, "
				"click on 'PolyCast5', and enter '%d' as the pin.\n\nAfter connecting once, PolyCast5 "
				"will automatically reconnect to the last known device after selecting an option from the Bluetooth menu.\n\nYou "
				"will also see the RGB LED turn blue to indicate PolyCast5 is currently connected to a device. If you don't wish to "
				"see this, it can be disabled in settings by setting 'Blink every' to 0 for 'Adjust RGB LED'.";
		
		// Load pairing key from NVS
		uint32_t pairing_key;
		bluetooth_pairing_key_load_nvs(&pairing_key);
		lv_label_set_text_fmt(instr_lbl, instr_text, pairing_key);

		lv_timer_handler();
		// Active bluetooth
		uint16_t cmd = BLUETOOTH_CMD_INIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, HOW_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -HOW_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->right_btn == 1) {
		// Forget all bluetooth bonding keys
		uint16_t cmd = BLUETOOTH_CMD_UNPAIR_ALL;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

		// Delete objects
		lv_obj_delete(cont); // Deletes children
		
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
		lv_obj_delete(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_bluetooth_media_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu, uint8_t type)
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

	// Build initial UI elements
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

		/* Small circles and labels */

		// UP
		circ_up = lv_obj_create(ring);
		lv_obj_add_style(circ_up, &style_circle, 0);
		lv_obj_set_size(circ_up, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_up, LV_ALIGN_CENTER, 0, -MEDIA_R);
		lv_obj_set_style_radius(circ_up, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_up, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_up, LV_OBJ_FLAG_SCROLLABLE);
		lbl_up = lv_label_create(circ_up);

		// Text
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			lv_label_set_text(lbl_up, LV_SYMBOL_VOLUME_MAX); // Vol+
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_label_set_text(lbl_up, LV_SYMBOL_OK); // Start
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE || type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			lv_label_set_text(lbl_up, LV_SYMBOL_UP); // Scroll up
		}
		else if (type == BLUETOOTH_MEDIA_CAMERA_PAGE) {
			lv_label_set_text(lbl_up, "");
		}
		lv_obj_set_style_text_font(lbl_up, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_up, user_secondary_color, 0);

		// Alignment
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE || type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_obj_align(lbl_up, LV_ALIGN_CENTER, 0, 0);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE || type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			lv_obj_align(lbl_up, LV_ALIGN_CENTER, 0, -1);
		}

		// RIGHT
		circ_right = lv_obj_create(ring);
		lv_obj_add_style(circ_right, &style_circle, 0);
		lv_obj_set_size(circ_right, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_right, LV_ALIGN_CENTER, MEDIA_R, 0);
		lv_obj_set_style_radius(circ_right, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_right, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_right, LV_OBJ_FLAG_SCROLLABLE);
		lbl_right = lv_label_create(circ_right);

		// Text
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE || type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			lv_label_set_text(lbl_right, LV_SYMBOL_NEXT); // Next || Page up
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_label_set_text(lbl_right, LV_SYMBOL_RIGHT); // Next slide
		}
		else if (type == BLUETOOTH_MEDIA_CAMERA_PAGE) {
			lv_label_set_text(lbl_right, "");
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) { // Vol up
			lv_label_set_text(lbl_right, LV_SYMBOL_VOLUME_MAX);
		}
		lv_obj_set_style_text_font(lbl_right, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_right, user_secondary_color, 0);

		// Alignment
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE || type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_obj_align(lbl_right, LV_ALIGN_CENTER, 1, 0);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			lv_obj_align(lbl_right, LV_ALIGN_CENTER, 0, 0);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			lv_obj_align(lbl_right, LV_ALIGN_CENTER, 0, 0);

			// Rotate to point text down
			lv_obj_update_layout(circ_right); // Ensure sizes are known

			int w = lv_obj_get_width(lbl_right);
			int h = lv_obj_get_height(lbl_right);
			
			// Rotate around the label's center
			lv_obj_set_style_transform_pivot_x(lbl_right, w / 2, 0);
			lv_obj_set_style_transform_pivot_y(lbl_right, h / 2, 0);
			
			lv_obj_set_style_transform_angle(lbl_right, 900, 0); // 900 = 90.0 degrees (units are 0.1)
			
			// Give LVGL a little extra draw area so the rotated glyph isn't clipped
			lv_obj_set_style_transform_width(lbl_right, 8, 0);
			lv_obj_set_style_transform_height(lbl_right, 8, 0);
		}

		// DOWN
		circ_down = lv_obj_create(ring);
		lv_obj_add_style(circ_down, &style_circle, 0);
		lv_obj_set_size(circ_down, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_down, LV_ALIGN_CENTER, 0, MEDIA_R);
		lv_obj_set_style_radius(circ_down, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_down, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_down, LV_OBJ_FLAG_SCROLLABLE);
		lbl_down = lv_label_create(circ_down);

		// Text
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			lv_label_set_text(lbl_down, LV_SYMBOL_VOLUME_MID); // Vol-
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_label_set_text(lbl_down, LV_SYMBOL_CLOSE); // ESC
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE || type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			lv_label_set_text(lbl_down, LV_SYMBOL_DOWN); // Scroll down
		}
		else if (type == BLUETOOTH_MEDIA_CAMERA_PAGE) {
			lv_label_set_text(lbl_down, "");
		}
		lv_obj_set_style_text_font(lbl_down, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_down, user_secondary_color, 0);

		// Alignment
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE || type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_obj_align(lbl_down, LV_ALIGN_CENTER, 0, 0);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE || type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			lv_obj_align(lbl_down, LV_ALIGN_CENTER, 0, 1);
		}

		// LEFT
		circ_left = lv_obj_create(ring);
		lv_obj_add_style(circ_left, &style_circle, 0);
		lv_obj_set_size(circ_left, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_left, LV_ALIGN_CENTER, -MEDIA_R, 0);
		lv_obj_set_style_radius(circ_left, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_left, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_left, LV_OBJ_FLAG_SCROLLABLE);
		lbl_left = lv_label_create(circ_left);

		// Text
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE || type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			lv_label_set_text(lbl_left, LV_SYMBOL_PREV); // Previous || Page down
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_label_set_text(lbl_left, LV_SYMBOL_LEFT); // Previous slide
		}
		else if (type == BLUETOOTH_MEDIA_CAMERA_PAGE) {
			lv_label_set_text(lbl_left, "");
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) { // Vol down
			lv_label_set_text(lbl_left, LV_SYMBOL_VOLUME_MID);
		}
		lv_obj_set_style_text_font(lbl_left, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_left, user_secondary_color, 0);

		// Alignment
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			lv_obj_align(lbl_left, LV_ALIGN_CENTER, 0, 0);
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_obj_align(lbl_left, LV_ALIGN_CENTER, -1, 0);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			lv_obj_align(lbl_left, LV_ALIGN_CENTER, 0, 0);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			lv_obj_align(lbl_left, LV_ALIGN_CENTER, 0, 0);

			// Rotate to point text up
			lv_obj_update_layout(circ_left); // Ensure sizes are known

			int w = lv_obj_get_width(lbl_left);
			int h = lv_obj_get_height(lbl_left);
			
			// Rotate around the label's center
			lv_obj_set_style_transform_pivot_x(lbl_left, w / 2, 0);
			lv_obj_set_style_transform_pivot_y(lbl_left, h / 2, 0);
			
			lv_obj_set_style_transform_angle(lbl_left, 900, 0); // 900 = 90.0 degrees (units are 0.1)
			
			// Give LVGL a little extra draw area so the rotated glyph isn't clipped
			lv_obj_set_style_transform_width(lbl_left, 8, 0);
			lv_obj_set_style_transform_height(lbl_left, 8, 0);
		}

		// CENTER
		circ_center = lv_obj_create(ring);
		lv_obj_add_style(circ_center, &style_circle, 0);
		lv_obj_set_size(circ_center, MEDIA_BTN_SZ, MEDIA_BTN_SZ);
		lv_obj_align(circ_center, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_radius(circ_center, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_scrollbar_mode(circ_center, LV_SCROLLBAR_MODE_OFF);
		lv_obj_clear_flag(circ_center, LV_OBJ_FLAG_SCROLLABLE);
		lbl_center = lv_label_create(circ_center);

		// Text
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			lv_label_set_text(lbl_center, LV_SYMBOL_PLAY); // Pause/Play
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_label_set_text(lbl_center, LV_SYMBOL_FILE); // Blank page
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			lv_label_set_text(lbl_center, LV_SYMBOL_MUTE); // Mute
		}
		else if (type == BLUETOOTH_MEDIA_CAMERA_PAGE) { // Take image/video
			lv_label_set_text(lbl_center, LV_SYMBOL_IMAGE);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) { // Like post
			lv_label_set_text(lbl_center, LV_SYMBOL_OK);
		}
		lv_obj_set_style_text_font(lbl_center, &lv_font_montserrat_18, 0);
		lv_obj_set_style_text_color(lbl_center, user_secondary_color, 0);

		// Alignment
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			lv_obj_align(lbl_center, LV_ALIGN_CENTER, 2, 0);
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			lv_obj_align(lbl_center, LV_ALIGN_CENTER, 1, 0);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			lv_obj_align(lbl_center, LV_ALIGN_CENTER, -1, 0);
		}
		else if (type == BLUETOOTH_MEDIA_CAMERA_PAGE || type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			lv_obj_align(lbl_center, LV_ALIGN_CENTER, 0, 0);
		}

		lv_timer_handler(); // Show now

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
	// Up pressed
	if (ui_btns->up_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_up, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_up, user_primary_color, 0);

		// Send command
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			uint16_t cmd = BLUETOOTH_CMD_VOLUME_UP;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_PRESENTATION_START;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_SCROLL_UP;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_SOCIALS_UP;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
	}
	// Right pressed
	else if (ui_btns->right_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_right, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_right, user_primary_color, 0);

		// Send command
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			uint16_t cmd = BLUETOOTH_CMD_NEXT_TRK;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_PRESENTATION_RIGHT;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_SCROLL_PG_DOWN;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_CMD_VOLUME_UP;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
	}
	// Down pressed
	else if (ui_btns->down_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_down, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_down, user_primary_color, 0);

		// Send command
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			uint16_t cmd = BLUETOOTH_CMD_VOLUME_DOWN;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_PRESENTATION_ESC;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_SCROLL_DOWN;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_SOCIALS_DOWN;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
	}
	// Left pressed
	else if (ui_btns->left_btn == 1) {		
		// Invert circle
		lv_obj_add_style(circ_left, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_left, user_primary_color, 0);

		// Send command
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			uint16_t cmd = BLUETOOTH_CMD_PREV_TRK;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_PRESENTATION_LEFT;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_SCROLL_PG_UP;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_CMD_VOLUME_DOWN;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
	}
	// Center pressed
	else if (ui_btns->select_btn == 1) {
		// Invert circle
		lv_obj_add_style(circ_center, &style_circle_pressed, 0);
		lv_obj_set_style_text_color(lbl_center, user_primary_color, 0);

		// Send command
		if (type == BLUETOOTH_MEDIA_CLASSIC_PAGE) {
			uint16_t cmd = BLUETOOTH_CMD_PLAY_PAUSE;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_PRESENTATION_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_PRESENTATION_BLANK;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SCROLL_PAGE) {
			uint16_t cmd = BLUETOOTH_CMD_MUTE;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_CAMERA_PAGE) {
			uint16_t cmd = BLUETOOTH_CMD_VOLUME_UP;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
		else if (type == BLUETOOTH_MEDIA_SOCIALS_PAGE) {
			// Send script
			uint16_t cmd = BLUETOOTH_SCRIPT_SOCIALS_LIKE;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
		}
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
		// Reset long semaphore to avoid false triggers
		xQueueReset(xRightButtonLongSemaphore);

		// Ensure updated based on NVS
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
		update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);

		// Save to NVS
		lcd_bluetooth_script_selected_set(bluetooth_menu->bluetooth_keyboard_menu.index);
	}
	// Down button pressed
	else if (ui_btns->down_btn == 1) {
		// Update selection
		bluetooth_menu->bluetooth_keyboard_menu.index++;
		update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);

		// Save to NVS
		lcd_bluetooth_script_selected_set(bluetooth_menu->bluetooth_keyboard_menu.index);
	}
	// Right button pressed (+5)
	else if (ui_btns->right_btn == 1) {
		// Update selection
		bluetooth_menu->bluetooth_keyboard_menu.index = bluetooth_menu->bluetooth_keyboard_menu.index + 5;
		update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);

		// Save to NVS
		lcd_bluetooth_script_selected_set(bluetooth_menu->bluetooth_keyboard_menu.index);
	}
	// Long right -> go to index 2 (first user index)
	else if (xSemaphoreTake(xRightButtonLongSemaphore, 0) == pdTRUE) {
		// Update selection
		bluetooth_menu->bluetooth_keyboard_menu.index = 2;
		update_keyboard_menu(&bluetooth_menu->bluetooth_keyboard_menu);

		// Save to NVS
		lcd_bluetooth_script_selected_set(bluetooth_menu->bluetooth_keyboard_menu.index);
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Refresh scrolling index
		keyboard_menu_refresh_from_nvs(&bluetooth_menu->bluetooth_keyboard_menu);

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

		// Refresh scrolling index
		keyboard_menu_refresh_from_nvs(&bluetooth_menu->bluetooth_keyboard_menu);

		// Hide bluetooth keyboard menu
		lv_obj_add_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset static
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
	// Option selected
	else if (ui_btns->select_btn == 1) {
		// Add/Edit selected
		if (bluetooth_menu->bluetooth_keyboard_menu.index == 0) {
			// Deactivate bluetooth
			uint16_t cmd = BLUETOOTH_CMD_DEINIT;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

			// Hide keyboard menu
			lv_obj_add_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Reset static
			do_once = false;
			
			// Switch pages
			ui_menu->page = BLUETOOTH_SCRIPT_ADD_PAGE;
		}
		// Test selected
		else if (bluetooth_menu->bluetooth_keyboard_menu.index == 1) {
			// Send test command
			uint16_t cmd = BLUETOOTH_SCRIPT_OFFSET + 1;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, 0);
		}
		// Category selected
		else {
			// Get category index (direct since no filtering)
			current_category = (uint8_t)(bluetooth_menu->bluetooth_keyboard_menu.index - NUM_KEYBOARD_BASE);
			
			// Hide keyboard menu
			lv_obj_add_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Switch to sub page
			ui_menu->page = BLUETOOTH_KEYBOARD_SUB_PAGE;
		}
	}
}

void lcd_bluetooth_keyboard_sub_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	static bool init = false;
	bluetooth_keyboard_menu_t *submenu = &bluetooth_keyboard_submenu;
	
	// Init once
	if (!init) {
		// Fetch menu
		setup_keyboard_submenu_page(submenu);
		keyboard_submenu_refresh_from_nvs(submenu, current_category);

		// Show
		lv_obj_remove_flag(submenu->main_list, LV_OBJ_FLAG_HIDDEN);

		init = true;
	}
	
	// Up button
	if (ui_btns->up_btn == 1) {
		submenu->index--;
		update_keyboard_submenu(submenu);
	}
	// Down button
	else if (ui_btns->down_btn == 1) {
		submenu->index++;
		update_keyboard_submenu(submenu);
	}
	// Select script
	else if (ui_btns->select_btn == 1) {
		// If no scripts in this category, ignore select
		if (bluetooth_keyboard_submenu.size <= 0) {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "No scripts in selected category");
			#endif
		
			return;
		}

		// Send the script to type out
		uint8_t script_idx = submenu->script_indices[submenu->index];
		uint16_t cmd = BLUETOOTH_SCRIPT_OFFSET + NUM_KEYBOARD_BASE + script_idx;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, 0);
	}
	// Back
	else if (ui_btns->left_btn == 1) {
		// Hide submenu
		lv_obj_add_flag(submenu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Clean up
		lv_obj_clean(submenu->main_list);
		
		// Show top-level keyboard menu
		lv_obj_remove_flag(bluetooth_menu->bluetooth_keyboard_menu.main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset init
		init = false;
		
		// Switch page
		ui_menu->page = BLUETOOTH_KEYBOARD_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Hide submenu
		lv_obj_add_flag(submenu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Clean up
		lv_obj_clean(submenu->main_list);
		
		// Reset init
		init = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu);  // True = home, false = sleep
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
				"\n\nThere, you should see a joinable Wi-Fi network named '" BT_PORTAL_SSID "'. Click on it and enter the password '%s'."
				"\n\nIf you don't see it, please wait a minute or try refreshing."
				"\n\nOnce connected, open up your internet browser of choice and search:\n\n%s\n\nFrom there, follow the on-screen instructions. "
				"DO NOT exit this page until you're done entering what you want into the web portal.";
		
		lv_label_set_text_fmt(instr_lbl, instr_text, bt_wifi_portal_pass, msg);
	
		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, SCRIPT_ADD_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -SCRIPT_ADD_Y_OFFSET, LV_ANIM_ON);
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Turn off web portal
		bluetooth_web_portal_stop();
		
		// Hide right arrow
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Delete objects
		lv_obj_delete(cont); // Deletes children
		
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
		lv_obj_delete(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

static void fmt_addr_str(const ble_addr_t *a, char *out, size_t out_sz)
{
	snprintf(out, out_sz, "%02X:%02X:%02X:%02X:%02X:%02X",
			a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static void prompt_rename_or_del(ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	// Show right and hide left arrow
	lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
	
	// Create and format ins labels
	lv_obj_t *lbl_ins = lv_label_create(ACTIVE_SCR);
	lcd_format_label(lbl_ins, "", user_secondary_color,
			&lv_font_montserrat_30, LV_ALIGN_CENTER, 0, 0);
				 
	lv_obj_t *lbl_exit = lv_label_create(ACTIVE_SCR);
	lcd_format_label(lbl_exit, "DEFAULT", user_secondary_color,
			&lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -16, -1);
				 
	lv_obj_t *lbl_name = lv_label_create(ACTIVE_SCR);
	lcd_format_label(lbl_name, "RENAME", user_secondary_color,
			&lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 13);
				 
	lv_obj_t *lbl_del = lv_label_create(ACTIVE_SCR);
	lcd_format_label(lbl_del, "DELETE", user_secondary_color,
			&lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -13);
	
	while (1) {
		lv_timer_handler();
		
		// Done
		if (xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {			
			// Save preferred peer to whitelist
			esp_err_t err = bluetooth_set_preferred_peer_nvs(&bluetooth_menu->bluetooth_peer_menu.peers[bluetooth_menu->bluetooth_peer_menu.index]);
			#ifdef POLYCAST5_DEBUG
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "bluetooth_set_preferred_peer_nvs failed: %s", esp_err_to_name(err));
			}
			#endif
			
			lv_obj_delete(lbl_exit);
			lv_obj_delete(lbl_name);
			lv_obj_delete(lbl_del);
			lv_obj_delete(lbl_ins);

			lcd_clear_pending_inputs = true; // Clear any false inputs
			
			// Show left and hide right arrow
			lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

			// Show bluetooth menu
			lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Switch pages
			ui_menu->page = BLUETOOTH_PAGE;
			
			// Go back
			return;
		}
		// Rename
		else if (xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
			
			lv_obj_delete(lbl_exit);
			lv_obj_delete(lbl_name);
			lv_obj_delete(lbl_del);
			lv_obj_delete(lbl_ins);
			
			lcd_clear_pending_inputs = true; // Clear any false inputs

			// Show left and hide right arrow
			lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
			
			// Prompt to enter name
			ui_menu->page = BLUETOOTH_RENAME_PEER_PAGE;
			
			// Go back
			return;
		}
		// Delete
		else if (xSemaphoreTake(xDownButtonSemaphore, 0) == pdTRUE) {			
			lv_obj_delete(lbl_exit);
			lv_obj_delete(lbl_name);
			lv_obj_delete(lbl_del);
			lv_obj_delete(lbl_ins);
			
			lcd_clear_pending_inputs = true; // Clear any false inputs

			// Delete selected peer from NVS
			ble_addr_t addr = bluetooth_menu->bluetooth_peer_menu.peers[bluetooth_menu->bluetooth_peer_menu.index];
			bluetooth_remove_peer_nvs(&addr);

			// Check size
			bluetooth_peer_info_t tmp[BT_MAX_PEERS];
			int n = bluetooth_get_peers_list_nvs(tmp, BT_MAX_PEERS);
			// If empty now, forget all
			if (n == 0) {
				// Active bluetooth
				uint16_t cmd = BLUETOOTH_CMD_INIT;
				xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
				
				// Forget all bluetooth bonding keys
				cmd = BLUETOOTH_CMD_UNPAIR_ALL_NO_REINIT;
				xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);
			}
			
			// Show left and hide right arrow
			lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

			// Force next rebuild of the known-devices list
			lv_obj_clean(bluetooth_menu->bluetooth_peer_menu.main_list);
			
	 		// Switch pages
			ui_menu->page = BLUETOOTH_KNOWN_DEVICES_PAGE;
			
			// Go back
			return;
		}
		
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

static void peer_menu_build(bluetooth_peer_menu_t *pm)
{
	// Read cached peers (BT stays OFF)
	bluetooth_peer_info_t tmp[BT_MAX_PEERS];
	int n = bluetooth_get_peers_list_nvs(tmp, BT_MAX_PEERS);

	// Size (+1 for the "Allow new devices" row)
	pm->size = (n > 0) ? (n + 1) : 1;

	// Row 0
	strncpy(pm->labels[0], "Pair New Device", sizeof(pm->labels[0]));
	pm->btns[0] = lv_list_add_btn(pm->main_list, NULL, pm->labels[0]);
	lv_obj_set_size(pm->btns[0], 200, 30);

	// Style selected
	if (0 == pm->index) {
		lv_obj_add_style(pm->btns[0], &pm->sel_style, 0);
	}
	else {
		lv_obj_add_style(pm->btns[0], &pm->btn_style, 0);
	}

	// Label center
	lv_obj_t *lbl0 = lv_obj_get_child(pm->btns[0], 0);
	lv_label_set_long_mode(lbl0, LV_LABEL_LONG_SCROLL);
	lv_obj_set_style_text_align(lbl0, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(lbl0, LV_ALIGN_CENTER, 0, 0);

	// Rows 1..n: saved peers (shifted by +1)
	for (int i = 0; i < n; ++i) {
		const int row = i + 1;

		// Store
		pm->peers[row] = tmp[i].addr;

		// Label text
		char label[32] = {0};
		// Use name if it exists, else use address
		if (!bluetooth_get_peer_label_nvs(&pm->peers[row], label, sizeof(label)) || label[0] == '\0') {
			fmt_addr_str(&pm->peers[row], label, sizeof(label));
		}

		strncpy(pm->labels[row], label, sizeof(pm->labels[row]) - 1);
		pm->labels[row][sizeof(pm->labels[row]) - 1] = '\0';

		// Style button
		pm->btns[row] = lv_list_add_btn(pm->main_list, NULL, pm->labels[row]);
		lv_obj_set_size(pm->btns[row], 200, 30);

		// Style selected
		if (row == pm->index) {
			lv_obj_add_style(pm->btns[row], &pm->sel_style, 0);
		}
		else {
			lv_obj_add_style(pm->btns[row], &pm->btn_style, 0);
		}

		// Format label
		lv_obj_t *lbl = lv_obj_get_child(pm->btns[row], 0);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
		lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
	}

	// Layout
	pm->cont = (pm->size > 0) ? lv_obj_get_parent(pm->btns[0]) : pm->main_list;
	lv_obj_set_flex_flow(pm->cont,  LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(pm->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(pm->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

	// Clamp
	if (pm->index >= pm->size) {
		pm->index = pm->size - 1;
	}
	if (pm->index < 0) {
		pm->index = 0;
	}

	// Scroll
	if (pm->size > 0) {
		lv_obj_scroll_to_view(pm->btns[pm->index], LV_ANIM_OFF);
	}
}

void lcd_bluetooth_known_devices_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	static bool init = false;
	bluetooth_peer_menu_t *peer_menu = &bluetooth_menu->bluetooth_peer_menu;
	
	// Init once
	if (!init) {
		// Fetch menu
		setup_known_devices_page(peer_menu);
		peer_menu_build(peer_menu);

		init = true;
	}
	
	// Up button
	if (ui_btns->up_btn == 1) {
		peer_menu->index--;
		update_known_devices_menu(peer_menu);
	}
	// Down button
	else if (ui_btns->down_btn == 1) {
		peer_menu->index++;
		update_known_devices_menu(peer_menu);
	}
	// Selected peer
	else if (ui_btns->select_btn == 1 && peer_menu->size > 0) {
		// Go to 'pair new' page
		if (peer_menu->index == 0) {
			// Hide peer menu
			lv_obj_add_flag(peer_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Clean up
			lv_obj_clean(peer_menu->main_list);
			
			// Reset init
			init = false;
			
			// Switch page
			ui_menu->page = BLUETOOTH_PAIR_NEW_PAGE;
		}
		// Yes whitelist: set specific peer only (in prompt_rename_or_del 'done')
		else {
			// Hide peer menu
			lv_obj_add_flag(peer_menu->main_list, LV_OBJ_FLAG_HIDDEN);
			
			// Clean up
			lv_obj_clean(peer_menu->main_list);
			
			// Reset init
			init = false;
			
			prompt_rename_or_del(ui_menu, bluetooth_menu);
		}
	}
	// Back
	else if (ui_btns->left_btn == 1) {
		// Hide peer menu
		lv_obj_add_flag(peer_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Clean up
		lv_obj_clean(peer_menu->main_list);
		
		// Show bluetooth menu
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset init
		init = false;
		
		// Switch page
		ui_menu->page = BLUETOOTH_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Hide peer menu
		lv_obj_add_flag(peer_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Clean up
		lv_obj_clean(peer_menu->main_list);
		
		// Show bluetooth menu
		lv_obj_remove_flag(bluetooth_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Reset init
		init = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu);  // True = home, false = sleep
	}
}

void lcd_bluetooth_pair_new_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	#define PAIR_NEW_Y_OFFSET 40
	
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
		lv_label_set_text(title_lbl, "Known Devices:");
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

		// Set custom text
		const char *instr_text =
				"The known devices page (prior) allows you to select a specific device to pair to from any of the devices you've paired to in the past.\n\n"
				"This can be great for pranks or just switching between various devices such as PC or phone without having to re-pair every time.\n\n"
				"Right now the Bluetooth whitelist has been cleared, allowing PolyCast5 to connect to a new device like normal.\n\n"
				"For this to work, please also walk out of range or turn off Bluetooth on any previously known devices.\n\n"
				"Afterwards, you can repair normally using '%d' as the pin.";		
		
		// Load pairing key from NVS
		uint32_t pairing_key;
		bluetooth_pairing_key_load_nvs(&pairing_key);
		lv_label_set_text_fmt(instr_lbl, instr_text, pairing_key);

		lv_timer_handler();
		
		// No whitelist: anyone can pair
		bluetooth_clear_peers_list_nvs(true); // Clear preferred peer
	
		// Active bluetooth
		uint16_t cmd = BLUETOOTH_CMD_INIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		init = true;
	}
	
	if (ui_btns->up_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, PAIR_NEW_Y_OFFSET, LV_ANIM_ON);
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -PAIR_NEW_Y_OFFSET, LV_ANIM_ON);
	}
	// Go back
	else if (ui_btns->left_btn) {
		// Deactivate bluetooth
		uint16_t cmd = BLUETOOTH_CMD_DEINIT;
		xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

		// Delete objects
		lv_obj_delete(cont); // Deletes children
		
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
		lv_obj_delete(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
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
		memcpy(display, bt_name_buf, cur_pos);
	}
	
	// Get current
	display[cur_pos] = cur_char;
	display[len] = '\0';
	
	// Set text and re-center
	lv_label_set_text(lbl_display, display);
	lv_obj_align(lbl_display, LV_ALIGN_CENTER, 0, 30);
}

void lcd_bluetooth_rename_peer_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu)
{
	#define BT_NUM_CHAR_ROWS 4

	static const char *bt_char_rows[BT_NUM_CHAR_ROWS] = {
		"_ABCDEFGHIJKLMNOPQRSTUVWXYZ",
		"abcdefghijklmnopqrstuvwxyz",
		"0123456789",
		"!@#$%^&*()-_=+[]{};:'\",<>/?\\|`~"
	};
	
	// Declare statics
	static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};
	static int cur_pos = 0; // User position
	static int row_idx = 0; // Which character row is active
    static int char_idx = 0; // Index within that row
	static char cur_char = '_';
	static lv_obj_t *lbl_dirs = NULL;
	static lv_obj_t *lbl_chars = NULL;
	static lv_obj_t *lbl_user_in = NULL;
	
	// Create initial label
	if (!lbl_user_in) {
		// Auto-fill previous
		ble_addr_t peer_addr = bluetooth_menu->bluetooth_peer_menu.peers[bluetooth_menu->bluetooth_peer_menu.index];
		char prefill[MAX_BT_NAME_LEN + 1] = {0}; // Buffer
		bluetooth_get_peer_label_nvs(&peer_addr, prefill, sizeof(prefill));

		// Copy the old name into buffer
		strncpy(bt_name_buf, prefill, MAX_CUSTOM_NAME_LEN);

		// Place cursor at the end
		cur_pos = strlen(bt_name_buf);
		
		// Starting char
		row_idx = 0;
		char_idx = 0;
		cur_char = bt_char_rows[row_idx][char_idx];
		
		lbl_user_in = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_user_in, "", user_secondary_color,
				&lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
						 
		lbl_dirs = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_dirs, "      Enter device name:\nPress HOME to cycle chars.", user_secondary_color,
				&lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -31);
						 
		lbl_chars = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_chars, "(Up to 12 characters)", user_secondary_color,
				&lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
						 
		update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
	}

	/* User input */
	// Cycle chars
	if (ui_btns->home_btn) {
		// Cycle character row
		row_idx = (row_idx + 1) % BT_NUM_CHAR_ROWS;
		char_idx = 0; // Reset within row
		
		// New current char
		cur_char = bt_char_rows[row_idx][char_idx];
		
		update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
	}
	// If up, iterate up
	else if (ui_btns->up_btn) {
		// Increment with wrap
		size_t row_len = strlen(bt_char_rows[row_idx]);
		char_idx = (char_idx + 1) % (int)row_len;
		cur_char = bt_char_rows[row_idx][char_idx];
		
		// Save to array
		bt_name_buf[cur_pos] = cur_char;
		
		update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
	}
	// If down, iterate down
	else if (ui_btns->down_btn) {
		// Decrement with wrap
		size_t row_len = strlen(bt_char_rows[row_idx]);
		char_idx = (char_idx + (int)row_len - 1) % (int)row_len;
		cur_char = bt_char_rows[row_idx][char_idx];
		
		// Save to array
		bt_name_buf[cur_pos] = cur_char;
		
		update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
	}
	// Can back out if at start
	else if (ui_btns->left_btn && cur_pos == 0) {
		// Delete labels since no longer used
		lv_obj_delete(lbl_user_in);
		lv_obj_delete(lbl_dirs);
		lv_obj_delete(lbl_chars);
		
		// Reset statics for next time
		lbl_user_in = lbl_chars = lbl_dirs = NULL;
		cur_pos = row_idx = char_idx = 0;
		cur_char = '_';
		memset(bt_name_buf, 0, sizeof bt_name_buf);

		// Force next rebuild of the known-devices list
		lv_obj_clean(bluetooth_menu->bluetooth_peer_menu.main_list);
		
 		// Switch to previous page
		ui_menu->page = BLUETOOTH_KNOWN_DEVICES_PAGE;
		return;
	}
	// Power off
	else if (ui_btns->pwr_btn) {
		// Delete labels since no longer used
		lv_obj_delete(lbl_user_in);
		lv_obj_delete(lbl_dirs);
		lv_obj_delete(lbl_chars);
		
		// Reset statics for next time
		lbl_user_in = lbl_chars = lbl_dirs = NULL;
		cur_pos = row_idx = char_idx = 0;
		cur_char = '_';
		memset(bt_name_buf, 0, sizeof bt_name_buf);
				
		// Force next rebuild of the known-devices list
		lv_obj_clean(bluetooth_menu->bluetooth_peer_menu.main_list);
		
 		lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
	}
	// If left and not at start
	else if (ui_btns->left_btn && cur_pos != 0) {
		// Clear the current slot
		bt_name_buf[cur_pos] = '\0';
	
		// De-increment left
		if (cur_pos > 0) {
			cur_pos--;
		}
	
		// Reload row/idx from the new slot's char
		char target = bt_name_buf[cur_pos] ? bt_name_buf[cur_pos] : '_';
		for (row_idx = 0; row_idx < BT_NUM_CHAR_ROWS; row_idx++) {
			const char *row = bt_char_rows[row_idx];
			const char *p = strchr(row, target);
			
			if (p) {
				char_idx = (int)(p - row);
				break;
			}
		}
		cur_char = bt_char_rows[row_idx][char_idx];
		
		update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
	}
	// If right
	else if (ui_btns->right_btn) {
		// Handle case where up/down wasn't pressed
		bt_name_buf[cur_pos] = cur_char;
		
		// If not yet at end
		if (cur_pos < MAX_CUSTOM_NAME_LEN - 1) {
			cur_pos++;
			bt_name_buf[cur_pos] = '\0';
			char_idx = 0;
			cur_char = bt_char_rows[row_idx][char_idx];
		}
		else {
			bt_name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
		}
		
		update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
	}
	// If save button pressed
	else if (ui_btns->select_btn) {
		// Save final
        if (cur_pos < MAX_CUSTOM_NAME_LEN) {
			bt_name_buf[cur_pos] = cur_char;

			// Terminate one past the last written char if room, else clamp
			size_t term = (cur_pos + 1 <= MAX_CUSTOM_NAME_LEN) ? (cur_pos + 1) : MAX_CUSTOM_NAME_LEN;
			bt_name_buf[term] = '\0';
	    }
	    
		bt_name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
		memcpy(saved_name, bt_name_buf, MAX_CUSTOM_NAME_LEN + 1);
		
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Device name: %s", saved_name);
		#endif
		
		// Delete labels since no longer used
		lv_obj_delete(lbl_user_in);
		lv_obj_delete(lbl_dirs);
		lv_obj_delete(lbl_chars);
		
		// Reset statics for next time
		lbl_user_in = lbl_chars = lbl_dirs = NULL;
		cur_pos = row_idx = char_idx = 0;
		cur_char = '_';
		
		// Clamp to MAX_BT_NAME_LEN to match storage buffer
		saved_name[MAX_BT_NAME_LEN] = '\0';
		
		// Save the chosen label for the selected peer
		int idx = bluetooth_menu->bluetooth_peer_menu.index;
		bluetooth_set_peer_label_nvs(&bluetooth_menu->bluetooth_peer_menu.peers[idx], saved_name);
		
		// Update in-memory copy so the UI shows it after rebuild
		strncpy(bluetooth_menu->bluetooth_peer_menu.labels[idx], saved_name, sizeof(bluetooth_menu->bluetooth_peer_menu.labels[idx]) - 1);
		bluetooth_menu->bluetooth_peer_menu.labels[idx][sizeof(bluetooth_menu->bluetooth_peer_menu.labels[idx]) - 1] = '\0';
		
		// Force next rebuild of the known-devices list
		lv_obj_clean(bluetooth_menu->bluetooth_peer_menu.main_list);
		
		// Go back to list page
		ui_menu->page = BLUETOOTH_KNOWN_DEVICES_PAGE;
		return;
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