#ifndef LCD_HOTKEY_FUNCS_H
#define LCD_HOTKEY_FUNCS_H

#include "espnow_funcs.h"
#include "lora_funcs.h"
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

typedef struct {
	uint8_t active_idx;
	bool has_lora[MAX_HOTKEY_OPTIONS]; // Hot0-Hot5
	bool has_espnow[MAX_HOTKEY_OPTIONS];
	lora_cmd_t lora_cmd[MAX_HOTKEY_OPTIONS];
	espnow_cmd_t espnow_cmd[MAX_HOTKEY_OPTIONS];
} hotkey_cmd_t;

extern hotkey_menu_t hotkey_menu;
extern hotkey_cmd_t hotkey_cmd;

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

/**
 * @brief Executes the selected hotkey configuration option
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] hotkey_menu Hotkey menu structure
 */
void lcd_hotkey_option_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, hotkey_menu_t *hotkey_menu);

/**
 * @brief Loads hotkey commands from NVS
 *
 * @param [in] hotkey_cmd Hotkey command structure
 */
void lcd_hotkey_nvs_load(hotkey_cmd_t *hotkey_cmd);

/**
 * @brief Saves hotkey commands to NVS
 *
 * @param [in] hotkey_cmd Hotkey command structure
 */
void lcd_hotkey_nvs_save(const hotkey_cmd_t *hotkey_cmd);

#endif // LCD_HOTKEY_FUNCS_H