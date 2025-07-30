#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"
#include "misc/lv_area.h"
#include "widgets/label/lv_label.h"
#include "font/lv_symbol_def.h"

#include "lcd_hotkey_funcs.h"
#include "lcd_utils.h"

hotkey_menu_t hotkey_menu = {
	.options = {"Hot1", "Hot2", "Hot3", "Hot4", "Hot5"},
	.size = MAX_HOTKEY_OPTIONS,
	.index = 0,
	.cont = NULL,
};

void lcd_hotkey_setup_page(hotkey_menu_t *menu)
{
	// Create container
	menu->cont = lv_obj_create(ACTIVE_SCR);
	
	// Create instruction labels
	menu->lbl_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(menu->lbl_arrow, LV_SYMBOL_LEFT LV_SYMBOL_MINUS, user_secondary_color,
				&lv_font_montserrat_12, LV_ALIGN_BOTTOM_RIGHT, -43, -44);
	
	menu->lbl_ins = lv_label_create(ACTIVE_SCR);
	lcd_format_label(menu->lbl_ins, "         |\nConfigure\n  hotkeys", user_secondary_color,
				&lv_font_montserrat_14, LV_ALIGN_BOTTOM_RIGHT, -8, -5);
	
	// Format
	lv_obj_set_size(menu->cont, 210, 106);
	lv_obj_center(menu->cont);
	lv_obj_set_style_bg_color(menu->cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(menu->cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_scrollbar_mode(menu->cont, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_scroll_dir(menu->cont, LV_DIR_VER);
	
	// Set flow
	lv_obj_set_flex_flow(menu->cont, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	
	// Set gap
	lv_obj_set_style_pad_gap(menu->cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

	// Prepare styles 
	// Normal button style
	lv_style_init(&menu->btn_style);
	lv_style_set_radius(&menu->btn_style, 8);
	lv_style_set_bg_color(&menu->btn_style, user_primary_color);
	lv_style_set_border_width(&menu->btn_style, 2);
	lv_style_set_border_color(&menu->btn_style, user_secondary_color);
	lv_style_set_border_side(&menu->btn_style, LV_BORDER_SIDE_FULL);
	lv_style_set_text_font(&menu->btn_style, &lv_font_montserrat_16);
	lv_style_set_text_color(&menu->btn_style, user_secondary_color);
	lv_style_set_text_align(&menu->btn_style, LV_TEXT_ALIGN_CENTER);

	// Selected button style
	lv_style_init(&menu->sel_style);
	lv_style_set_radius(&menu->sel_style, 8);
	lv_style_set_bg_color(&menu->sel_style, user_secondary_color);
	lv_style_set_border_width(&menu->sel_style, 2);
	lv_style_set_border_color(&menu->sel_style, user_secondary_color);
	lv_style_set_border_side(&menu->sel_style, LV_BORDER_SIDE_FULL);
	lv_style_set_text_font(&menu->sel_style, &lv_font_montserrat_16);
	lv_style_set_text_color(&menu->sel_style, user_primary_color);
	lv_style_set_text_align(&menu->sel_style, LV_TEXT_ALIGN_CENTER);

	// Create button per option
	for (int i = 0; i < menu->size; i++) {
		menu->btns[i] = lv_btn_create(menu->cont);
		lv_obj_set_size(menu->btns[i], 58, 50);
		
		// Add style
		if (i == menu->index)
			lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
		else
			lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
	
		// Create child label
		lv_obj_t *lbl = lv_label_create(menu->btns[i]);
		lv_label_set_text(lbl, menu->options[i]);
		
		// Format
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
		lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
	}
	
	// Hide for now
	lv_obj_add_flag(menu->cont, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(menu->lbl_ins, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(menu->lbl_arrow, LV_OBJ_FLAG_HIDDEN);
}

void lcd_hotkey_update_menu(hotkey_menu_t *menu)
{
	// Reveal
	lv_obj_remove_flag(menu->cont, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(menu->lbl_ins, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(menu->lbl_arrow, LV_OBJ_FLAG_HIDDEN);

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
}

