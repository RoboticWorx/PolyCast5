#ifndef LCD_INFRARED_FUNCS_H
#define LCD_INFRARED_FUNCS_H

#include "lvgl.h"

#include <string.h>

#define MAX_IR_OPTIONS 20
#define MAX_CUSTOM_NAME_LEN 10

// Forward-declare structs (from lcd_funcs.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    const char *options[MAX_IR_OPTIONS];
    lv_obj_t *btns[MAX_IR_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
} ir_menu_t;

extern ir_menu_t ir_menu;

void lcd_infrared_create_new_remote(ui_menu_t *ui_menu, ir_menu_t *ir_menu);
void lcd_infrared_create_custom_name(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns);

/**
 * @brief Create initial structures to display IR data
 *
 * @param [in] menu Infrared menu structure
 */
void lcd_infrared_setup_page(ir_menu_t *menu);

/**
 * @brief Update IR structures to display
 *
 * @param [in] menu Infrared menu structure
 */
void lcd_infrared_update_menu(ir_menu_t *menu);

/**
 * @brief Select IR option to execute
 *
 * @param [in] menu Infrared menu structure
 */
void lcd_infrared_save_new_signal(ir_menu_t *menu);

#endif // LCD_INFRARED_FUNCS_H