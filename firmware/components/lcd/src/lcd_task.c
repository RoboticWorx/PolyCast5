#include "lcd_task.h"
#include "drivers/display/st7789/lv_st7789.h"
#include "lcd_funcs.h"
#include "lvgl.h"

#include "esp_log.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

static const char *TAG = "LCD_TASK";

// Pin definitions (adjust these based on your hardware)
#define PIN_NUM_MOSI 23 // SPI MOSI
#define PIN_NUM_CLK 18	// SPI SCLK
#define PIN_NUM_CS 5	// Chip Select
#define PIN_NUM_DC 2	// Data/Command
#define PIN_NUM_RST 4	// Reset (optional, set to -1 if not used)

// LCD Task
static void lcd_task(void *pvParameters) {
	lcd_reset();
	lv_init();

	lv_display_t *disp = lv_st7789_create(240, // Horizontal resolution
										  240, // Vertical resolution
										  LV_LCD_FLAG_NONE, // Default flags
										  lcd_send_cmd,		// Command callback
										  lcd_send_color	// Color callback
	);

	lv_st7789_set_invert(disp, false);
	lv_st7789_set_gap(disp, 0, 0);

	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000),
							  0); // Black background

	lv_obj_t *rect = lv_obj_create(scr);
	lv_obj_set_size(rect, 100, 100);
	lv_obj_align(rect, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(rect, lv_color_hex(0xFF0000), 0); // Red rectangle

	for (;;) {
		lv_task_handler();
		vTaskDelay(pdMS_TO_TICKS(5));
	}
}

void lcd_task_create(void) {
	xTaskCreate(lcd_task, "lcd_task", 4096, NULL, 5, NULL);
}