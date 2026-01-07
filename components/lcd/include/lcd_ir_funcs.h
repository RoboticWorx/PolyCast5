#ifndef LCD_IR_FUNCS_H
#define LCD_IR_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#include <string.h>

#define MAX_IR_OPTIONS 33 // MAX_IR_OPTIONS - 3 default is num signals
#define MAX_CUSTOM_NAME_LEN 12

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    lv_obj_t *btns[MAX_IR_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
    lv_style_t btn_style;
    lv_style_t sel_style;
    lv_style_t name_style;
    lv_style_t name_sel_style;
    lv_obj_t *cont;
} ir_menu_t;

extern ir_menu_t ir_menu;

/**
 * @brief Allows option to remove or rename a remote/signal or create/delete remotes
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu IR menu structure
 */
void lcd_ir_edit_remotes(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t *ir_menu );

/**
 * @brief Create custom name for IR remote/signal and save
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu IR menu structure
 */
void lcd_ir_create_custom_name(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t *ir_menu);

/**
 * @brief Create initial structures to display IR data
 *
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_ir_setup_page(ir_menu_t *ir_menu);

/**
 * @brief Update the current IR menu based on user input
 *
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_ir_update_menu(ir_menu_t *ir_menu);

/**
 * @brief Builds the menu for the current remote
 *
 * @param [in] menu Infrared menu structure
 * @param [in] current_remote Index of the current remote
 */
void lcd_ir_build_current_menu(ir_menu_t *menu, size_t current_remote);

/**
 * @brief Prompts user to send a signal to save then saves that signal
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_ir_save_new_signal(ui_menu_t *ui_menu, ir_menu_t *ir_menu);

#endif // LCD_IR_FUNCS_H