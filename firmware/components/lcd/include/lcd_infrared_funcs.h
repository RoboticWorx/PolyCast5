#ifndef LCD_INFRARED_FUNCS_H
#define LCD_INFRARED_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#include <string.h>

#define MAX_IR_OPTIONS 20
#define MAX_CUSTOM_NAME_LEN 12

// Forward-declare structs (from lcd_funcs.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    char *options[MAX_IR_OPTIONS];
    lv_obj_t *btns[MAX_IR_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
} ir_menu_t;

extern ir_menu_t ir_menu;

void lcd_infrared_edit_remotes(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns);

/**
 * @brief Clears all NVS for ir_names namespace
 */
void lcd_infrared_ir_menu_nvs_clear(void);

/**
 * @brief Removes a given index from NVS
 *
 * @param [in] ir_menu IR menu structure
 * @param [in] idx Index to delete
 *
 * @returns ESP error status
 */
esp_err_t lcd_infrared_ir_menu_nvs_delete(ir_menu_t *ir_menu, uint8_t idx);

/**
 * @brief Extracts IR menu options from NVS
 *
 * @param [in] ir_menu IR menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_infrared_ir_menu_nvs_load(ir_menu_t *ir_menu);

/**
 * @brief Saves IR menu options to NVS
 *
 * @param [in] ir_menu IR menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_infrared_ir_menu_nvs_save(const ir_menu_t *ir_menu);

/**
 * @brief Create custom name for IR remote/signal and save to options in NVS
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu Infrared menu structure
 * @param [in] ui_btns UI buttons structure
 */
void lcd_infrared_create_custom_name(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns);

/**
 * @brief Create initial structures to display IR data
 *
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_infrared_setup_page(ir_menu_t *ir_menu);

/**
 * @brief Update IR structures to display
 *
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_infrared_update_menu(ir_menu_t *ir_menu);

/**
 * @brief Select IR option to execute
 *
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_infrared_save_new_signal(ir_menu_t *ir_menu);

#endif // LCD_INFRARED_FUNCS_H