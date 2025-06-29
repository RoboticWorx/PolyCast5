#ifndef LCD_WIFI_FUNCS_H
#define LCD_WIFI_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#define MAX_WIFI_OPTIONS 20
#define MAX_WIFI_SUBOPTIONS 20
#define TOPIC_KEY_LEN 16

#define WIFI_MENU_NS "wf_mn_ns" // NVS namespace
#define WIFI_MENU_KEY_COUNT "wf_mn_ke" // u8: number of saved topic keys
#define WIFI_MENU_KEY_FMT "wf_mn%02d" // Blob key format

#define WIFI_TOPIC_NS "wf_tp_ns"
#define WIFI_TOPIC_KEY_COUNT "wf_tp_ke"
#define WIFI_TOPIC_KEY_FMT "wf_tp%02d"

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
void lcd_wifi_beacon_page(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns);
void lcd_wifi_data_page(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns);
void lcd_wifi_sync_page(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns);
void lcd_wifi_send_page(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns);
void lcd_wifi_setup_send_page(wifi_menu_t *wifi_menu);

esp_err_t lcd_wifi_menu_nvs_load(wifi_menu_t *menu);
esp_err_t lcd_wifi_menu_nvs_save(const wifi_menu_t *menu);
esp_err_t lcd_wifi_topic_keys_nvs_load(wifi_menu_t *menu);
esp_err_t lcd_wifi_topic_keys_nvs_save(const wifi_menu_t *menu);

void lcd_wifi_create_custom_name(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu, ui_btns_t *ui_btns);

#ifdef POLYCAST5_WIFI_DUMP_NVS
	void lcd_wifi_dump_menu_nvs(void);
	void lcd_wifi_dump_wifi_topic_nvs(void);
#endif

#endif // LCD_WIFI_FUNCS_H