#ifndef LCD_IR_FUNCS_H
#define LCD_IR_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#include <string.h>

#define MAX_IR_OPTIONS 20
#define MAX_CUSTOM_NAME_LEN 12

#define A_IR_REMOTE_NS "a_ir_rem_ns" // NVS namespace
#define A_REMOTE_KEY_COUNT "a_ir_rem_count" // u8: number of user remotes
#define A_REMOTE_KEY_FMT "a_r%02d" // a_r00, a_r01 …

//#define B_IR_REMOTE_NS "b_ir_rem_ns"
//#define B_REMOTE_KEY_COUNT "b_ir_rem_count"
//#define B_REMOTE_KEY_FMT "b_r%02d"

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

/**
 * @brief Allows option to remove or rename a remote
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu IR menu structure
 * @param [in] ui_btns UI buttons structure
 */
void lcd_ir_edit_remotes(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns);

/**
 * @brief Removes a given index from NVS
 *
 * @param [in] ir_menu IR menu structure
 * @param [in] idx Index to delete
 * @param [in] ns NVS namespace
 * @param [in] count NVS count for number of entries saved
 * @param [in] fmt NVS key to save entries under
 *
 * @returns ESP error status
 */
esp_err_t lcd_ir_ir_menu_nvs_delete(ir_menu_t *ir_menu, uint8_t idx, const char* ns, const char* count, const char* fmt);

/**
 * @brief Extracts IR menu options from NVS
 *
 * @param [in] ir_menu IR menu structure
 * @param [in] ns NVS namespace
 * @param [in] count NVS count for number of entries saved
 * @param [in] fmt NVS key to save entries under
 * 
 * @returns ESP error status
 */
esp_err_t lcd_ir_ir_menu_nvs_load(ir_menu_t *ir_menu, const char* ns, const char* count, const char* fmt);

/**
 * @brief Saves IR menu options to NVS
 *
 * @param [in] ir_menu IR menu structure
 * @param [in] ns NVS namespace
 * @param [in] count NVS count for number of entries saved
 * @param [in] fmt NVS key to save entries under
 * 
 * @returns ESP error status
 */
esp_err_t lcd_ir_ir_menu_nvs_save(const ir_menu_t *ir_menu, const char* ns, const char* count, const char* fmt);

/**
 * @brief Create custom name for IR remote/signal and save to options in NVS
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu Infrared menu structure
 * @param [in] ui_btns UI buttons structure
 */
void lcd_ir_create_custom_name(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns);

/**
 * @brief Create initial structures to display IR data
 *
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_ir_setup_page(ir_menu_t *ir_menu);

/**
 * @brief Update IR structures to display
 *
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_ir_update_menu(ir_menu_t *ir_menu);

/**
 * @brief Select IR option to execute
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu Infrared menu structure
 */
void lcd_ir_save_new_signal(ui_menu_t *ui_menu, ir_menu_t *ir_menu);

#endif // LCD_IR_FUNCS_H