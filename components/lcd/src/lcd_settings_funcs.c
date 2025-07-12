#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"

#include "nvs.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"

#include "font/lv_symbol_def.h"
#include "core/lv_obj_pos.h"
#include "core/lv_obj.h"
#include "misc/lv_area.h"
#include "nvs_flash.h"
#include "widgets/label/lv_label.h"

#include "lcd_utils.h"
#include "lcd_settings_funcs.h"

#define SETTINGS_COLOR_NS "se_co_ns" // NVS namespace
#define SETTINGS_COLOR_PRIM_KEY "se_pr_ke"
#define SETTINGS_COLOR_SEC_KEY "se_se_ke"

#define COLOR_OPTION_COUNT 23

settings_menu_t settings_menu = {
    .options = {"Set unlock pin", "Change colors", "Adjust haptics", "Adjust sleep timer", "Reboot", "Factory reset"},
    .size = 6,
    .index = 0,
    .cont = NULL,
};

static const lv_color_t primary_color_options[COLOR_OPTION_COUNT] = {
	// Neutrals
	LV_COLOR_MAKE(0x00, 0x00, 0x00), // Black
	LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), // White

	// Warm reds / oranges
	LV_COLOR_MAKE(0x8B, 0x00, 0x00), // Dark Red
	LV_COLOR_MAKE(0xFF, 0x00, 0x00), // Pure Red
	LV_COLOR_MAKE(0xFF, 0x57, 0x22), // Deep Orange
	LV_COLOR_MAKE(0xFF, 0x98, 0x00), // Orange 500
	LV_COLOR_MAKE(0xFF, 0xC1, 0x07), // Amber 500
	LV_COLOR_MAKE(0xFF, 0xD7, 0x00), // Gold
	LV_COLOR_MAKE(0x79, 0x55, 0x48), // Brown
	LV_COLOR_MAKE(0xFF, 0xEB, 0x3B), // Yellow 500
	LV_COLOR_MAKE(0xE9, 0x1E, 0x63), // Pink 500

	// Greens
	LV_COLOR_MAKE(0xCD, 0xDC, 0x39), // Lime 500
	LV_COLOR_MAKE(0x4C, 0xAF, 0x50), // Green 500
	LV_COLOR_MAKE(0x00, 0x8B, 0x00), // 8B Green
	LV_COLOR_MAKE(0x00, 0x64, 0x00), // Dark Green
	LV_COLOR_MAKE(0x00, 0x96, 0x88), // Teal 500

	// Blues and purples
	LV_COLOR_MAKE(0x03, 0xA9, 0xF4), // Light Blue 500
	LV_COLOR_MAKE(0x46, 0x82, 0xB4), // Steel Blue
	LV_COLOR_MAKE(0x00, 0x00, 0x8B), // Dark Blue
	LV_COLOR_MAKE(0x3F, 0x51, 0xB5), // Indigo 500
	LV_COLOR_MAKE(0x9C, 0x27, 0xB0), // Purple 500
	LV_COLOR_MAKE(0xA0, 0x20, 0xF0), // Pure Purple
	LV_COLOR_MAKE(0x30, 0x19, 0x34), // Dark Purple
};

static const lv_color_t secondary_color_options[COLOR_OPTION_COUNT] = {
	// Neutrals
	LV_COLOR_MAKE(0x00, 0x00, 0x00), // Black
	LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), // White

	// Warm reds / oranges
	LV_COLOR_MAKE(0x8B, 0x00, 0x00), // Dark Red
	LV_COLOR_MAKE(0xFF, 0x00, 0x00), // Pure Red
	LV_COLOR_MAKE(0xFF, 0x57, 0x22), // Deep Orange
	LV_COLOR_MAKE(0xFF, 0x98, 0x00), // Orange 500
	LV_COLOR_MAKE(0xFF, 0xC1, 0x07), // Amber 500
	LV_COLOR_MAKE(0xFF, 0xD7, 0x00), // Gold
	LV_COLOR_MAKE(0x79, 0x55, 0x48), // Brown
	LV_COLOR_MAKE(0xFF, 0xEB, 0x3B), // Yellow 500
	LV_COLOR_MAKE(0xE9, 0x1E, 0x63), // Pink 500

	// Greens
	LV_COLOR_MAKE(0xCD, 0xDC, 0x39), // Lime 500
	LV_COLOR_MAKE(0x4C, 0xAF, 0x50), // Green 500
	LV_COLOR_MAKE(0x00, 0x8B, 0x00), // 8B Green
	LV_COLOR_MAKE(0x00, 0x64, 0x00), // Dark Green
	LV_COLOR_MAKE(0x00, 0x96, 0x88), // Teal 500

	// Blues and purples
	LV_COLOR_MAKE(0x03, 0xA9, 0xF4), // Light Blue 500
	LV_COLOR_MAKE(0x46, 0x82, 0xB4), // Steel Blue
	LV_COLOR_MAKE(0x00, 0x00, 0x8B), // Dark Blue
	LV_COLOR_MAKE(0x3F, 0x51, 0xB5), // Indigo 500
	LV_COLOR_MAKE(0x9C, 0x27, 0xB0), // Purple 500
	LV_COLOR_MAKE(0xA0, 0x20, 0xF0), // Pure Purple
	LV_COLOR_MAKE(0x30, 0x19, 0x34), // Dark Purple
};

static bool primary_color_selected = true;

void lcd_settings_setup_page(settings_menu_t *menu)
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

void lcd_settings_update_menu(settings_menu_t *menu)
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

void lcd_settings_colors_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
	#define X_COL_POS 53
	
	// Statics
	static bool do_once = false;
	
	static lv_obj_t *lbl_ins;
	static lv_obj_t *lbl_primary;
	static lv_obj_t *lbl_secondary;
	static lv_obj_t *lbl_selected;
	static lv_obj_t *primary_color_box;
	static lv_obj_t *secondary_color_box;
	
	// Only execute once
	if (!do_once) {	
		primary_color_selected = true;
			
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "Select a color to change:", user_secondary_color,
        			 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

        // Border color for contrast			 
        lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 100); // % darker 
        			 
        // Create color boxes to show the change
	    primary_color_box = lv_obj_create(ACTIVE_SCR);
	    lv_obj_set_size(primary_color_box,  100,  80);
	    lv_obj_align(primary_color_box, LV_ALIGN_CENTER, -X_COL_POS,  10);
	    lv_obj_set_style_bg_color(primary_color_box, user_primary_color, LV_PART_MAIN);
	    lv_obj_set_style_bg_opa(primary_color_box, LV_OPA_COVER, LV_PART_MAIN);
	    lv_obj_set_style_border_width(primary_color_box, 3, LV_PART_MAIN);
	    lv_obj_set_style_border_color(primary_color_box, darker_user_primary_color, LV_PART_MAIN);
	    
	    secondary_color_box = lv_obj_create(ACTIVE_SCR);
	    lv_obj_set_size(secondary_color_box,  100,  80);
	    lv_obj_align(secondary_color_box, LV_ALIGN_CENTER, X_COL_POS,  10);
	    lv_obj_set_style_bg_color(secondary_color_box, user_secondary_color, LV_PART_MAIN);
	    lv_obj_set_style_bg_opa(secondary_color_box, LV_OPA_COVER, LV_PART_MAIN);
	    lv_obj_set_style_border_width(secondary_color_box, 3, LV_PART_MAIN);
	    lv_obj_set_style_border_color(secondary_color_box, darker_user_primary_color, LV_PART_MAIN);
	    
	    // Text labels
	    lbl_primary = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_primary, "Primary", user_secondary_color,
        			 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, -X_COL_POS, 43);
        			 
        lbl_secondary = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_secondary, "Secondary", user_primary_color,
        			 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, X_COL_POS, 43);
        			 
        lbl_selected = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_selected, LV_SYMBOL_CLOSE, user_secondary_color,
        			 &lv_font_montserrat_30, LV_ALIGN_CENTER, -X_COL_POS, 18);
		
		do_once = true;
	}
	
	// Select a color
	if (ui_btns->select_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_primary);
		lv_obj_delete(lbl_secondary);
		lv_obj_delete(lbl_selected);
		lv_obj_delete(primary_color_box);
		lv_obj_delete(secondary_color_box);
		
		// Reset statics
		lbl_ins = primary_color_box = secondary_color_box = lbl_primary = lbl_secondary = lbl_selected = NULL;
		do_once = false;
		
		// Show top and bottom arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Hide right
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_COLORS_SEL_PAGE;
	}
	// Switch selected right
	else if (ui_btns->right_btn == 1) {	
		// Move selected from primary to secondary
		if (primary_color_selected) {
			lv_obj_set_x(lbl_selected, X_COL_POS);
			lv_obj_set_style_text_color(lbl_selected, user_primary_color, 0);
		}
		else { // Secondary to primary
			lv_obj_set_x(lbl_selected, -X_COL_POS);
			lv_obj_set_style_text_color(lbl_selected, user_secondary_color, 0);
		}
		primary_color_selected = !primary_color_selected;
	}
	// Switch selected left
	else if (ui_btns->left_btn == 1 && !primary_color_selected) {	
		lv_obj_set_x(lbl_selected, -X_COL_POS);
		lv_obj_set_style_text_color(lbl_selected, user_secondary_color, 0);
		primary_color_selected = !primary_color_selected;
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_primary);
		lv_obj_delete(lbl_secondary);
		lv_obj_delete(lbl_selected);
		lv_obj_delete(primary_color_box);
		lv_obj_delete(secondary_color_box);
		
		// Reset statics
		lbl_ins = primary_color_box = secondary_color_box = lbl_primary = lbl_secondary = lbl_selected = NULL;
		do_once = false;
		
		// Show settings list
		lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show top and bottom arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Hide right
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_primary);
		lv_obj_delete(lbl_secondary);
		lv_obj_delete(primary_color_box);
		lv_obj_delete(secondary_color_box);
		
		// Reset statics
		lbl_ins = primary_color_box = secondary_color_box = lbl_primary = lbl_secondary = NULL;
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_settings_colors_sel_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
	#define X_SEL_POS 62
	
	// Statics
	static bool do_once = false;
	static uint8_t new_color_idx = 0;
	
	static lv_obj_t *lbl_ins;
	static lv_obj_t *lbl_arr;
	static lv_obj_t *old_color_box;
	static lv_obj_t *new_color_box;
	
	// Only execute once
	if (!do_once) {
		new_color_idx = 0;
			    
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "Use up/down to adjust.", user_secondary_color,
        			 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 18);
        			 
		lbl_arr = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_arr, LV_SYMBOL_MINUS LV_SYMBOL_RIGHT, user_secondary_color,
        			 &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 10);
        
        // Border color for contrast			 
        lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 100); // % darker 
        
        // Create color boxes to show the change
	    old_color_box = lv_obj_create(ACTIVE_SCR);
	    lv_obj_set_size(old_color_box,  60,  60);
	    lv_obj_align(old_color_box, LV_ALIGN_CENTER, -X_SEL_POS,  10);
	    lv_obj_set_style_bg_opa(old_color_box, LV_OPA_COVER, LV_PART_MAIN);
	    lv_obj_set_style_border_width(old_color_box, 3, LV_PART_MAIN);
	    
	    // Set 'old' box to what was selected
		if (primary_color_selected) {
			lv_obj_set_style_bg_color(old_color_box, user_primary_color, LV_PART_MAIN);
	    	lv_obj_set_style_border_color(old_color_box, darker_user_primary_color, LV_PART_MAIN);
		}
		else {
			lv_obj_set_style_bg_color(old_color_box, user_secondary_color, LV_PART_MAIN);
	    	lv_obj_set_style_border_color(old_color_box, darker_user_primary_color, LV_PART_MAIN);
		}

        // New color
	    new_color_box = lv_obj_create(ACTIVE_SCR);
	    lv_obj_set_size(new_color_box,  60,  60);
	    lv_obj_align(new_color_box, LV_ALIGN_CENTER, X_SEL_POS,  10);
	    lv_obj_set_style_bg_color(new_color_box, user_secondary_color, LV_PART_MAIN);
	    lv_obj_set_style_bg_opa(new_color_box, LV_OPA_COVER, LV_PART_MAIN);
	    lv_obj_set_style_border_width(new_color_box, 3, LV_PART_MAIN);
	    lv_obj_set_style_border_color(new_color_box, user_secondary_color, LV_PART_MAIN);
	    
	    const lv_color_t *opts = primary_color_selected ? primary_color_options : secondary_color_options;
        lv_color_t c = opts[new_color_idx];
        
        // Skip if forbidden color (current or secondary)
        while(lv_color_eq(c, user_primary_color) || lv_color_eq(c, user_secondary_color)) {
	        new_color_idx = (new_color_idx + 1) % COLOR_OPTION_COUNT;
	        c = opts[new_color_idx];
	    }
        lv_obj_set_style_bg_color(new_color_box, c, LV_PART_MAIN);
        lv_obj_set_style_border_color(new_color_box, c, LV_PART_MAIN);

		do_once = true;
	}
	
	// Increment new color up
	if (ui_btns->up_btn == 1) {
		// Pick which palette to use
	    const lv_color_t *opts = primary_color_selected ? primary_color_options : secondary_color_options;
                                
		// Increment with wrap
		new_color_idx = (new_color_idx + 1) % COLOR_OPTION_COUNT;
		
		// Assign to index of selected
        lv_color_t c = opts[new_color_idx];
        
        // Skip if forbidden color (current or secondary)
	    while(lv_color_eq(c, user_primary_color) || lv_color_eq(c, user_secondary_color)) {
	        new_color_idx = (new_color_idx + 1) % COLOR_OPTION_COUNT;
	        c = opts[new_color_idx];
	    }
        
        // Show
        lv_obj_set_style_bg_color(new_color_box, c, LV_PART_MAIN);
        lv_obj_set_style_border_color(new_color_box, c, LV_PART_MAIN);
	}
	// Decrement new color down
	else if (ui_btns->down_btn == 1) {
		// Pick which palette to use
	    const lv_color_t *opts = primary_color_selected ? primary_color_options : secondary_color_options;
                                
		// Decrement with wrap
		new_color_idx = (new_color_idx + COLOR_OPTION_COUNT - 1) % COLOR_OPTION_COUNT;
		
		// Assign to index of selected
        lv_color_t c = opts[new_color_idx];
        
        // Skip if forbidden color (current or secondary)
	    while(lv_color_eq(c, user_primary_color) || lv_color_eq(c, user_secondary_color)) {
	        new_color_idx = (new_color_idx + COLOR_OPTION_COUNT - 1) % COLOR_OPTION_COUNT;
	        c = opts[new_color_idx];
	    }
        
        // Show
        lv_obj_set_style_bg_color(new_color_box, c, LV_PART_MAIN);
        lv_obj_set_style_border_color(new_color_box, c, LV_PART_MAIN);
	}
	// Confirm new color
	else if (ui_btns->select_btn == 1) {
		lv_color_t c = primary_color_selected ? primary_color_options[new_color_idx] : secondary_color_options[new_color_idx];
		if (primary_color_selected) {
			user_primary_color = c;
		}
		else {
			user_secondary_color = c;
		}
		
		// Save to NVS to load at boot
		lcd_settings_color_nvs_save(new_color_idx, primary_color_selected);
		
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_arr);
		lv_obj_delete(old_color_box);
		lv_obj_delete(new_color_box);
		
		// Hide arrows
		lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
		
		// Confirmation text
		lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_rst, "Reloading with\nnew color...", user_secondary_color,
				 &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(1500));
		esp_restart();
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_arr);
		lv_obj_delete(old_color_box);
		lv_obj_delete(new_color_box);
		
		// Reset statics
		lbl_ins = lbl_arr = old_color_box = new_color_box = NULL;
		do_once = false;
		
		// Show settings list
		lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_arr);
		lv_obj_delete(old_color_box);
		lv_obj_delete(new_color_box);
		
		// Reset statics
		lbl_ins = lbl_arr = old_color_box = new_color_box = NULL;
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_settings_factory_rst_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu)
{
	// Statics
	static bool do_once = false;
	
	static lv_obj_t *lbl_ins;
	static lv_obj_t *lbl_note;
	
	// Only execute once
	if (!do_once) {
		lbl_ins = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_ins, "Press select to factory reset.", user_secondary_color,
        			 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 18);
        			 
        lbl_note = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_note, "NOTE: This will erase\n       all user data!", user_secondary_color,
        			 &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);

		do_once = true;
	}
	
	if (ui_btns->select_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_note);
		
		// Reset statics
		lbl_ins = lbl_note = NULL;
		do_once = false;
		
		// Confirmation text
		lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
		lcd_format_label(lbl_rst, "Resetting...", user_secondary_color,
				 &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 0);
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(1000));
		
		ESP_ERROR_CHECK(nvs_flash_erase()); // Factory reset
		esp_restart();
	}
	// Back selected
	else if (ui_btns->left_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_note);
		
		// Reset statics
		lbl_ins = lbl_note = NULL;
		do_once = false;
		
		// Show settings list
		lv_obj_remove_flag(settings_menu->main_list, LV_OBJ_FLAG_HIDDEN);
		
		// Show arrows
		lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
		
		// Switch pages
		ui_menu->page = SETTINGS_PAGE;
	}
	// Home or power off selected
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Delete objects
		lv_obj_delete(lbl_ins);
		lv_obj_delete(lbl_note);
		
		// Reset statics
		lbl_ins = lbl_note = NULL;
		do_once = false;
		
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

void lcd_settings_color_nvs_save(int new_color_idx, bool is_primary)
{
    esp_err_t err;
    nvs_handle_t handle;

    // Open NVS
    err = nvs_open(SETTINGS_COLOR_NS, NVS_READWRITE, &handle);
    ESP_ERROR_CHECK(err);

    // Pick a key for primary vs secondary
    const char *key = is_primary ? SETTINGS_COLOR_PRIM_KEY : SETTINGS_COLOR_SEC_KEY;

    // Store the index
    err = nvs_set_i32(handle, key, new_color_idx);
    ESP_ERROR_CHECK(err);

    //Commit & close
    err = nvs_commit(handle);
    ESP_ERROR_CHECK(err);
    nvs_close(handle);
}

void lcd_settings_color_nvs_load(void)
{
    esp_err_t err;
    nvs_handle_t handle;

	// Open NVS
    err = nvs_open(SETTINGS_COLOR_NS, NVS_READONLY, &handle);
    if(err == ESP_OK) {
        int32_t idx;
        // Get primary color
        if(nvs_get_i32(handle, SETTINGS_COLOR_PRIM_KEY, &idx) == ESP_OK) {
            user_primary_color = primary_color_options[idx];
        }
        
        // Get secondary color
        if(nvs_get_i32(handle, SETTINGS_COLOR_SEC_KEY, &idx) == ESP_OK) {
            user_secondary_color = secondary_color_options[idx];
        }
        nvs_close(handle);
    }
}


