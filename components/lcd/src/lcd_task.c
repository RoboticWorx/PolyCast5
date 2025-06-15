#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lcd_task.h"
#include "gpio_task.h"
#include "lcd_funcs.h"

static const char *TAG = "LCD_TASK";

static const TickType_t btn_timer_interval = pdMS_TO_TICKS(200);
static const TickType_t sleep_timer_interval = pdMS_TO_TICKS(POLYCAST5_DEFAULT_SLEEP_TIME_MS);

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

volatile bool lcd_clear_pending_inputs = false;

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
    
    TickType_t btn_timer_last = xTaskGetTickCount();	
	TickType_t sleep_timer_last = xTaskGetTickCount();
	
	
	//nvs_flash_erase(); // Factory reset

	//lcd_ns_nvs_clear(ESPNOW_RX_MAC_NS);
	//lcd_ns_nvs_clear(ESPNOW_MENU_NS);
	//lcd_ns_nvs_clear(ESPNOW_LMK_NS);
	
	#ifdef POLYCAST5_IR_NVS_CLEAR
		lcd_ns_nvs_clear(A_IR_REMOTE_NS);
	#endif
	
	
	// Create common items
	lcd_init_selection_labels(&ui_menu);
	
	
	// Load user data from NVS
	lcd_ir_menu_nvs_load(&ir_menu, A_IR_REMOTE_NS, A_IR_REMOTE_KEY_COUNT, A_IR_REMOTE_KEY_FMT);
	
	lcd_lora_menu_nvs_load(&lora_menu);
	lcd_lora_key_nvs_load(&lora_menu);
	
	lcd_espnow_menu_nvs_load(&espnow_menu);
	lcd_espnow_lmk_nvs_load(&espnow_menu);
	lcd_espnow_rx_mac_nvs_load(&espnow_menu);
	
		
	// Create common pages
	lcd_ir_setup_page(&ir_menu);
	
	lcd_lora_setup_page(&lora_menu);
	lcd_lora_setup_subpage(&lora_menu);
	
	lcd_espnow_setup_page(&espnow_menu);
	lcd_espnow_setup_send_page(&espnow_menu);
	
	lcd_wifi_setup_page(&wifi_menu);
	lcd_wifi_create_scan_list(&wifi_menu.scan_menu);
	
	
	#ifdef POLYCAST5_ESPNOW_DUMP_NVS
		lcd_espnow_dump_nvs();
	#endif
	
	
	while (1)
	{
		if (xTaskGetTickCount() - btn_timer_last >= btn_timer_interval) {
			btn_timer_last = xTaskGetTickCount();
			
			if (xSemaphoreTake(xUpButtonSemaphore, 0)) {
				ui_btns.up_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount(); // Reset sleep timer
			}
			else {
				ui_btns.up_btn = 0;
			}
			if (xSemaphoreTake(xDownButtonSemaphore, 0)) {
				ui_btns.down_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.down_btn = 0;
			}
			if (xSemaphoreTake(xRightButtonSemaphore, 0)) {
				ui_btns.right_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.right_btn = 0;
			}
			if (xSemaphoreTake(xLeftButtonSemaphore, 0)) {
				ui_btns.left_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.left_btn = 0;
			}
			if (xSemaphoreTake(xBackButtonSemaphore, 0)) {
				ui_btns.back_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.back_btn = 0;
			}
			if (xSemaphoreTake(xSelectButtonSemaphore, 0)) {
				ui_btns.select_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.select_btn = 0;
			}
			// If in loop screen and extra buttons were pressed -> clear them
			if (lcd_clear_pending_inputs) {
				lcd_clear_user_in(); // Clear any pending inputs
				lcd_clear_pending_inputs = false;
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
			// IR remotes page
			else if (ui_menu.page == INFRARED_PAGE) {
				lcd_infrared_page_selected(&ui_menu, &ir_menu, &ui_btns);
			}
			else if (ui_menu.page == INFRARED_REMOTE_NAME_PAGE) {
				lcd_ir_create_custom_name(&ui_menu, &ir_menu, &ui_btns);
			}
			else if (ui_menu.page == INFRARED_REMOTE_EDIT_PAGE) {
				lcd_ir_edit_remotes(&ui_menu, &ir_menu, &ui_btns);
			}
			// Wi-Fi page
			else if (ui_menu.page == WIFI_PAGE) {
				lcd_wifi_page_selected(&ui_menu, &wifi_menu, &ui_btns);
			}
			else if (ui_menu.page == WIFI_SCAN_PAGE) {
				lcd_wifi_scan_page(&ui_menu, &wifi_menu, &ui_btns);
			}
			else if (ui_menu.page == WIFI_PASSWORD_PAGE) {
				lcd_wifi_get_password(&ui_menu, &wifi_menu, &ui_btns);
			}
		}
						
		// Sleep condition
		if ((ui_menu.page == HOME_PAGE) && ((xTaskGetTickCount() - sleep_timer_last >= sleep_timer_interval) || (xSemaphoreTake(xPowerButtonSemaphore, 0) == pdTRUE))) {
			lcd_device_sleep();
			
			sleep_timer_last = xTaskGetTickCount();
		}

		lv_timer_handler();
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void lcd_task_create(void)
{
	if (xTaskCreatePinnedToCore(lcd_task, "lcd_task", 1024 * 8, NULL, tskIDLE_PRIORITY + 2, NULL, 0) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start lcd_task");
	}
}