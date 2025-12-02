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

#define USING_GROK 1 // Else using ChatGPT

QueueHandle_t xAiCmdQueue;

EXT_RAM_BSS_ATTR static ai_cmd_t msg;
EXT_RAM_BSS_ATTR static char ai_response[AI_RESPONSE_MAX_LEN] = {0}; // TODO: Increase MAX_LEN here and for BT

static void ai_task(void *pvParameters)
{
	// Holds actual command text
	xAiCmdQueue = xQueueCreate(1, sizeof(ai_cmd_t));
	configASSERT(xAiCmdQueue);

	esp_err_t err;

	#ifdef USING_GROK
	err = xai_save_api_key_nvs("xai-7hrSSKPvjhZ19QK5IW3Ab7byKQYfBlCVHAlAGsR8FIQkE0BaE7eaeG98O27xqMf0XW4Vrcz71Qdp5dzp");
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to save xAI API key to NVS: %s", esp_err_to_name(err));
	}
	#else
	err = openai_save_api_key_nvs("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");
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to save OpenAI API key to NVS: %s", esp_err_to_name(err));
	}
	#endif

	// Clear message buffer
	memset(&msg, 0, sizeof(msg));

	while (1) {
		// Block until AI task activated
		if (xQueueReceive(xAiCmdQueue, &msg, portMAX_DELAY) != pdTRUE) {
			continue;
		}

		// Clear ai_response buffer
		memset(ai_response, 0, sizeof(ai_response));

		// Send cmd to the AI
		// 'ai_response' output
		#ifdef USING_GROK
		err = xai_send_command(msg.cmd, ai_response, sizeof(ai_response));
		#else
		err = openai_send_command(msg.cmd, ai_response, sizeof(ai_response));
		#endif

		if (err == ESP_OK) {
			#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "AI script: %s", ai_response);
			#endif

			// Send pointer to script to execute as BLE keyboard sequence
			char *ai_script_ptr = ai_response;
			xQueueSend(xBluetoothAiCmdQueue, &ai_script_ptr, portMAX_DELAY);
		}
		else {
			ESP_LOGE(TAG, "AI request failed: %s", esp_err_to_name(err));
		}

		// Reset message buffer
		memset(&msg, 0, sizeof(msg));

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void ai_task_create(void)
{
	if (xTaskCreate(ai_task, "ai_task", 1024 * 6, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
		ESP_LOGE(TAG, "Failed to start ai_task");
	}
}