#ifndef LCD_TASK_H
#define LCD_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/** Create and start the LCD/LVGL FreeRTOS task. */
void lcd_task_create(void);

#ifdef __cplusplus
}
#endif

#endif /* LCD_TASK_H */