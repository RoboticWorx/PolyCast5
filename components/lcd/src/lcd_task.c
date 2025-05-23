#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lcd_task.h"
#include "lcd_funcs.h"

#include "nvs.h"
#include "nvs_flash.h" // nvs_flash_erase();

//#include "espressif_logo.h"


static const char *TAG = "LCD_TASK";

ui_menu_t ui_menu = {
    .options = (const char *[]) {"Bluetooth","LoRa","ESP-NOW","Infrared","Tools", "Settings","Wi-Fi"},
    .size = 7,
    .index = 1, // “LoRa” starts in the middle
    .page = SELECTION_PAGE,
    .lbl_top = NULL,
    .lbl_mid = NULL,
    .lbl_bot = NULL,
    .arrow_bot = NULL,
    .arrow_top = NULL,
    .arrow_right = NULL,
    .arrow_left = NULL,
};

ui_btns_t ui_btns = {
    .up_btn = 0,
    .down_btn = 0,
    .right_btn = 0,
    .left_btn = 0,
    .back_btn = 0,
    .home_btn = 0,
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
	
	// No scrollbar
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
					 
	ui_menu.arrow_left = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu.arrow_left, LV_SYMBOL_LEFT, user_secondary_color,
					 &lv_font_montserrat_14, LV_ALIGN_LEFT_MID, 4, 0);

	ui_menu.arrow_right = lv_label_create(ACTIVE_SCR);
	lcd_format_label(ui_menu.arrow_right, LV_SYMBOL_RIGHT, user_secondary_color,
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
	
	//nvs_flash_erase(); // Factory reset

	//lcd_menu_nvs_clear(LORA_OPTIONS_NS);
	//lcd_menu_nvs_clear(LORA_ENC_NS);
	
	lcd_ir_ir_menu_nvs_load(&ir_menu, A_IR_REMOTE_NS, A_REMOTE_KEY_COUNT, A_REMOTE_KEY_FMT);
	
	lcd_lora_menu_nvs_load(&lora_menu, LORA_OPTIONS_NS, LORA_OPTIONS_KEY_COUNT, LORA_OPTIONS_KEY_FMT);
	lcd_lora_key_nvs_load(&lora_menu, LORA_ENC_NS, LORA_ENC_KEY_COUNT, LORA_ENC_KEY_FMT);
		
	lcd_ir_setup_page(&ir_menu);
	
	lcd_lora_setup_page(&lora_menu);
	lcd_lora_setup_subpage(&lora_menu);
	
		for (int i = 0; i < lora_menu.size; i++) {
        uint8_t *key = lora_menu.keys[i];
        if (key) {
            ESP_LOGI(TAG, "Key[%d]:", i);
            ESP_LOG_BUFFER_HEX(TAG, key, 16);
        } else {
            ESP_LOGI(TAG, "Key[%d]: <NULL>", i);
        }
    }
	
	while (1)
	{
		if (xTaskGetTickCount() - timer_last >= timer_interval) {
			timer_last = xTaskGetTickCount();
			
			if (xSemaphoreTake(xUpButtonSemaphore, 0)) {
				ui_btns.up_btn = 1;
			}
			else {
				ui_btns.up_btn = 0;
			}
			if (xSemaphoreTake(xDownButtonSemaphore, 0)) {
				ui_btns.down_btn = 1;
			}
			else {
				ui_btns.down_btn = 0;
			}
			if (xSemaphoreTake(xRightButtonSemaphore, 0)) {
				ui_btns.right_btn = 1;
			}
			else {
				ui_btns.right_btn = 0;
			}
			if (xSemaphoreTake(xLeftButtonSemaphore, 0)) {
				ui_btns.left_btn = 1;
			}
			else {
				ui_btns.left_btn = 0;
			}
			if (xSemaphoreTake(xBackButtonSemaphore, 0)) {
				ui_btns.back_btn = 1;
			}
			else {
				ui_btns.back_btn = 0;
			}
			if (xSemaphoreTake(xHomeButtonSemaphore, 0)) {
				ui_btns.home_btn = 1;
			}
			else {
				ui_btns.home_btn = 0;
			}

			
			if (ui_menu.page == HOME_PAGE) {
				// Show cool two frame animation and allow user to change animation scrolling up/down
			} 
			else if (ui_menu.page == SELECTION_PAGE) {
				
				lcd_selection_page_selected(&ui_menu, &ui_btns);
			}
			// LoRa page (PolyPlugs)
			else if (ui_menu.page == LORA_PAGE) {
				
				lcd_lora_page_selected(&ui_menu, &lora_menu, &ui_btns);
			}
			else if (ui_menu.page == LORA_NAME_PAGE) {
				
				lcd_lora_create_custom_name(&ui_menu, &lora_menu, &ui_btns);
			}
			else if (ui_menu.page == LORA_SUBPAGE) {
				
				lcd_lora_subpage_selected(&ui_menu, &lora_menu, &ui_btns);
			}
			else if (ui_menu.page == LORA_OPTIONS_SUBPAGE) {
				
				lcd_lora_subpage_option_selected(&ui_menu, &lora_menu, &ui_btns);
			}
			// IR remotes
			else if (ui_menu.page == INFRARED_PAGE) {
					
				lcd_infrared_page_selected(&ui_menu, &ir_menu, &ui_btns);
			}
			else if (ui_menu.page == INFRARED_REMOTE_NAME_PAGE) {
				
				lcd_ir_create_custom_name(&ui_menu, &ir_menu, &ui_btns);
			}
			else if (ui_menu.page == INFRARED_REMOTE_EDIT_PAGE) {
				
				lcd_ir_edit_remotes(&ui_menu, &ir_menu, &ui_btns);
			}

		}

		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

void lcd_task_create(void)
{
	xTaskCreatePinnedToCore(lcd_task, "lcd_task", 4096 * 2, NULL,
							tskIDLE_PRIORITY + 1, NULL, 0);
}