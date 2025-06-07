#ifndef LCD_ESPNOW_FUNCS_H
#define LCD_ESPNOW_FUNCS_H

#include "esp_err.h"

#include "lvgl.h"

#define ESPNOW_RX_MAC_NS "es_rxm_ns" // NVS namespace
#define ESPNOW_RX_MAC_KEY_COUNT "es_rxm_ke" // u8: number of option
#define ESPNOW_RX_MAC_KEY_FMT "es_rxm%d"

#define ESPNOW_MENU_NS "es_me_ns" // NVS namespace
#define ESPNOW_MENU_KEY_COUNT "es_me_ke" // u8: number of option
#define ESPNOW_MENU_KEY_FMT "es_me%02d"

#define MAX_ESPNOW_OPTIONS 20

#define ESPNOW_MAC_SIZE 6

// Forward-declare structs (from lcd_funcs.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    char *options[MAX_ESPNOW_OPTIONS];
    uint8_t rx_mac[MAX_ESPNOW_OPTIONS][ESPNOW_MAC_SIZE];
    lv_obj_t *btns[MAX_ESPNOW_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
} espnow_menu_t;

extern espnow_menu_t espnow_menu; 

void lcd_espnow_create_custom_name(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns);

void lcd_espnow_setup_page(espnow_menu_t *menu);

void lcd_espnow_update_menu(espnow_menu_t *menu);

void lcd_espnow_get_rx_mac(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns);

esp_err_t lcd_espnow_menu_nvs_save(const espnow_menu_t *menu);

esp_err_t lcd_espnow_menu_nvs_load(espnow_menu_t *menu);

esp_err_t lcd_espnow_rx_mac_nvs_save(const espnow_menu_t *espnow_menu);

esp_err_t lcd_espnow_rx_mac_nvs_load(espnow_menu_t *espnow_menu);

esp_err_t lcd_espnow_rx_mac_nvs_delete(espnow_menu_t *espnow_menu, uint8_t slot);

#endif // LCD_ESPNOW_FUNCS_H