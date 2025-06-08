#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lcd_task.h"
#include "lcd_funcs.h"

#include "libs/gif/lv_gif.h"
#include "nvs.h"
#include "nvs_flash.h" // nvs_flash_erase();

static const char *TAG = "LCD_TASK";

ui_menu_t ui_menu = {
    .options = (const char *[]) {"Bluetooth","PolyPlug","ESP32","Infrared","Tools", "Settings","Wi-Fi"},
    .size = 7,
    .index = 1, // “LoRa” starts in the middle
    .page = HOME_PAGE,
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
    .select_btn = 0,
};


lv_color_t user_primary_color = LV_COLOR_MAKE(0x00, 0x00, 0x8B); 
lv_color_t user_secondary_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);

static void lcd_task(void *pvParameters)
{
	user_primary_color = lv_color_hex(0x00008B);
	
	// No scrollbar
	lv_obj_set_scrollbar_mode(ACTIVE_SCR, LV_SCROLLBAR_MODE_OFF);
	
	// Set background
    lv_obj_set_style_bg_color(ACTIVE_SCR, user_primary_color, 0);
    lv_obj_set_style_bg_opa(ACTIVE_SCR, LV_OPA_COVER, 0); // Ensure the background is fully opaque
					 
	// Create images
	lcd_init_images();
    
    TickType_t timer_last = xTaskGetTickCount();
	const TickType_t timer_interval = pdMS_TO_TICKS(200);
	
	//nvs_flash_erase(); // Factory reset

	//lcd_ns_nvs_clear(ESPNOW_RX_MAC_NS);
	//lcd_ns_nvs_clear(ESPNOW_MENU_NS);
	
	
	// Create common items
	lcd_init_selection_labels(&ui_menu);
	
	// Load user data from NVS
	lcd_ir_ir_menu_nvs_load(&ir_menu, A_IR_REMOTE_NS, A_REMOTE_KEY_COUNT, A_REMOTE_KEY_FMT);
	
	lcd_lora_menu_nvs_load(&lora_menu, LORA_OPTIONS_NS, LORA_OPTIONS_KEY_COUNT, LORA_OPTIONS_KEY_FMT);
	lcd_lora_key_nvs_load(&lora_menu, LORA_ENC_NS, LORA_ENC_KEY_COUNT, LORA_ENC_KEY_FMT);
	
	lcd_espnow_menu_nvs_load(&espnow_menu);
	lcd_espnow_rx_mac_nvs_load(&espnow_menu);
		
	// Create common pages
	lcd_ir_setup_page(&ir_menu);
	
	lcd_lora_setup_page(&lora_menu);
	lcd_lora_setup_subpage(&lora_menu);
	
	lcd_espnow_setup_page(&espnow_menu);
	lcd_espnow_setup_send_page(&espnow_menu);
		
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
			if (xSemaphoreTake(xSelectButtonSemaphore, 0)) {
				ui_btns.select_btn = 1;
			}
			else {
				ui_btns.select_btn = 0;
			}

			
			if (ui_menu.page == HOME_PAGE) {
				// Show cool two frame animation and allow user to change animation scrolling up/down				
				lcd_home_page_selected(&ui_menu, &ui_btns);
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
			// ESP-NOW page
			else if (ui_menu.page == ESPNOW_PAGE) {
				lcd_espnow_page_selected(&ui_menu, &espnow_menu, &ui_btns);
			}
			else if (ui_menu.page == ESPNOW_RX_MAC_PAGE) {
				lcd_espnow_get_rx_mac(&ui_menu, &espnow_menu, &ui_btns);
			}
			else if (ui_menu.page == ESPNOW_NAME_PAGE) {
				lcd_espnow_create_custom_name(&ui_menu, &espnow_menu, &ui_btns);
			}
			else if (ui_menu.page == ESPNOW_OPTION_PAGE) {
				lcd_espnow_option_selected(&ui_menu, &espnow_menu, &ui_btns);
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
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void lcd_task_create(void)
{
	xTaskCreatePinnedToCore(lcd_task, "lcd_task", 4096 * 2, NULL,
							tskIDLE_PRIORITY + 1, NULL, 0);
}