#ifndef LCD_WIFI_FUNCS_H
#define LCD_WIFI_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#define MAX_WIFI_OPTIONS 20
#define MAX_WIFI_SUBOPTIONS 20
#define TOPIC_KEY_LEN 16

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
    lv_obj_t *lbl_send_ins;
	lv_obj_t *lbl_send_cmd;
	lv_obj_t *lbl_send_box;
	lv_obj_t *lbl_send;
	lv_obj_t *lbl_edit;
	lv_obj_t *lbl_receipt;
	lv_obj_t *arrow_top;
	lv_obj_t *arrow_bot;
	uint8_t cmd_to_send;
} wifi_submenu_t;

typedef struct {
    char* options[MAX_WIFI_OPTIONS];
    lv_obj_t *btns[MAX_WIFI_OPTIONS];
    uint8_t topic_keys[MAX_WIFI_OPTIONS][TOPIC_KEY_LEN];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	wifi_submenu_t wifi_submenu;
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
 * @brief Creates the Wi-Fi send page for quick user access
 *
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_setup_send_page(wifi_menu_t *wifi_menu);

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
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_scan_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Gets network password via user input
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_get_password(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Executes Wi-Fi beacon page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_beacon_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Executes Wi-Fi data page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_data_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Prompts user confirmation before sync
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_sync_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Executes Wi-Fi send page to send data via MQTT
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_send_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Gets user input to name a Wi-Fi PolyPlug
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_create_custom_name(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Prompts user confirmation to update the device via OTA
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_ota_confirm_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Display updating screen and update progress bar
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 */
void lcd_wifi_ota_updating_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Loads Wi-Fi menu items from NVS
 *
 * @param [in] wifi_menu Wi-Fi menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_wifi_menu_nvs_load(wifi_menu_t *menu);

/**
 * @brief Saves Wi-Fi menu items to NVS
 *
 * @param [in] wifi_menu Wi-Fi menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_wifi_menu_nvs_save(const wifi_menu_t *menu);

/**
 * @brief Loads Wi-Fi topic keys from NVS
 *
 * @param [in] wifi_menu Wi-Fi menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_wifi_topic_keys_nvs_load(wifi_menu_t *menu);

/**
 * @brief Saves Wi-Fi topic keys to NVS
 *
 * @param [in] wifi_menu Wi-Fi menu structure
 *
 * @returns ESP error status
 */
esp_err_t lcd_wifi_topic_keys_nvs_save(const wifi_menu_t *menu);

#ifdef POLYCAST5_WIFI_DUMP_NVS
	void lcd_wifi_dump_menu_nvs(void);
	void lcd_wifi_dump_wifi_topic_nvs(void);
#endif

#endif // LCD_WIFI_FUNCS_H