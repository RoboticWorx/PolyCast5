#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bluetooth_funcs.h"
#include "portmacro.h"
#include "bluetooth_task.h"
#include "gpio_task.h"
#include "gpio_funcs.h"

#define TAG "BLUETOOTH_TASK"

QueueHandle_t xBluetoothCmdQueue;

static uint8_t bluetooth_cmd = 0;
volatile bool bluetooth_connected = false;

static void bluetooth_task(void *arg)
{
	xBluetoothCmdQueue = xQueueCreate(1, sizeof(uint8_t));
	configASSERT(xBluetoothCmdQueue);
	
	while (1) {
		// If a bluetooth command is received
		if (xQueueReceive(xBluetoothCmdQueue, &bluetooth_cmd, portMAX_DELAY) == pdTRUE) {
			// Initialize command received
			if (bluetooth_cmd == BLUETOOTH_QUEUE_CMD_INIT) {
				bluetooth_init();
			}
			// De-initialize command received
			else if (bluetooth_cmd == BLUETOOTH_QUEUE_CMD_DEINIT) {
				bluetooth_deinit();
			}
			// Vol-up command received
			else if (bluetooth_cmd == BLUETOOTH_QUEUE_CMD_VOL_UP && bluetooth_connected) {
				bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_UP, true);
	            vTaskDelay(pdMS_TO_TICKS(100)); // Simulate press
	            bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_UP, false);
			}
			// Vol-down command received
			else if (bluetooth_cmd == BLUETOOTH_QUEUE_CMD_VOL_DOWN && bluetooth_connected) {
				bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_DOWN, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_cmd(BLUETOOTH_CMD_VOLUME_DOWN, false);
			}
		}		
		
		//vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void bluetooth_task_create(void)
{
	if (xTaskCreate(bluetooth_task, "bluetooth_task", 1024 * 4, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start bluetooth_task");
	}
}