#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "lcd_task.h"
#include "gpio_task.h"
#include "lcd_bluetooth_funcs.h"
#include "lcd_hotkey_funcs.h"
#include "lcd_utils.h"
#include "widgets/label/lv_label.h"

extern int8_t lcd_ledc_brightness;

static const char *TAG = "LCD_TASK";

static volatile bool is_charging = false;

static const TickType_t btn_timer_interval = pdMS_TO_TICKS(200);

uint8_t sleep_time_s = 30; // Default 30s

ui_menu_t ui_menu = {
	.options = (const char *[]) {"Bluetooth", "PolyPlug", "ESP32", "Infrared", "Tools",  "Settings", "Wi-Fi"},
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
	.select_btn = 0,
	.home_btn = 0,
	.pwr_btn = 0,
};

volatile bool lcd_clear_pending_inputs = false;
volatile bool go_to_sleep = false;

lv_color_t user_primary_color = LV_COLOR_MAKE(0x00, 0x00, 0x8B); 
lv_color_t user_secondary_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);

static void lcd_task(void *pvParameters)
{
	lcd_settings_color_nvs_load(); // Load user colors from NVS
	
	// No scrollbar
	lv_obj_set_scrollbar_mode(ACTIVE_SCR, LV_SCROLLBAR_MODE_OFF);
	
	// Set background
	lv_obj_set_style_bg_color(ACTIVE_SCR, user_primary_color, 0);
	lv_obj_set_style_bg_opa(ACTIVE_SCR, LV_OPA_COVER, 0); // Ensure the background is fully opaque
					 
	// Create images
	lcd_init_images();
	
	TickType_t btn_timer_last = xTaskGetTickCount();
	TickType_t sleep_timer_last = xTaskGetTickCount();
	
	uint8_t battery_percentage;
	
	//nvs_flash_erase(); // Factory reset

	//lcd_ns_nvs_clear(ESPNOW_RX_MAC_NS);
	//lcd_ns_nvs_clear(ESPNOW_MENU_NS);
	//lcd_ns_nvs_clear(ESPNOW_LMK_NS);
	
	#ifdef POLYCAST5_IR_NVS_CLEAR
		lcd_ns_nvs_clear(A_IR_REMOTE_NS);
	#endif
	
	#ifdef POLYCAST5_WIFI_NVS_CLEAR
		lcd_ns_nvs_clear(WIFI_MENU_NS);
		lcd_ns_nvs_clear(WIFI_TOPIC_NS);
	#endif
	
	
	// Create common items
	lcd_init_selection_labels(&ui_menu);


	// Set brightness at boot
	xSemaphoreTake(xLEDCMutex, portMAX_DELAY); // Lock LEDC
	lcd_settings_lcd_ledc_nvs_load();
    uint32_t duty = (lcd_ledc_brightness * ((1 << LCD_LEDC_RESOLUTION) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CHANNEL);
    xSemaphoreGive(xLEDCMutex); // Release LEDC
    
    
	/* Load all user settings */
	lcd_hotkey_nvs_load(&hotkey_cmd);
	
	lcd_lora_menu_nvs_load(&lora_menu);
	lcd_lora_key_nvs_load(&lora_menu);
	
	lcd_espnow_menu_nvs_load(&espnow_menu);
	lcd_espnow_lmk_nvs_load(&espnow_menu);
	lcd_espnow_rx_mac_nvs_load(&espnow_menu);
	
	lcd_wifi_menu_nvs_load(&wifi_menu);
	lcd_wifi_topic_keys_nvs_load(&wifi_menu);
	
	lcd_settings_pin_nvs_load(&settings_menu);
	lcd_settings_pin_attempts_nvs_load(); // Loads pin_attempts global
	lcd_settings_sleep_timer_nvs_load();
	xSemaphoreTake(xHapticsMutex, portMAX_DELAY); // Lock haptics
	lcd_settings_haptics_nvs_load(); // Load haptics
	xSemaphoreGive(xHapticsMutex); // Release haptics
	xSemaphoreTake(xRgbLedMutex, portMAX_DELAY); // Lock RGB LED
	lcd_settings_rgb_led_nvs_load();
	xSemaphoreGive(xRgbLedMutex); // Release RGB LED
	
	/* Create common pages */
	lcd_hotkey_setup_page(&hotkey_menu);
	
	lcd_ir_setup_page(&ir_menu);
	
	lcd_lora_setup_page(&lora_menu);
	lcd_lora_setup_subpage(&lora_menu);
	lcd_lora_setup_plan_page(&ui_menu, &lora_plan_menu);
	
	lcd_espnow_setup_page(&espnow_menu);
	lcd_espnow_setup_send_page(&espnow_menu);
	
	lcd_wifi_setup_page(&wifi_menu);
	lcd_wifi_setup_send_page(&wifi_menu);
	lcd_wifi_create_scan_list(&wifi_menu.scan_menu);
	
	lcd_tools_setup_page(&tools_menu);
	
	lcd_settings_setup_page(&settings_menu);
	lcd_settings_setup_pin_page(&settings_menu);
	
	lcd_bluetooth_setup_page(&bluetooth_menu);
	
	#ifdef POLYCAST5_ESPNOW_DUMP_NVS
		lcd_espnow_dump_nvs();
	#endif
	
	#ifdef POLYCAST5_WIFI_DUMP_NVS
		lcd_wifi_dump_menu_nvs();
		lcd_wifi_dump_wifi_topic_nvs();
	#endif
	
	while (1)
	{
		if (xTaskGetTickCount() - btn_timer_last >= btn_timer_interval) {
			btn_timer_last = xTaskGetTickCount();
			
			if (xUpButtonSemaphore && xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
				ui_btns.up_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount(); // Reset sleep timer
			}
			else {
				ui_btns.up_btn = 0;
			}
			if (xDownButtonSemaphore && xSemaphoreTake(xDownButtonSemaphore, 0) == pdTRUE) {
				ui_btns.down_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.down_btn = 0;
			}
			if (xRightButtonSemaphore && xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {
				ui_btns.right_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.right_btn = 0;
			}
			if (xLeftButtonSemaphore && xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {
				ui_btns.left_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.left_btn = 0;
			}
			if (xSelectButtonSemaphore && xSemaphoreTake(xSelectButtonSemaphore, 0) == pdTRUE) {
				ui_btns.select_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.select_btn = 0;
			}
			if (xHomeButtonSemaphore && xSemaphoreTake(xHomeButtonSemaphore, 0) == pdTRUE) {
				ui_btns.home_btn = 1;
				
				sleep_timer_last = xTaskGetTickCount();
			}
			else {
				ui_btns.home_btn = 0;
			}
			if (xPowerButtonSemaphore && xSemaphoreTake(xPowerButtonSemaphore, 0) == pdTRUE) {
				ui_btns.pwr_btn = 1;
				
				if (!is_charging) {
					lv_label_set_text(ui_menu.lbl_battery_txt, "..."); // Start with dots until battery updated
				}
				else {
					lv_label_set_text(ui_menu.lbl_battery_txt, ""); // Or blank if charging
				}
				lv_timer_handler();
				
				go_to_sleep = true;
			}
			else {
				ui_btns.pwr_btn = 0;
				
				go_to_sleep = false;
			}
			
			// If in loop screen and extra buttons were pressed -> clear them
			if (lcd_clear_pending_inputs) {
				lcd_clear_user_in(); // Clear any pending inputs
				lcd_clear_pending_inputs = false;
			}
			
			if (ui_menu.page == HOME_PAGE) {
				// Show cool two frame animation and allow user to change animation scrolling up/down				
				lcd_home_page(&ui_btns, &ui_menu, &settings_menu);
			} 
			else if (ui_menu.page == UNLOCK_PAGE) {
				lcd_unlock_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == HOTKEY_PAGE) {
				lcd_hotkey_page(&ui_btns, &ui_menu, &hotkey_menu);
			}
			else if (ui_menu.page == HOTKEY_OPTION_PAGE) {
				lcd_hotkey_option_page(&ui_btns, &ui_menu, &hotkey_menu);
			}
			else if (ui_menu.page == SELECTION_PAGE) {
				lcd_selection_page(&ui_btns, &ui_menu, &ir_menu, &lora_menu, &espnow_menu, &wifi_menu, &tools_menu, &settings_menu, &bluetooth_menu);
			}
			// IR remotes page
			else if (ui_menu.page == INFRARED_PAGE) {
				lcd_infrared_page(&ui_btns, &ui_menu, &ir_menu);
			}
			else if (ui_menu.page == INFRARED_REMOTE_NAME_PAGE) {
				lcd_ir_create_custom_name(&ui_btns, &ui_menu, &ir_menu);
			}
			else if (ui_menu.page == INFRARED_REMOTE_EDIT_PAGE) {
				lcd_ir_edit_remotes(&ui_btns, &ui_menu, &ir_menu);
			}
			// LoRa page (PolyPlugs)
			else if (ui_menu.page == LORA_PAGE) {
				lcd_lora_page(&ui_btns, &ui_menu, &lora_menu);
			}
			else if (ui_menu.page == LORA_ADD_PAGE) {
				lcd_lora_add_page(&ui_btns, &ui_menu, &lora_menu);
			}
			else if (ui_menu.page == LORA_NAME_PAGE) {
				lcd_lora_create_custom_name(&ui_btns, &ui_menu, &lora_menu);
			}
			else if (ui_menu.page == LORA_SUBPAGE) {
				lcd_lora_subpage(&ui_btns, &ui_menu, &lora_menu, &lora_plan_menu);
			}
			else if (ui_menu.page == LORA_LOOP_SUBPAGE) {
				lcd_lora_loop_subpage(&ui_btns, &ui_menu, &lora_menu);
			}
			else if (ui_menu.page == LORA_AWAY_SUBPAGE) {
				lcd_lora_away_subpage(&ui_btns, &ui_menu, &lora_menu);
			}
			else if (ui_menu.page == LORA_PLAN_SUBPAGE) {
				lcd_lora_plan_subpage(&ui_btns, &ui_menu, &lora_menu, &lora_plan_menu);
			}
			else if (ui_menu.page == LORA_PLAN_CONFIRM_SUBPAGE) {
				lcd_lora_plan_confirm_subpage(&ui_btns, &ui_menu, &lora_plan_menu);
			}
			else if (ui_menu.page == LORA_PLAN_TIMES_SUBPAGE) {
				lcd_lora_plan_times_subpage(&ui_btns, &ui_menu, &lora_menu, &lora_plan_menu);
			}
			// ESP-NOW page
			else if (ui_menu.page == ESPNOW_PAGE) {
				lcd_espnow_page(&ui_btns, &ui_menu, &espnow_menu);
			}
			else if (ui_menu.page == ESPNOW_RX_MAC_PAGE) {
				lcd_espnow_get_rx_mac(&ui_btns, &ui_menu, &espnow_menu);
			}
			else if (ui_menu.page == ESPNOW_NAME_PAGE) {
				lcd_espnow_create_custom_name(&ui_btns, &ui_menu, &espnow_menu);
			}
			else if (ui_menu.page == ESPNOW_OPTION_PAGE) {
				lcd_espnow_option(&ui_btns, &ui_menu, &espnow_menu);
			}
			// Wi-Fi page
			else if (ui_menu.page == WIFI_PAGE) {
				lcd_wifi_page(&ui_btns, &ui_menu, &wifi_menu);
			}
			else if (ui_menu.page == WIFI_SCAN_PAGE) {
				lcd_wifi_scan_page(&ui_btns, &ui_menu, &wifi_menu);
			}
			else if (ui_menu.page == WIFI_PASSWORD_PAGE) {
				lcd_wifi_get_password(&ui_btns, &ui_menu, &wifi_menu);
			}
			else if (ui_menu.page == WIFI_BEACON_PAGE) {
				lcd_wifi_beacon_page(&ui_btns, &ui_menu, &wifi_menu);
			}
			else if (ui_menu.page == WIFI_DATA_PAGE) {
				lcd_wifi_data_page(&ui_btns, &ui_menu, &wifi_menu);
			}
			else if (ui_menu.page == WIFI_SYNC_PAGE) {
				lcd_wifi_sync_page(&ui_btns, &ui_menu, &wifi_menu);
			}
			else if (ui_menu.page == WIFI_SEND_PAGE) {
				lcd_wifi_send_page(&ui_btns, &ui_menu, &wifi_menu);
			}
			else if (ui_menu.page == WIFI_NAME_PAGE) {
				lcd_wifi_create_custom_name(&ui_btns, &ui_menu, &wifi_menu);
			}
			// Tools page
			else if (ui_menu.page == TOOLS_PAGE) {
				lcd_tools_page(&ui_btns, &ui_menu, &tools_menu);
			}
			else if (ui_menu.page == TOOLS_COIN_PAGE) {
				lcd_tools_coin_page(&ui_btns, &ui_menu, &tools_menu);
			}
			else if (ui_menu.page == TOOLS_DOCS_PAGE) {
				lcd_tools_docs_page(&ui_btns, &ui_menu, &tools_menu);
			}
			else if (ui_menu.page == TOOLS_DICE_PAGE) {
				lcd_tools_dice_page(&ui_btns, &ui_menu, &tools_menu);
			}
			// Settings page
			else if (ui_menu.page == SETTINGS_PAGE) {
				lcd_settings_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_COLORS_PAGE) {
				lcd_settings_colors_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_PIN_PAGE) {
				lcd_settings_pin_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_COLORS_SEL_PAGE) {
				lcd_settings_colors_sel_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_HAPTIC_PAGE) {
				lcd_settings_adjust_haptics_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_SLEEP_TIMER_PAGE) {
				lcd_settings_sleep_timer_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_RGB_LED_PAGE) {
				lcd_settings_adjust_rgb_led_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_LCD_PAGE) {
				lcd_settings_adjust_lcd_page(&ui_btns, &ui_menu, &settings_menu);
			}
			else if (ui_menu.page == SETTINGS_FACTORY_RST_PAGE) {
				lcd_settings_factory_rst_page(&ui_btns, &ui_menu, &settings_menu);
			}
			// Bluetooth page
			else if (ui_menu.page == BLUETOOTH_PAGE) {
				lcd_bluetooth_page(&ui_btns, &ui_menu, &bluetooth_menu);
			}
			else if (ui_menu.page == BLUETOOTH_PAIR_PAGE) {
				lcd_bluetooth_pair_page(&ui_btns, &ui_menu, &bluetooth_menu);
			}
			else if (ui_menu.page == BLUETOOTH_MEDIA_PAGE) {
				lcd_bluetooth_media_page(&ui_btns, &ui_menu, &bluetooth_menu);
			}
			else if (ui_menu.page == BLUETOOTH_KEYBOARD_PAGE) {
				lcd_bluetooth_keyboard_page(&ui_btns, &ui_menu, &bluetooth_menu);
			}
			else if (ui_menu.page == BLUETOOTH_SCRIPT_ADD_PAGE) {
				lcd_bluetooth_add_script_page(&ui_btns, &ui_menu, &bluetooth_menu);
			}
		}
		
		// Sleep condition
		#ifdef POLYCAST5_DIS_SLEEP_TIMER
			if ((ui_menu.page == HOME_PAGE) && go_to_sleep) {
				lcd_device_sleep();
			}
		#else
			TickType_t sleep_timer_interval = pdMS_TO_TICKS(sleep_time_s * 1000);
			
			// If home and sleep_timer_interval has passed without intervention
			if ((ui_menu.page == HOME_PAGE) && ((xTaskGetTickCount() - sleep_timer_last >= sleep_timer_interval) || go_to_sleep)) {
				lcd_device_sleep();
				
				sleep_timer_last = xTaskGetTickCount();
			}
		#endif
		
		// Update battery text
		if (xIsChargingSemaphore && xSemaphoreTake(xIsChargingSemaphore, 0) == pdTRUE) {
			is_charging = true;
		}
		else if (xNotChargingSemaphore && xSemaphoreTake(xNotChargingSemaphore, 0) == pdTRUE) {
			is_charging = false;
		}
		if (xAdcBatReadingQueue && xQueueReceive(xAdcBatReadingQueue, &battery_percentage, 0) == pdTRUE) {
			lcd_update_battery(&ui_menu, battery_percentage, is_charging);
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