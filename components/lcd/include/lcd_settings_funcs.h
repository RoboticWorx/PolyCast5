#ifndef LCD_SETTINGS_FUNCS_H
#define LCD_SETTINGS_FUNCS_H

#include "misc/lv_style.h"
#include "misc/lv_types.h"

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

#define MAX_SETTINGS_OPTIONS 20

typedef struct {
    char *options[MAX_SETTINGS_OPTIONS];
    lv_obj_t *btns[MAX_SETTINGS_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
} settings_menu_t;

extern settings_menu_t settings_menu;

/**
 * @brief Pre-load settings page for quick access
 *
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_setup_page(settings_menu_t *settings_menu);

/**
 * @brief Update settings page based on user input
 *
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_update_menu(settings_menu_t *settings_menu);


#endif // LCD_SETTINGS_FUNCS_H