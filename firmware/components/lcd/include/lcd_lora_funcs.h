#ifndef LCD_LORA_FUNCS_H
#define LCD_LORA_FUNCS_H

#include "lvgl.h"

#include "esp_err.h"

#define MAX_LORA_OPTIONS 20

#define LORA_OPTIONS_NS "a_lora_op_ns" // NVS namespace
#define LORA_OPTIONS_KEY_COUNT "a_lora_op_count" // u8: number of user remotes
#define LORA_OPTIONS_KEY_FMT "a_lo%02d" // a_lo00, a_lo01 …

// Forward-declare structs (from lcd_funcs.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    char *options[MAX_LORA_OPTIONS];
    lv_obj_t *btns[MAX_LORA_OPTIONS];
    int size;
    int index;
    lv_obj_t *cont;
	lv_style_t btn_style;
	lv_style_t sel_style;
} lora_submenu_t;

typedef struct {
    char *options[MAX_LORA_OPTIONS];
    lv_obj_t *btns[MAX_LORA_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
	lora_submenu_t submenu;
} lora_menu_t;

extern lora_menu_t lora_menu;

void lcd_lora_subpage_option_selected(ui_menu_t *ui_menu, lora_menu_t *lora_menu, ui_btns_t *ui_btns);

void lcd_lora_subpage_selected(ui_menu_t *ui_menu, lora_menu_t *lora_menu, ui_btns_t *ui_btns);

void lcd_lora_update_submenu(lora_menu_t *menu);

void lcd_lora_setup_page(lora_menu_t *menu);

void lcd_lora_setup_subpage(lora_menu_t *menu);

void lcd_lora_update_menu(lora_menu_t *menu);

void lcd_lora_create_custom_name(ui_menu_t *ui_menu, lora_menu_t *lora_menu, ui_btns_t *ui_btns);

esp_err_t lcd_lora_menu_nvs_save(const lora_menu_t *menu, const char* ns, const char* count, const char* fmt);

esp_err_t lcd_lora_menu_nvs_load(lora_menu_t *menu, const char* ns, const char* count, const char* fmt);

#endif // LCD_LORA_FUNCS_H