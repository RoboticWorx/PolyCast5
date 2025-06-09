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

volatile bool lcd_clear_pending_inputs = false;

lv_color_t user_primary_color = LV_COLOR_MAKE(0x00, 0x00, 0x8B); 
lv_color_t user_secondary_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);

static void dump_names(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ESPNOW_MENU_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "menu-ns open failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t cnt = 0;
    nvs_get_u8(h, ESPNOW_MENU_KEY_COUNT, &cnt);
    ESP_LOGI(TAG, "=== ESP-NOW peer names (%u) ===", cnt);

    for (uint8_t i = 0; i < cnt; i++) {
        char key[16];  snprintf(key, sizeof(key), ESPNOW_MENU_KEY_FMT, i);

        size_t len = 0;
        err = nvs_get_str(h, key, NULL, &len);
        if (err == ESP_OK && len > 1 && len < 64) {
            char *buf = malloc(len);
            if (buf) {
                nvs_get_str(h, key, buf, &len);
                ESP_LOGI(TAG, "  [%u] \"%s\"", i, buf);
                free(buf);
            }
        } else {
            ESP_LOGW(TAG, "  [%u] missing or too long (%s)", i, esp_err_to_name(err));
        }
    }
    nvs_close(h);
}

static void dump_macs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ESPNOW_RX_MAC_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mac-ns open failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t cnt = 0;
    nvs_get_u8(h, ESPNOW_RX_MAC_KEY_COUNT, &cnt);
    ESP_LOGI(TAG, "=== ESP-NOW peer MACs (%u) ===", cnt);

    for (uint8_t i = 0; i < cnt; i++) {
        char key[16];  snprintf(key, sizeof(key), ESPNOW_RX_MAC_KEY_FMT, i);
        uint8_t mac[6]; size_t len = sizeof(mac);

        err = nvs_get_blob(h, key, mac, &len);
        if (err == ESP_OK && len == 6) {
            ESP_LOGI(TAG, "  [%u] %02X:%02X:%02X:%02X:%02X:%02X",
                     i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            ESP_LOGW(TAG, "  [%u] missing / wrong size (%s)", i, esp_err_to_name(err));
        }
    }
    nvs_close(h);
}

static void dump_lmks(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ESPNOW_LMK_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lmk-ns open failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t cnt = 0;
    nvs_get_u8(h, ESPNOW_LMK_KEY_COUNT, &cnt);
    ESP_LOGI(TAG, "=== ESP-NOW LMKs (%u) ===", cnt);

    for (uint8_t i = 0; i < cnt; i++) {
        char key[16];
        snprintf(key, sizeof(key), ESPNOW_LMK_KEY_FMT, i);

        uint8_t lmk[LMK_LEN];
        size_t  len = sizeof(lmk);

        err = nvs_get_blob(h, key, lmk, &len);
        if (err == ESP_OK && len == LMK_LEN) {

            /* build a 32-char hex string in a tiny buffer */
            char hex[LMK_LEN * 2 + 1];
            for (int j = 0; j < LMK_LEN; j++) {
                sprintf(&hex[j * 2], "%02X", lmk[j]);
            }
            hex[LMK_LEN * 2] = '\0';

            ESP_LOGI(TAG, "  [%u] %s", i, hex);
        } else {
            ESP_LOGW(TAG, "  [%u] missing / wrong size (%s)",
                     i, esp_err_to_name(err));
        }
    }
    nvs_close(h);
}



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
	//lcd_ns_nvs_clear(ESPNOW_LMK_NS);
	
	
	// Create common items
	lcd_init_selection_labels(&ui_menu);
	
	// Load user data from NVS
	lcd_ir_ir_menu_nvs_load(&ir_menu, A_IR_REMOTE_NS, A_REMOTE_KEY_COUNT, A_REMOTE_KEY_FMT);
	
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
	
		ESP_LOGI(TAG, "========================================");
    dump_names();
    dump_macs();
    dump_lmks();
    ESP_LOGI(TAG, "========================================");
		
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