#ifndef LCD_FUNCS_H
#define LCD_FUNCS_H

#include "lcd_task.h"
#include "lvgl.h"

#define ST7789_CS_PIN 11  // CS for ST7789
#define ST7789_DC_PIN 25  // DC for ST7789
#define ST7789_RST_PIN 26 // RST for ST7789
#define ST7789_LEDK_PIN 10 // BL for ST7789

// Function prototypes matching LVGL 9.2 (adjusted for error message)
void lcd_send_cmd(lv_display_t *disp, const uint8_t *cmd, unsigned int cmd_len,
				  const uint8_t *param, unsigned int param_len);
void lcd_send_color(lv_display_t *disp, const uint8_t *cmd,
					unsigned int cmd_len, unsigned char *data,
					unsigned int data_len);
void lcd_reset(void);

#endif // LCD_FUNCS_H