#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bluetooth_funcs.h"

#define TAG "BLUETOOTH_TASK"

volatile bool bluetooth_connected = false;

static void bluetooth_task(void *arg)
{
	bluetooth_init();
	
	static bool send_volum_up = false;
    while (1) {
		if (bluetooth_connected) {
	        ESP_LOGI(TAG, "Send the volume");
	        if (send_volum_up) {
	            bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_UP, true);
	            vTaskDelay(pdMS_TO_TICKS(100)); // Simulate press
	            bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_UP, false);
	        }
	        else {
	            bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_DOWN, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_DOWN, false);
	        }
	        
	        send_volum_up = !send_volum_up;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void bluetooth_task_create(void)
{
	if (xTaskCreate(bluetooth_task, "bluetooth_task", 1024 * 4, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start bluetooth_task");
	}
}