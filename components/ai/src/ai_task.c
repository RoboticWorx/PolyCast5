#include "polycast5_macros.h"

#include <string.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/idf_additions.h"

#include "esp_log.h"
#include "esp_err.h"

#include "wifi_funcs.h"
#include "gpio_funcs.h"
#include "bluetooth_funcs.h"

#include "wifi_task.h"
#include "gpio_task.h"
#include "bluetooth_task.h"

#include "ai_funcs.h"

#define TAG "AI_TASK"

// TODO: Add mutexes
extern bool wifi_connected;
extern bluetooth_state_t bluetooth_state;
extern bool wifi_ai_req;

QueueHandle_t xAiCmdQueue;

EXT_RAM_BSS_ATTR static ai_cmd_t msg;
EXT_RAM_BSS_ATTR static char script[AI_RESPONSE_MAX_LEN] = {0};

static void ai_task(void *pvParameters)
{
	// Holds actual command text
	xAiCmdQueue = xQueueCreate(1, sizeof(ai_cmd_t));
	configASSERT(xAiCmdQueue);

	esp_err_t err;
	err = openai_save_api_key_nvs("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");
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to save OpenAI API key to NVS: %s", esp_err_to_name(err));
	}

	while (1) {
		// Clear message buffer
		memset(&msg, 0, sizeof(msg));

		// Block until AI task activated
		if (xQueueReceive(xAiCmdQueue, &msg, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		// Signal not to check for OTA
		wifi_ai_req = true;

		// Connect to previous Wi-Fi network
		wifi_login_t prev_network = wifi_funcs_get_prev(); // Loads boot state saved network info
		prev_network.prev = true; // Connecting to previous
		if (xQueueSend(xWifiSelectedNetworkQueue, &prev_network, portMAX_DELAY) != pdPASS) {
			ESP_LOGE(TAG, "Failed: xWifiSelectedNetworkQueue previous_network");
		}

		// Wait for Wi-Fi connected
		xSemaphoreTake(xWifiNetworkConnectedSemaphore, portMAX_DELAY);

		if (!wifi_connected) {
			ESP_LOGW(TAG, "WiFi not connected: dropping AI cmd: %s", msg.cmd);
			continue;
		}

		// Clear script buffer
		memset(script, 0, sizeof(script));

		// Send command to OpenAI
		esp_err_t err = openai_send_command(msg.cmd, script, sizeof(script));

		if (err == ESP_OK) {
			ESP_LOGI(TAG, "AI script: %s", script);

			// Connect to BLE
			uint16_t cmd = BLUETOOTH_CMD_INIT;
			xQueueSend(xBluetoothMediaCmdQueue, &cmd, portMAX_DELAY);

			// Wait for BLE connected
			xSemaphoreTake(xBleConnectedSemaphore, portMAX_DELAY);

			vTaskDelay(pdMS_TO_TICKS(5000));

			if (bluetooth_state != BT_STATE_RUNNING) {
				ESP_LOGW(TAG, "BLE not running: dropping AI cmd: %s", msg.cmd);
				continue;
			}

			char *ai_script_ptr = script;
			xQueueSend(xBluetoothAiCmdQueue, &ai_script_ptr, portMAX_DELAY);
		}
		else {
			ESP_LOGE(TAG, "AI request failed: %s", esp_err_to_name(err));
		}

		wifi_ai_req = false;

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void ai_task_create(void)
{
	if (xTaskCreate(ai_task, "ai_task", 1024 * 6, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start ai_task");
	}
}