#ifndef LCD_TASK_H
#define LCD_TASK_H

#include "lcd_funcs.h"

#define ACTIVE_SCR (lv_scr_act())

extern ir_menu_t ir_menu;

extern lv_color_t user_primary_color;
extern lv_color_t user_secondary_color;

/** Create and start the LCD/LVGL FreeRTOS task. */
void lcd_task_create(void);

#endif /* LCD_TASK_H */