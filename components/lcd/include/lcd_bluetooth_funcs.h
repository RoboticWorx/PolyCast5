#ifndef LCD_BLUETOOTH_FUNCS_H
#define LCD_BLUETOOTH_FUNCS_H

#include "esp_err.h"
#include "nimble/ble.h"

#include "misc/lv_style.h"
#include "misc/lv_types.h"

#include "bluetooth_funcs.h"
#include "bluetooth_web_portal.h"

#define NUM_BLUETOOTH_OPTIONS 9

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

// Simple “known devices” list
typedef struct {
    char labels[BT_MAX_PEERS][18]; // "AA:BB:CC:DD:EE:FF"
    ble_addr_t peers[BT_MAX_PEERS]; // Identity addresses
    int size;
    int index;
    lv_obj_t *main_list;
    lv_style_t btn_style, sel_style;
    lv_obj_t *btns[BT_MAX_PEERS], *cont;
} bluetooth_peer_menu_t;

typedef struct {
	char *options[NUM_KEYBOARD_BASE + MAX_KEYBOARD_SCRIPTS];
	lv_obj_t *btns[NUM_KEYBOARD_BASE + MAX_KEYBOARD_SCRIPTS];
	int size;
	int index;
	lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	char cat_labels[MAX_CATEGORIES][BT_CAT_LABEL_MAX_LEN + 1]; // For dynamic category names
	uint8_t script_indices[MAX_KEYBOARD_SCRIPTS]; // For mapping submenu positions to script indices
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
	bluetooth_peer_menu_t bluetooth_peer_menu;
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
 * @brief Explains bluetooth autoconnect and what the connection pin is
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_how_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Starts bluetooth and brings up the media controller menu
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 * @param [in] type Type of media controller: music, presentation, etc.
 */
void lcd_bluetooth_media_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu, uint8_t type);

/**
 * @brief Starts bluetooth and brings up the keyboard page to execute predefined texts or USB rubber ducky
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_keyboard_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Keyboard subpage to show scripts of a specific category
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_keyboard_sub_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Known devices page so you can pick a given device to connect to specifically
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_known_devices_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Explains how the known devices page works and turns off the bluetooth whitelist
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_pair_new_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Scrollable instructions to add a script as well as starts the http web portal
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_add_script_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Allow renaming a given bluetooth peer
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_rename_peer_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/** 
 * @brief Sets the last selected script
 *
 * @param [in] idx Index to set
 *
 * @returns ESP error status
 */
esp_err_t lcd_bluetooth_script_selected_set(uint8_t idx);

/** 
 * @brief Gets the last selected script
 *
 * @returns The last selected script index
 */
uint8_t lcd_bluetooth_script_selected_get(void);


#endif // LCD_BLUETOOTH_FUNCS_H