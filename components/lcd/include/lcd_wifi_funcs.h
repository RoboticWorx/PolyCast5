#ifndef LCD_WIFI_FUNCS_H
#define LCD_WIFI_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#define MAX_WIFI_OPTIONS 4
#define MAX_WIFI_SUBOPTIONS 20

#define LORA_OPTIONS_NS "lo_op_ns" // NVS namespace
#define LORA_OPTIONS_KEY_COUNT "lo_op_ke" // u8: number of option
#define LORA_OPTIONS_KEY_FMT "lo_op%02d" // lo_op00, lo_op01 …

#define LORA_ENC_NS "lo_en_ns" // NVS namespace
#define LORA_ENC_KEY_COUNT "lo_en_ke" // u8: number of user remotes
#define LORA_ENC_KEY_FMT "lo_en%02d" // lo_en00, lo_en01 …

// Forward-declare structs (from lcd_funcs.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
	lv_obj_t *btns[MAX_WIFI_SUBOPTIONS];
    lv_obj_t *main_list;
    lv_obj_t *cont;
    lv_style_t btn_style;
	lv_style_t sel_style;
    int size;
    int index;
} wifi_scan_menu_t;

typedef struct {
    char *options[MAX_WIFI_SUBOPTIONS];
    lv_obj_t *btns[MAX_WIFI_SUBOPTIONS];
    int size;
    int index;
    lv_obj_t *cont;
	lv_style_t btn_style;
	lv_style_t sel_style;
} wifi_submenu_t;

typedef struct {
    char* options[MAX_WIFI_OPTIONS];
    lv_obj_t *btns[MAX_WIFI_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	wifi_submenu_t submenu;
	wifi_scan_menu_t scan_menu;
} wifi_menu_t;

extern wifi_menu_t wifi_menu; 

/**
 * @brief Creates the central Wi-Fi page then hides it for quick access
 *
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_setup_page(wifi_menu_t *wifi_menu);

/**
 * @brief Creates the Wi-Fi scan page then hides it for quick access
 *
 * @param [in] wifi_scan_menu Wi-Fi scan menu structure
 */
void lcd_wifi_create_scan_list(wifi_scan_menu_t *wifi_scan_menu);

/**
 * @brief Updates and shows Wi-Fi page user selection
 *
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_update_menu(wifi_menu_t *wifi_menu);

/**
 * @brief Executes when WIFI_SCAN_PAGE selected
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 * @param [in] ui_btns Btn menu structure
 */
void lcd_wifi_scan_page(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns);

void lcd_wifi_get_password(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns);


#endif // LCD_WIFI_FUNCS_H