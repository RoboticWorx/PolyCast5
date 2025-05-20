#ifndef LCD_LORA_FUNCS_H
#define LCD_LORA_FUNCS_H

#include "lvgl.h"

#define MAX_LORA_OPTIONS 20

typedef struct {
    char *options[MAX_LORA_OPTIONS];
    lv_obj_t *btns[MAX_LORA_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
	lv_style_t btn_style;
	lv_style_t sel_style;
	lv_obj_t *cont;
} lora_menu_t;

extern lora_menu_t lora_menu;




void lcd_lora_setup_page(lora_menu_t *menu);

void lcd_lora_update_menu(lora_menu_t *menu);

#endif // LCD_LORA_FUNCS_H