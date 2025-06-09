#ifndef LCD_ESPNOW_FUNCS_H
#define LCD_ESPNOW_FUNCS_H

#include "polycast5_macros.h"

#include "esp_err.h"

#include "lvgl.h"

#define ESPNOW_RX_MAC_NS "es_rxm_ns" // NVS namespace
#define ESPNOW_RX_MAC_KEY_COUNT "es_rxm_ke" // u8: number of option
#define ESPNOW_RX_MAC_KEY_FMT "es_rxm%d"

#define ESPNOW_MENU_NS "es_me_ns" // NVS namespace
#define ESPNOW_MENU_KEY_COUNT "es_me_ke" // u8: number of option
#define ESPNOW_MENU_KEY_FMT "es_me%02d"

#define ESPNOW_LMK_NS "es_lm_ns"
#define ESPNOW_LMK_KEY_COUNT "es_lm_ke"
#define ESPNOW_LMK_KEY_FMT "es_lm%02d"

#define MAX_ESPNOW_OPTIONS 20

#define ESPNOW_MAC_SIZE 6
#define LMK_LEN 16

// Forward-declare structs (from lcd_funcs.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    lv_obj_t *lbl_send_tx;
    lv_obj_t *lbl_send_rx;
	lv_obj_t *lbl_send_cmd;
	lv_obj_t *lbl_send_box;
	lv_obj_t *lbl_send;
	lv_obj_t *lbl_edit;
	lv_obj_t *arrow_top;
	lv_obj_t *arrow_bot;
	uint8_t cmd_to_send;
} espnow_submenu_t;

typedef struct {
    char *options[MAX_ESPNOW_OPTIONS];
    uint8_t rx_mac[MAX_ESPNOW_OPTIONS][ESPNOW_MAC_SIZE];
    uint8_t lmk[MAX_ESPNOW_OPTIONS][LMK_LEN];
    lv_obj_t *btns[MAX_ESPNOW_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	espnow_submenu_t espnow_submenu;
} espnow_menu_t;

extern espnow_menu_t espnow_menu; 

/**
 * @brief Pre-load ESP-NOW send page labels and data
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
void lcd_espnow_setup_send_page(espnow_menu_t *espnow_menu);

/**
 * @brief Pre-load ESP-NOW page labels and data
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
void lcd_espnow_setup_page(espnow_menu_t *espnow_menu);

/**
 * @brief Update ESP-NOW page labels and data for user input
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
void lcd_espnow_update_menu(espnow_menu_t *espnow_menu);

/**
 * @brief Executes when specific ESP-NOW option is selected
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_espnow_option_selected(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns);

/**
 * @brief Takes user input to create a name for adding/renaming a selected ESP32
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_espnow_create_custom_name(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns);

/**
 * @brief Takes user input to obtain the MAC address of a specific ESP32 receiver
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_espnow_get_rx_mac(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns);

/**
 * @brief Saves new ESP32 menu option and name to NVS
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
esp_err_t lcd_espnow_menu_nvs_save(const espnow_menu_t *menu);

/**
 * @brief Loads all ESP32 menu options and names from NVS
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
esp_err_t lcd_espnow_menu_nvs_load(espnow_menu_t *menu);

/**
 * @brief Saves new ESP32 receiver MAC to NVS
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
esp_err_t lcd_espnow_rx_mac_nvs_save(const espnow_menu_t *espnow_menu);

/**
 * @brief Saves new ESP32 receiver LMK to NVS
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
esp_err_t lcd_espnow_lmk_nvs_save(const espnow_menu_t *espnow_menu);

/**
 * @brief Loads all ESP32 receiver MACs from NVS
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
esp_err_t lcd_espnow_rx_mac_nvs_load(espnow_menu_t *espnow_menu);

/**
 * @brief Loads all ESP32 receiver LMKs from NVS
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
esp_err_t lcd_espnow_lmk_nvs_load(espnow_menu_t *espnow_menu);

/**
 * @brief Deletes a ESP32 receiver MAC from NVS
 *
 * @param [in] espnow_menu ESP-NOW menu structure
 */
esp_err_t lcd_espnow_rx_mac_lmk_nvs_delete(espnow_menu_t *espnow_menu, uint8_t slot);

#ifdef POLYCAST5_ESPNOW_DUMP_NVS
	/**
	 * @brief Logs ESP-NOW NVS state on startup
	 */
	void lcd_espnow_dump_nvs(void);
#endif

#endif // LCD_ESPNOW_FUNCS_H