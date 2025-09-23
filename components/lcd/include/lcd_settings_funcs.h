#ifndef LCD_SETTINGS_FUNCS_H
#define LCD_SETTINGS_FUNCS_H

#include "esp_err.h"

#include "misc/lv_style.h"
#include "misc/lv_types.h"

#define SETTINGS_MAX_PIN_LEN 5

#define SETTINGS_REMOVE_LOCK_TXT "Remove unlock PIN"
#define SETTINGS_SET_LOCK_TXT "Set unlock PIN"

#define MAX_SETTINGS_OPTIONS 20

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
	bool pin_set;
	bool prompt_pin;
	char unlock_pin[SETTINGS_MAX_PIN_LEN + 1];
	lv_obj_t *pin_container;
	lv_obj_t *lbl_ins;
	lv_obj_t *lbl_back;
	lv_obj_t *lbl_attempts;
} settings_pin_menu_t;

typedef struct {
	char *options[MAX_SETTINGS_OPTIONS];
	lv_obj_t *btns[MAX_SETTINGS_OPTIONS];
	int size;
	int index;
	lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	settings_pin_menu_t pin_menu;
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

/**
 * @brief Pre-creates user pin page to unlock the device
 *
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_setup_pin_page(settings_menu_t *settings_menu);

/**
 * @brief Rebuilds pin boxes from scratch
 *
 * @param [in] pin_container Holds the different pin boxes
 * @param [in] pin_labels Text label for each pin box
 * @param [in] unlock_pin What the text should be
 * @param [in] num_boxes Pin box counter
 * @param [in] num_filled Number of pin boxes
 */
void lcd_settings_rebuild_pin_boxes(lv_obj_t *pin_container, lv_obj_t **pin_labels, char *unlock_pin, int *num_boxes, int num_filled);

/**
 * @brief Prompts user to create a pin to unlock the device
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_pin_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes settings colors page so the user can adjust the primary and secondary device colors
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_colors_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes settings colors selected page so the user can adjust the color chosen to something else
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_colors_sel_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes adjust haptics page so user can change haptic settings
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_adjust_haptics_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes adjust sleep timer page so user can change sleep timer duration
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_sleep_timer_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes adjust RGB LED page so user can change the RGB LED configuration
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_adjust_rgb_led_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes adjust LCD page so user can change the LCD brightness
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_adjust_lcd_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes factory reset page
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_factory_rst_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Shows some basic help info like how to reset the device
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_help_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/* NVS functions */

/**
 * @brief Saves the current primary and secondary colors to NVS
 *
 * @param [in] new_color_idx Selected color index
 * @param [in] is_primary Flag to see if it's the primary or secondary color being updated
 */
void lcd_settings_color_nvs_save(int new_color_idx, bool is_primary);

/**
 * @brief Loads the current primary and secondary colors from NVS
 */
void lcd_settings_color_nvs_load(void);

/**
 * @brief Saves the entered unlock PIN to NVS
 *
 * @param [in] settings_menu_t Settings menu structure
 */
void lcd_settings_pin_nvs_save(const settings_menu_t *menu);

/**
 * @brief Loads the saved unlock PIN from NVS
 *
 * @param [in] settings_menu_t Settings menu structure
 */
void lcd_settings_pin_nvs_load(settings_menu_t *menu);

/**
 * @brief Saves the current pin entry attempts global to NVS
 */
void lcd_settings_pin_attempts_nvs_save(void);

/**
 * @brief Loads the current pin entry attempts into global from NVS
 */
void lcd_settings_pin_attempts_nvs_load(void);

/**
 * @brief Saves the current haptic length and states to NVS
 */
void lcd_settings_haptics_nvs_save(void);

/**
 * @brief Loads the current haptic length and states from NVS
 */
void lcd_settings_haptics_nvs_load(void);

/**
 * @brief Saves the current sleep timer length to NVS
 */
void lcd_settings_sleep_timer_nvs_save(void);

/**
 * @brief Loads the current sleep timer length from NVS
 */
void lcd_settings_sleep_timer_nvs_load(void);

/**
 * @brief Saves the current RGB LED period and total duration to NVS
 */
void lcd_settings_rgb_led_nvs_save(void);

/**
 * @brief Loads the current RGB LED period and total duration from NVS
 */
void lcd_settings_rgb_led_nvs_load(void);

/**
 * @brief Saves the current LCD brightness to NVS
 */
void lcd_settings_lcd_ledc_nvs_save(void);

/**
 * @brief Loads the current LCD brightness from NVS
 */
void lcd_settings_lcd_ledc_nvs_load(void);

#endif // LCD_SETTINGS_FUNCS_H