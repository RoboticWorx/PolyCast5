#ifndef LCD_ESPNOW_FUNCS_H
#define LCD_ESPNOW_FUNCS_H

#include "esp_err.h"

#include "lvgl.h"

#include "lcd_lora_funcs.h"

#define MAX_ESPNOW_OPTIONS 20


typedef struct {
    char *options[MAX_LORA_OPTIONS];
    //uint8_t *keys[16];
    lv_obj_t *btns[MAX_LORA_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
} espnow_menu_t;

extern espnow_menu_t espnow_menu; 

void lcd_espnow_setup_page(espnow_menu_t *menu);

void lcd_espnow_update_menu(espnow_menu_t *menu);

void lcd_espnow_get_rx_mac(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu, ui_btns_t *ui_btns);

#endif // LCD_ESPNOW_FUNCS_H