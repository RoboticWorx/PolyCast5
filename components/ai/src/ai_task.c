#include "polycast5_macros.h"

#include <string.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/idf_additions.h"

#include "esp_log.h"
#include "esp_err.h"

#include "wifi_task.h"
#include "gpio_task.h"
#include "bluetooth_task.h"

#include "ai_funcs.h"

#define TAG "AI_TASK"

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

		// Clear script buffer
		memset(script, 0, sizeof(script));

		// Send command to OpenAI
		esp_err_t err = openai_send_command(msg.cmd, script, sizeof(script));

		if (err == ESP_OK) {
			ESP_LOGI(TAG, "AI script: %s", script);

			char *ai_script_ptr = script;
			xQueueSend(xBluetoothAiCmdQueue, &ai_script_ptr, portMAX_DELAY);
		}
		else {
			ESP_LOGE(TAG, "AI request failed: %s", esp_err_to_name(err));
		}

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void ai_task_create(void)
{
	if (xTaskCreate(ai_task, "ai_task", 1024 * 6, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start ai_task");
	}
}