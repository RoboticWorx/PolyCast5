#ifndef LCD_BLUETOOTH_FUNCS_H
#define LCD_BLUETOOTH_FUNCS_H

#include "esp_err.h"

#include "misc/lv_style.h"
#include "misc/lv_types.h"

#define NUM_BLUETOOTH_OPTIONS 3

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
	char *options[NUM_BLUETOOTH_OPTIONS];
	lv_obj_t *btns[NUM_BLUETOOTH_OPTIONS];
	int size;
	int index;
	lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
} bluetooth_keyboard_menu_t;

typedef struct {
	char *options[NUM_BLUETOOTH_OPTIONS];
	lv_obj_t *btns[NUM_BLUETOOTH_OPTIONS];
	int size;
	int index;
	lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	bluetooth_keyboard_menu_t bluetooth_keyboard_menu;
} bluetooth_menu_t;

extern bluetooth_menu_t bluetooth_menu;

/**
 * @brief Pre-load Bluetooth page for quick access
 *
 * @param [in] bluetooth_menu Settings menu structure
 */
void lcd_bluetooth_setup_page(bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Update bluetooth page based on user input
 *
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_update_menu(bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Update bluetooth keyboard page based on user input
 *
 * @param [in] bluetooth_keyboard_menu Bluetooth keyboard menu structure
 */
void lcd_bluetooth_update_keyboard_menu(bluetooth_keyboard_menu_t *bluetooth_keyboard_menu);

/**
 * @brief Signals to start advertising bluetooth and displays pairing instructions
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_pair_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Starts bluetooth and brings up the media controller menu
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_media_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Starts bluetooth and brings up the keyboard page to execute predefined texts or USB rubber ducky
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_keyboard_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);


#endif // LCD_BLUETOOTH_FUNCS_H