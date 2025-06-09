#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_log_buffer.h"
#include "portmacro.h"

#include "lora_task.h"
#include "lcd_espnow_funcs.h"
#include "espnow_funcs.h"
#include "espnow_task.h"

#define TAG "ESPNOW_TASK"

espnow_cmd_t espnow_cmd;

static const uint8_t UNIVERSAL_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint8_t received_enc_key[ENC_KEY_LEN];
static uint8_t received_lmk[LMK_LEN];

SemaphoreHandle_t xEspCmdStatusSemaphore;

QueueHandle_t xEspSendEncKeyQueueNVS;
QueueHandle_t xEspSendEncKeyQueue;
QueueHandle_t xEspSendCmdQueue;

/*
	SPI RAM Config:
	- Try to allocate Wi-Fi firstly
	- Allow .bss
	- Allow .noinit
*/

static void espnow_task(void *param)
{
	xEspCmdStatusSemaphore = xSemaphoreCreateBinary();
	
    xEspSendEncKeyQueueNVS = xQueueCreate(1, ENC_KEY_LEN);
	if (xEspSendEncKeyQueueNVS == NULL) {
		ESP_LOGE(TAG, "Failed to create xEspSendEncKeyQueueNVS");
		vTaskDelete(NULL);
	}
	
	xEspSendEncKeyQueue = xQueueCreate(1, ENC_KEY_LEN);
	if (xEspSendEncKeyQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xEspSendEncKeyQueue");
		vTaskDelete(NULL);
	}
	
	xEspSendCmdQueue = xQueueCreate(1, sizeof(espnow_cmd_t));
	if (xEspSendCmdQueue == NULL) {
		ESP_LOGE(TAG, "Failed to create xEspSendCmdQueue");
		vTaskDelete(NULL);
	}
    
	while (1) {

		// Key generated and requesting send 
		if (xQueueReceive(xEspSendEncKeyQueue, received_enc_key, 0) == pdPASS) {
			// Start radio and initialize ESP-NOW
			ESP_ERROR_CHECK(esp_funcs_wifi_radio_start(WIFI_CHANNEL));
		    ESP_ERROR_CHECK(esp_funcs_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL, false, NULL));
		    
		    // Send the data
		    //char* msg = "Hello from polycast!";
		    esp_funcs_espnow_send_data(UNIVERSAL_MAC, received_enc_key, ENC_KEY_LEN);
			
			// Stop radio and de-initialize ESP-NOW
		    ESP_ERROR_CHECK(esp_funcs_espnow_deinit());
		    ESP_ERROR_CHECK(esp_funcs_wifi_radio_stop());
		    
		    // Send the data to LCD task to save to NVS under given option
		    xQueueSend(xEspSendEncKeyQueueNVS, received_enc_key, portMAX_DELAY);
		}
		
		if (xQueueReceive(xEspSendCmdQueue, &espnow_cmd, 0) == pdPASS) {
			// Start radio and initialize ESP-NOW
			ESP_ERROR_CHECK(esp_funcs_wifi_radio_start(WIFI_CHANNEL));
		    ESP_ERROR_CHECK(esp_funcs_espnow_init(espnow_cmd.mac_selected, WIFI_CHANNEL, espnow_cmd.enc, espnow_cmd.enc ? espnow_cmd.lmk : NULL));
		    
		    #ifdef POLYCAST5_DEBUG
			    ESP_LOGI(TAG, "Sending: %u", espnow_cmd.cmd_to_send);
			    ESP_LOG_BUFFER_HEX("To MAC", espnow_cmd.mac_selected, ESPNOW_MAC_SIZE);
			    if (espnow_cmd.enc) {
			        ESP_LOG_BUFFER_HEX("LMK", espnow_cmd.lmk, LMK_LEN);
			    }
		    #endif
		    
		    // Send the data
		    esp_funcs_espnow_send_data(espnow_cmd.mac_selected, &espnow_cmd.cmd_to_send, sizeof(espnow_cmd.cmd_to_send));
		    
		    // Wait for ACK frame
		    vTaskDelay(pdMS_TO_TICKS(100));
			
			// Stop radio and de-initialize ESP-NOW
		    ESP_ERROR_CHECK(esp_funcs_espnow_deinit());
		    ESP_ERROR_CHECK(esp_funcs_wifi_radio_stop());
		}
    
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void espnow_task_create(void)
{
    if (xTaskCreate(espnow_task, "espnow_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start espnow_task");
	}
}
