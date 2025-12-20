#include "polycast5_macros.h"

#include <string.h>
#include <strings.h>
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

#define AI_PROMPT_NVS_MAX_LEN 4096

QueueHandle_t xAiCmdQueue;

char ai_wifi_portal_pass[64];

EXT_RAM_BSS_ATTR static ai_cmd_t msg;
EXT_RAM_BSS_ATTR static char prompt_buf[AI_PROMPT_NVS_MAX_LEN] = {0};
EXT_RAM_BSS_ATTR static char ai_response[AI_RESPONSE_MAX_LEN] = {0}; // TODO: Increase MAX_LEN here and for BT

static ai_cmd_type_t parse_kind_and_query(const char *in, const char **query_out)
{
    // Trim leading spaces
    while (*in == ' ') in++;

    // Case-insensitive prefix match

	// If password query
    if ((!strncasecmp(in, "password", 8) && (in[8] == ' ' || in[8] == '\t')) ||
		(!strncasecmp(in, "pass", 4) && (in[4] == ' ' || in[4] == '\t'))) {

		*query_out = in + (!strncasecmp(in, "pass", 4) ? 4 : 8); // Move past prefix
        while (**query_out == ' ' || **query_out == '\t') (*query_out)++; // Trim any spaces

        return CMD_CRED_PASSWORD;
    }

	// If username query
    if ((!strncasecmp(in, "username", 8) && (in[8] == ' ' || in[8] == '\t')) ||
		(!strncasecmp(in, "user", 4) && (in[4] == ' ' || in[4] == '\t'))) {

		*query_out = in + (!strncasecmp(in, "user", 4) ? 4 : 8); // Move past prefix
        while (**query_out == ' ' || **query_out == '\t') (*query_out)++; // Trim any spaces

        return CMD_CRED_USERNAME;
	}

	// Fallback to full command
    *query_out = in;

    return CMD_NORMAL;
}

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

		const char *query = NULL;
		ai_cmd_type_t type = parse_kind_and_query(msg.cmd, &query);

		// Auto keyboard command
		if (type == CMD_NORMAL) {
			// Build autotype prompt (NVS override; fallback to compiled default)
			memset(prompt_buf, 0, sizeof(prompt_buf)); // Zero out previous contents
			const char *prompt = ai_get_autokey_prompt(prompt_buf, sizeof(prompt_buf));

			// Send cmd to the AI
			// 'ai_response' output
			err = xai_send_command(prompt, msg.cmd, ai_response, sizeof(ai_response));
			
			#ifdef USING_CHATGPT // UNTESTED!
			err = openai_send_command(msg.cmd, ai_response, sizeof(ai_response));
			#endif
		}
		// Username or password command
		else {
			err = ai_lookup_creds(type, query, ai_response, sizeof(ai_response));
		}

		if (err == ESP_OK) {
			#ifdef POLYCAST5_DEBUG
			// Never log credential payloads (passwords/usernames)
			if (type == CMD_NORMAL) {
				ESP_LOGI(TAG, "AI script: %s", ai_response);
			}
			else {
				ESP_LOGI(TAG, "Credential script resolved (len=%u)", (unsigned)strlen(ai_response));
			}
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