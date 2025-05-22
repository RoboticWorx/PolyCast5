#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "espnow_funcs.h"
#include "espnow_task.h"

#define TAG "ESPNOW_TASK"

//static const uint8_t UNIVERSAL_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/*
	SPI RAM Config:
	- Try to allocate Wi-Fi firstly
	- Allow .bss
	- Allow .noinit
*/

static void espnow_task(void *param)
{
    
    
	while (1) {
		/*
		// Start radio and initialize ESP-NOW
		ESP_ERROR_CHECK(esp_funcs_wifi_radio_start(WIFI_CHANNEL));
	    ESP_ERROR_CHECK(esp_funcs_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL));
	    
	    // Send the data
	    char* msg = "Hello from polycast!";
	    esp_funcs_espnow_send_broadcast(UNIVERSAL_MAC, (uint8_t *)(msg), strlen(msg));
		
		// Stop radio and de-initialize ESP-NOW
	    ESP_ERROR_CHECK(esp_funcs_espnow_deinit());
	    ESP_ERROR_CHECK(esp_funcs_wifi_radio_stop());
	    */
    
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}

void espnow_task_create(void)
{
    if (xTaskCreate(espnow_task, "espnow_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start espnow_task");
	}
}
