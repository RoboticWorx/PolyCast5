#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#define TAG "WIFI_TASK"

//QueueHandle_t xWifiMqttCmdQueue;

//SemaphoreHandle_t xWifiStartScanSemaphore;

static void tools_task(void *param)
{
	//xWifiMqttSuccessSemaphore = xSemaphoreCreateBinary();
	//configASSERT(xWifiMqttSuccessSemaphore);
	
	//xWifiScanQueue = xQueueCreate(WIFI_MAX_NETWORKS, sizeof(wifi_scan_t));
	//configASSERT(xWifiScanQueue);
	
	while (1) {
    
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void tools_task_create(void)
{
    if (xTaskCreate(tools_task, "tools_task", 1024 * 1, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start tools_task");
	}
}
