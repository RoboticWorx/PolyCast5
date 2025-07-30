#ifndef LCD_HOTKEY_FUNCS_H
#define LCD_HOTKEY_FUNCS_H

#include "lvgl.h"

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

#define MAX_HOTKEY_OPTIONS 5 // Long/short left and home + long left

typedef struct {
	const char *options[MAX_HOTKEY_OPTIONS];
	lv_obj_t *btns[MAX_HOTKEY_OPTIONS];
	int size;
	int index;
	lv_obj_t *cont;
	lv_obj_t *lbl_ins;
	lv_obj_t *lbl_arrow;
	lv_style_t btn_style;
	lv_style_t sel_style;
} hotkey_menu_t;

extern hotkey_menu_t hotkey_menu;

/**
 * @brief Pre-load hotkey page for quick access
 *
 * @param [in] hotkey_menu Hotkey menu structure
 */
void lcd_hotkey_setup_page(hotkey_menu_t *hotkey_menu);

/**
 * @brief Update hotkey menu based on user input
 *
 * @param [in] hotkey_menu Hotkey menu structure
 */
void lcd_hotkey_update_menu(hotkey_menu_t *hotkey_menu);

#endif // LCD_HOTKEY_FUNCS_H