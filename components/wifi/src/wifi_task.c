#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "espnow_task.h"
#include "espnow_funcs.h"

#define TAG "WIFI_TASK"

wifi_scan_t wifi_scan[WIFI_MAX_NETWORKS];

QueueHandle_t xWifiScanQueue;

SemaphoreHandle_t xWifiStartScanSemaphore;

static void wifi_task(void *param)
{
	xWifiStartScanSemaphore = xSemaphoreCreateBinary();
	
	xWifiScanQueue = xQueueCreate(WIFI_MAX_NETWORKS, sizeof(wifi_scan_t));
    
	while (1) {
		if (xSemaphoreTake(xWifiStartScanSemaphore, 0) == pdTRUE) {
			espnow_funcs_wifi_radio_start(WIFI_CHANNEL);
			
			wifi_funcs_wifi_scan(wifi_scan);
			
			espnow_funcs_wifi_radio_stop();
		}
    
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void wifi_task_create(void)
{
    if (xTaskCreate(wifi_task, "wifi_task", 1024 * 3, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start wifi_task");
	}
}
