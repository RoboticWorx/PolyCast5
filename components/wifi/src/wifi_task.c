#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "wifi_funcs.h"
#include "wifi_task.h"

#define TAG "WIFI_TASK"

static void wifi_task(void *param)
{
	
    
	while (1) {

    
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void wifi_task_create(void)
{
    if (xTaskCreate(wifi_task, "wifi_task", 1024 * 3, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start wifi_task");
	}
}
