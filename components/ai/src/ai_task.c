#include "polycast5_macros.h"

#include <string.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/idf_additions.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"

#include "wifi_task.h"
#include "gpio_task.h"
#include "ai_web_portal.h"
#include "bluetooth_task.h"

#include "ai_funcs.h"

#define TAG "AI_TASK"

#define USING_GROK 1 // Else using ChatGPT

QueueHandle_t xAiCmdQueue;

char ai_wifi_portal_pass[64];

EXT_RAM_BSS_ATTR static ai_cmd_t msg;
EXT_RAM_BSS_ATTR static char ai_response[AI_RESPONSE_MAX_LEN] = {0}; // TODO: Increase MAX_LEN here and for BT

static void ai_task(void *pvParameters)
{
	// Holds actual command text
	xAiCmdQueue = xQueueCreate(1, sizeof(ai_cmd_t));
	configASSERT(xAiCmdQueue);

	esp_err_t err;

	// If Wi-Fi AI portal password NVS doesn't exist yet, set it
	if (ai_wifi_pass_load_nvs(ai_wifi_portal_pass, sizeof(ai_wifi_portal_pass)) != ESP_OK) {
		// Random chars to pick from
		static const char alphabet[] =
				"ABCDEFGHJKLMNPQRSTUVWXYZ"
				"abcdefghijkmnopqrstuvwxyz"
				"0123456789";
		
		const size_t N = sizeof(alphabet) - 1;
		const size_t PASS_LEN = 12;
	
		// Create random password
		for (size_t i = 0; i < PASS_LEN; ++i) {
			uint32_t r = esp_random();
			ai_wifi_portal_pass[i] = alphabet[r % N];
		}
		ai_wifi_portal_pass[PASS_LEN] = '\0';
		
		// Save that version to NVS
		ai_wifi_pass_save_nvs(ai_wifi_portal_pass);
		
		#ifdef POLYCAST5_PASS_DEBUG
		ESP_LOGW(TAG, "Setting first time AI Wi-Fi portal password: %s", ai_wifi_portal_pass);
		#endif
	}
	else {
		#ifdef POLYCAST5_PASS_DEBUG
		ESP_LOGI(TAG, "Using pre-set AI Wi-Fi portal password: '%s'", ai_wifi_portal_pass);
		#endif
	}
	
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