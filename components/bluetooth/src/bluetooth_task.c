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

QueueHandle_t xBluetoothMediaCmdQueue;

volatile bool bluetooth_connected = false;

static uint8_t bluetooth_cmd = 0;
static uint8_t battery_percentage = 100;
static const TickType_t battery_timer_interval = pdMS_TO_TICKS(1000);

static void bluetooth_task(void *arg)
{
	xBluetoothMediaCmdQueue = xQueueCreate(1, sizeof(uint8_t));
	configASSERT(xBluetoothMediaCmdQueue);
	
	TickType_t battery_timer_last = xTaskGetTickCount();
	
	while (1) {
		// If a bluetooth command is received
		if (xQueueReceive(xBluetoothMediaCmdQueue, &bluetooth_cmd, 0) == pdTRUE) {
			/* Initialization stuff */
			// Initialize command received
			if (bluetooth_cmd == BLUETOOTH_CMD_INIT) {
				bluetooth_init();
			}
			// De-initialize command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_DEINIT) {
				bluetooth_deinit();
			}
			/* Media commands */
			// Vol-up command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_VOLUME_UP && bluetooth_connected) {
				bluetooth_send_media(BLUETOOTH_CMD_VOLUME_UP, true);
	            vTaskDelay(pdMS_TO_TICKS(100)); // Simulate press
	            bluetooth_send_media(BLUETOOTH_CMD_VOLUME_UP, false);
			}
			// Vol-down command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_VOLUME_DOWN && bluetooth_connected) {
				bluetooth_send_media(BLUETOOTH_CMD_VOLUME_DOWN, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_VOLUME_DOWN, false);
			}
			// Next track command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_NEXT_TRK && bluetooth_connected) {
				bluetooth_send_media(BLUETOOTH_CMD_NEXT_TRK, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_NEXT_TRK, false);
			}
			// Previous track command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_PREV_TRK && bluetooth_connected) {
				bluetooth_send_media(BLUETOOTH_CMD_PREV_TRK, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_PREV_TRK, false);
			}
			// Play pause command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_PLAY_PAUSE && bluetooth_connected) {
				bluetooth_send_media(BLUETOOTH_CMD_PLAY_PAUSE, true);
	            vTaskDelay(pdMS_TO_TICKS(100));
	            bluetooth_send_media(BLUETOOTH_CMD_PLAY_PAUSE, false);
			}
			/* Text scripts */
			// Script one command received
			else if (bluetooth_cmd == BLUETOOTH_CMD_SCRIPT_ONE && bluetooth_connected) {
				#define DAN_TXT "Hey Uncle Dan!\nJustin here on his PolyCast5 or Pentamote, I'm having trouble deciding. Perhaps you could help.\n\nPolyCast5 sounds cool and is super unique but can also feel like a mouth-full. Pentamote sounds pretty cool too and is faster to say but maybe loses some uniqueness.\n\nWhat do you think? I already own the .com for both which evens the playing field.\n\nOr maybe there is a better name I haven't thought of?\n\nThanks for your assistance.\n\n"
				bluetooth_send_string(DAN_TXT, 1);
			}
		}
		
		// Get device battery level
		xQueueReceive(xAdcBatBluetoothQueue, &battery_percentage, 0);
		
		// Update bluetooth battery level every battery_timer_interval
		if (xTaskGetTickCount() - battery_timer_last >= battery_timer_interval) {
			battery_timer_last = xTaskGetTickCount();
			
			bluetooth_set_battery_level(battery_percentage);
		}
		
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void bluetooth_task_create(void)
{
	if (xTaskCreate(bluetooth_task, "bluetooth_task", 1024 * 4, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start bluetooth_task");
	}
}