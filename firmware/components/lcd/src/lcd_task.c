#include "lcd_task.h"
#include "core/lv_obj_pos.h"
#include "font/lv_symbol_def.h"
#include "lcd_funcs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lv_api_map_v8.h"
#include "misc/lv_area.h"
#include "misc/lv_style_gen.h"

//#include "espressif_logo.h"

#include "gpio_funcs.h"
#include "gpio_task.h"


static const char *TAG = "LCD_TASK";

menu_t ui_menu = {
    .options = (const char *[]) {"Bluetooth","LoRa","ESP-NOW","Infrared","Settings","Wi-Fi"},
    .size = 6,
    .index = 1, // “LoRa” starts in the middle
    .page = SELECTION_PAGE,
    .lbl_top = NULL,
    .lbl_mid = NULL,
    .lbl_bot = NULL,
    .arrow_bot = NULL,
    .arrow_top = NULL,
};

ir_menu_t ir_menu = {
    .options = {"Add New", "test1", "test2", "test3"},
    .size = 4,
    .index = 0,
    .cont = NULL,
};

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
	
	// No scroll-bar
	lv_obj_set_scrollbar_mode(ACTIVE_SCR, LV_SCROLLBAR_MODE_OFF);
	
	// Set background
    lv_obj_set_style_bg_color(ACTIVE_SCR, user_primary_color, 0);
    lv_obj_set_style_bg_opa(ACTIVE_SCR, LV_OPA_COVER, 0); // Ensure the background is fully opaque
    
    
    // Create and format center button
    lv_obj_t *ui_btn_mid = lv_btn_create(ACTIVE_SCR);
    lcd_format_center_button(ui_btn_mid, user_primary_color, user_secondary_color);


	// Format labels
	ui_menu.arrow_top = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu.arrow_top, LV_SYMBOL_UP, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 0);

	ui_menu.lbl_top = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu.lbl_top, "Bluetooth", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

	/*lv_obj_t *user_left_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_left_arrow, LV_SYMBOL_LEFT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);*/

	ui_menu.lbl_mid = lv_label_create(ui_btn_mid);
	lcd_format_label(ui_menu.lbl_mid, "LoRa",
					 user_secondary_color, &lv_font_montserrat_30,
					 LV_ALIGN_CENTER, 0, 0);
					 
	lv_obj_t *user_left_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_left_arrow, LV_SYMBOL_LEFT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);

	lv_obj_t *user_right_arrow = lv_label_create(ACTIVE_SCR);
	lcd_format_label(user_right_arrow, LV_SYMBOL_RIGHT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_RIGHT_MID, -4, 0);

	ui_menu.lbl_bot = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu.lbl_bot, "ESP-NOW", user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -15);

	ui_menu.arrow_bot = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu.arrow_bot, LV_SYMBOL_DOWN, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, 0);

	lv_obj_t *battery_icon_text = lv_label_create(ACTIVE_SCR);
	lcd_format_label(battery_icon_text, "100", user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -28, 0);

	lv_obj_t *battery_icon = lv_label_create(ACTIVE_SCR); // 3, 2, 1, EMPTY
	lcd_format_label(battery_icon, LV_SYMBOL_BATTERY_FULL, user_secondary_color,
					 &lv_font_montserrat_18, LV_ALIGN_TOP_RIGHT, -2, -3);
					 
	
	// Create images
	/*   
    lv_obj_t *espressif_logo_obj = lv_img_create(ACTIVE_SCR);
    lv_img_set_src(espressif_logo_obj, &espressif_logo);
    lv_obj_align(espressif_logo_obj, LV_ALIGN_LEFT_MID, 5, 30);
    */
    //lv_obj_add_flag(infrared_logo_obj, LV_OBJ_FLAG_HIDDEN);

	TickType_t timer_last = xTaskGetTickCount();
	const TickType_t timer_interval = pdMS_TO_TICKS(300);
	
	
	lcd_setup_infrared_page(&ir_menu);
    

	while (1)
	{
		if (ui_menu.page == HOME_PAGE) {

		} 
		else if (ui_menu.page == SELECTION_PAGE) {

			if (xTaskGetTickCount() - timer_last >= timer_interval) {
				
				timer_last = xTaskGetTickCount();
				
				/*lv_obj_remove_flag(ui_menu.lbl_top, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(ui_menu.lbl_mid, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(ui_menu.lbl_bot, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(ui_menu.arrow_top, LV_OBJ_FLAG_HIDDEN);
				lv_obj_remove_flag(ui_menu.arrow_bot, LV_OBJ_FLAG_HIDDEN);*/
				
				lcd_page_1_selected(&ui_menu);
			}
		
		}
		else if (ui_menu.page == INFRARED_PAGE) {
			lcd_page_2_selected(&ui_menu, &ir_menu);

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