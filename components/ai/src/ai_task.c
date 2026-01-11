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
#include "ai_key_web_portal.h"
#include "bluetooth_task.h"

#include "ai_prompts.h"
#include "ai_utils.h"

#define TAG "AI_TASK"

#define AI_PROMPT_NVS_MAX_LEN 4096

QueueHandle_t xAiCmdQueue;

char ai_wifi_portal_pass[64];

POLYCAST5_USE_PSRAM static char prompt_buf[AI_PROMPT_NVS_MAX_LEN] = {0};
POLYCAST5_USE_PSRAM static char ai_response[AI_RESPONSE_MAX_LEN] = {0}; // TODO: Increase MAX_LEN here and for BT

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

        return AI_CMD_CRED_PASSWORD;
    }

    // If username query
    if ((!strncasecmp(in, "username", 8) && (in[8] == ' ' || in[8] == '\t')) ||
        (!strncasecmp(in, "user", 4) && (in[4] == ' ' || in[4] == '\t'))) {

        *query_out = in + (!strncasecmp(in, "user", 4) ? 4 : 8); // Move past prefix
        while (**query_out == ' ' || **query_out == '\t') (*query_out)++; // Trim any spaces

        return AI_CMD_CRED_USERNAME;
    }

    // Fallback to full command
    *query_out = in;

    return AI_CMD_NORMAL;
}

static void ai_task(void *pvParameters)
{
    // Holds actual command text
    xAiCmdQueue = xQueueCreate(1, sizeof(ai_cmd_t));
    configASSERT(xAiCmdQueue);

    esp_err_t err = ESP_OK;

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
    } else {
        #ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGI(TAG, "Using pre-set AI Wi-Fi portal password: '%s'", ai_wifi_portal_pass);
        #endif
    }

    while (1) {
        ai_cmd_t cmd = {0};

        // Block until AI task activated
        if (xQueueReceive(xAiCmdQueue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "AI task received command type=%d, msg_len=%u, reasoning=%d", (int)cmd.type, (unsigned)cmd.msg_len, cmd.reasoning);
        #endif

        if (!cmd.msg) {
            ESP_LOGW(TAG, "AI cmd missing msg pointer");
            if (cmd.free_on_done && cmd.free_ptr) {
                free(cmd.free_ptr);
            }
            continue;    
        }

        // Clear ai_response buffer
        memset(ai_response, 0, sizeof(ai_response));

        const char *query = NULL;

        // If bt query, parse type of query
        if (cmd.type == AI_CMD_NORMAL || cmd.type == AI_CMD_CRED_USERNAME || cmd.type == AI_CMD_CRED_PASSWORD) {
            cmd.type = parse_kind_and_query(cmd.msg, &query);
        }

        // Auto keyboard command
        if (cmd.type == AI_CMD_NORMAL) {
            // Build autotype prompt (NVS override; fallback to compiled default)
            memset(prompt_buf, 0, sizeof(prompt_buf)); // Zero out previous contents
            const char *prompt = ai_utils_get_autokey_prompt(prompt_buf, sizeof(prompt_buf));

            // 'ai_response' output
            err = ai_utils_send_command_xai(prompt, cmd.msg, ai_response, sizeof(ai_response), cmd.reasoning);
        } else if (cmd.type == AI_CMD_CRED_USERNAME || cmd.type == AI_CMD_CRED_PASSWORD) { // Username or password command
            err = ai_utils_lookup_creds(cmd.type, query, ai_response, sizeof(ai_response));
        } else if (cmd.type == AI_CMD_RAW_FRAMES) { // Organizing raw Wi-Fi frames
            #ifdef POLYCAST5_DEBUG
            size_t msg_len = (cmd.msg_len != 0) ? cmd.msg_len : strlen(cmd.msg);
            ESP_LOGI(TAG, "AI_CMD_RAW_FRAMES payload len=%u", (unsigned)msg_len);
            #endif

            // 'ai_response' output
            err = ai_utils_send_command_xai(AI_PROMPT_RAW_FRAMES, cmd.msg, ai_response, sizeof(ai_response), cmd.reasoning);
        }

        if (err == ESP_OK) {
            if (cmd.type == AI_CMD_NORMAL) {
                #ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "AI script: %s", ai_response);
                #endif

                char *ai_script_ptr = ai_response;
                xQueueSend(xBluetoothAiCmdQueue, &ai_script_ptr, portMAX_DELAY);
            } else if (cmd.type == AI_CMD_CRED_USERNAME || cmd.type == AI_CMD_CRED_PASSWORD) {
                #ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Credential script resolved (len=%u)", (unsigned)strlen(ai_response));
                #endif

                char *ai_script_ptr = ai_response;
                xQueueSend(xBluetoothAiCmdQueue, &ai_script_ptr, portMAX_DELAY);
            } else if (cmd.type == AI_CMD_RAW_FRAMES) {
                #ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Raw frames sniff resolved with response. Grok analysis of raw frames: %s", ai_response);
                #endif

                char *ai_script_ptr = ai_response;
                xQueueSend(xWifiAiRawSniffQueue, &ai_script_ptr, portMAX_DELAY);
            }
        } else {
            ESP_LOGE(TAG, "AI request failed: %s", esp_err_to_name(err));
        }

        if (cmd.free_on_done && cmd.free_ptr) {
            free(cmd.free_ptr);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void ai_task_create(void)
{
		if (xTaskCreate(ai_task, "ai_task", 1024 * 6, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start ai_task");
    }
}