#ifndef LCD_SETTINGS_FUNCS_H
#define LCD_SETTINGS_FUNCS_H

#include "misc/lv_style.h"
#include "misc/lv_types.h"

#define MAX_SETTINGS_OPTIONS 20

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

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
 * @brief Executes factory reset page
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_factory_rst_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

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



#endif // LCD_SETTINGS_FUNCS_H