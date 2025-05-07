#ifndef LCD_TASK_H
#define LCD_TASK_H

#define ACTIVE_SCR (lv_scr_act())

/** Create and start the LCD/LVGL FreeRTOS task. */
void lcd_task_create(void);

#endif /* LCD_TASK_H */