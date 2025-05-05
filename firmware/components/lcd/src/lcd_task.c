#include "lcd_task.h"
#include "font/lv_symbol_def.h"
#include "lcd_funcs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lv_api_map_v8.h"
#include "misc/lv_area.h"
#include "misc/lv_style_gen.h"
#include "resolutiontest.h"

#define ACTIVE_SCR (lv_disp_get_scr_act(lcd_get_display()))

static const char *TAG = "LCD_TASK";

lv_color_t user_primary_color = LV_COLOR_MAKE(0x00, 0x00, 0x8B); 
lv_color_t user_secondary_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);

/*
	// Show image
    lv_obj_t *img = lv_img_create(ACTIVE_SCR);
    lv_img_set_src(img, &resolutiontest);
    lv_obj_center(img);
*/


static void lcd_task(void *pvParameters)
{
	user_primary_color = lv_color_hex(0x00008B);
	
	
	// Set background
    lv_obj_set_style_bg_color(ACTIVE_SCR, user_primary_color, 0);
    lv_obj_set_style_bg_opa(ACTIVE_SCR, LV_OPA_COVER, 0); // Ensure the background is fully opaque
    
    
    // Format center button
    lv_obj_t *user_center_option_button = lv_btn_create(ACTIVE_SCR);
	lv_obj_set_size(user_center_option_button, 175, 45);
	lv_obj_align(user_center_option_button, LV_ALIGN_CENTER, 0, 0);
	
	lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 40); // % darker 
	lv_color_t darker_user_secondary_color = lv_color_darken(user_secondary_color, 20);
	static lv_style_t user_center_option_button_style;
	lv_style_init(&user_center_option_button_style);
	lv_style_set_radius(&user_center_option_button_style, 8); // rounded corners
	lv_style_set_bg_color(&user_center_option_button_style, darker_user_primary_color);
	lv_style_set_bg_grad_color(&user_center_option_button_style, user_primary_color);
	lv_style_set_bg_grad_dir(&user_center_option_button_style, LV_GRAD_DIR_VER);
	lv_style_set_border_width(&user_center_option_button_style, 2);
	lv_style_set_border_color(&user_center_option_button_style, darker_user_secondary_color);
	lv_style_set_shadow_spread(&user_center_option_button_style, 3);
	lv_style_set_shadow_width(&user_center_option_button_style, 6);
	lv_style_set_shadow_offset_x(&user_center_option_button_style, 3);
	lv_style_set_shadow_offset_y(&user_center_option_button_style, 3);
	lv_style_set_shadow_color(&user_center_option_button_style, lv_color_hex(0x000000));
	lv_obj_add_style(user_center_option_button, &user_center_option_button_style, 0);


	// Format labels
	lv_obj_t *user_top_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_top_arrow, LV_SYMBOL_UP, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 0);

	lv_obj_t *user_top_option = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_top_option, "Infrared", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

	/*lv_obj_t *user_left_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_left_arrow, LV_SYMBOL_LEFT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);*/

	lv_obj_t *user_center_option_button_label = lv_label_create(user_center_option_button);
	lcd_format_label(user_center_option_button_label, "LoRa",
					 user_secondary_color, &lv_font_montserrat_30,
					 LV_ALIGN_CENTER, 0, 0);

	lv_obj_t *user_right_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_right_arrow, LV_SYMBOL_RIGHT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_RIGHT_MID, -4, 0);

	lv_obj_t *user_bottom_option = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_bottom_option, "Wi-Fi", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -15);

	lv_obj_t *user_bottom_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_bottom_arrow, LV_SYMBOL_DOWN, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, 0);

	lv_obj_t *battery_icon_text = lv_label_create(ACTIVE_SCR);
	lcd_format_label(battery_icon_text, "100", user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -28, 0);

	lv_obj_t *battery_icon = lv_label_create(ACTIVE_SCR); // 3, 2, 1, EMPTY
	lcd_format_label(battery_icon, LV_SYMBOL_BATTERY_FULL, user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT, -2, -3);


	const char *menu_options[] = {"Infrared", "LoRa", "Wi-Fi", "ESP-NOW", "Settings"};
	const int menu_size = sizeof(menu_options) / sizeof(menu_options[0]);
	int menu_index = 1;
	bool scrolling_up = false;

	TickType_t last = xTaskGetTickCount();
	const TickType_t interval = pdMS_TO_TICKS(500);

	while (1) {
		if (xTaskGetTickCount() - last >= interval) {
			last = xTaskGetTickCount();
			
			if (scrolling_up) {
				menu_index = (menu_index + 1) % menu_size;
			    const char *next_bottom = menu_options[(menu_index + 1) % menu_size];
				lcd_scroll_anim(user_top_option, user_center_option_button_label, user_bottom_option, next_bottom, scrolling_up, 400);
			}
			else {
				menu_index = (menu_index + menu_size - 1) % menu_size;
			    const char *next_top = menu_options[(menu_index + menu_size - 1) % menu_size];
				lcd_scroll_anim(user_top_option, user_center_option_button_label, user_bottom_option, next_top, scrolling_up, 400);
			}

			//scrolling_up = !scrolling_up;
		}
		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void lcd_task_create(void)
{
	xTaskCreatePinnedToCore(lcd_task, "lcd_task", 4096 * 2, NULL,
							tskIDLE_PRIORITY + 1, NULL, 0);
}