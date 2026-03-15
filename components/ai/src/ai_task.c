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
#include "ai_key_portal.h"
#include "bluetooth_task.h"

#include "ai_prompts.h"
#include "ai_utils.h"
#include "ai_task.h"
#include "ai_voice.h"

#define TAG "AI_TASK"

QueueHandle_t xAiCmdQueue;

EventGroupHandle_t xAiEventGroup;

SemaphoreHandle_t xAiSoundHeardSemaphore;

char ai_wifi_portal_pass[64];

volatile bool mic_recording = false; // To lcd_bluetooth.c

POLYCAST5_USE_PSRAM static char prompt_buf[AI_PROMPT_NVS_MAX_LEN] = {0};
POLYCAST5_USE_PSRAM static char ai_response[AI_RESPONSE_MAX_LEN] = {0}; // TODO: Increase MAX_LEN here and for BT
POLYCAST5_USE_PSRAM static char user_transcript[1024];

// Streaming callback: queues each content delta for bluetooth_task to type over BLE
static esp_err_t stream_to_bluetooth_cb(const char *delta, void *ctx)
{
    (void)ctx;

    // If BLE was torn down mid-stream, abort the HTTP request
    if (xEventGroupGetBits(xBluetoothEventGroup) & BLUETOOTH_CANCEL_TYPING_BIT) {
        return ESP_FAIL;
    }

    // Signal done thinking
    xEventGroupSetBits(xAiEventGroup, AI_DONE_THINKING_BIT);

    if (delta && delta[0]) {
        char *copy = strdup(delta);
        if (copy) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Streaming AI BLE: '%s'", copy);
#endif
            // Queue the strdup'd chunk; bluetooth_task drains and free()s it
            if (xQueueSend(xBluetoothAiStreamQueue, &copy, pdMS_TO_TICKS(5000)) != pdTRUE) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGW(TAG, "stream_to_bluetooth_cb timeout: '%s'", copy);
#endif
                free(copy); // Queue full/timeout
            }
        }
    }
    return ESP_OK;
}

static ai_cmd_type_t parse_kind_and_query(const char *in, const char **query_out)
{
    // Trim leading spaces
    while (*in == ' ') in++;

    // Case-insensitive prefix match

    // If password query
    if ((!strncasecmp(in, "password", 8) && (in[8] == ' ' || in[8] == '\t')) ||
        (!strncasecmp(in, "passwords", 9) && (in[9] == ' ' || in[9] == '\t'))) {

        *query_out = in + (!strncasecmp(in, "passwords", 9) ? 9 : 8); // Move past prefix
        while (**query_out == ' ' || **query_out == '\t') (*query_out)++; // Trim any spaces

        return AI_CMD_CRED_PASSWORD;
    }

    // If username query
    if ((!strncasecmp(in, "username", 8) && (in[8] == ' ' || in[8] == '\t')) ||
        (!strncasecmp(in, "usernames", 9) && (in[9] == ' ' || in[9] == '\t'))) {

        *query_out = in + (!strncasecmp(in, "usernames", 9) ? 9 : 8); // Move past prefix
        while (**query_out == ' ' || **query_out == '\t') (*query_out)++; // Trim any spaces

        return AI_CMD_CRED_USERNAME;
    }

    // Fallback to full command
    *query_out = in;

    return AI_CMD_KEYBOARD_DONE_REC;
}

static void ai_task(void *pvParameters)
{
    // Holds actual command text
    xAiCmdQueue = xQueueCreate(1, sizeof(ai_cmd_t));
    configASSERT(xAiCmdQueue);

    xAiSoundHeardSemaphore = xSemaphoreCreateBinary();
    configASSERT(xAiSoundHeardSemaphore);

    xAiEventGroup = xEventGroupCreate();
    configASSERT(xAiEventGroup);

    esp_err_t err = ESP_OK;

    // If Wi-Fi AI portal password NVS doesn't exist yet, set it
    if (ai_key_portal_pass_load_nvs(ai_wifi_portal_pass, sizeof(ai_wifi_portal_pass)) != ESP_OK) {
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
        ai_key_portal_pass_save_nvs(ai_wifi_portal_pass);
        
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGW(TAG, "Setting first time AI Wi-Fi portal password: %s", ai_wifi_portal_pass);
#endif
    } else {
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGI(TAG, "Using pre-set AI Wi-Fi portal password: '%s'", ai_wifi_portal_pass);
#endif
    }

    ai_voice_pcm_t pcm = {0};

    err = ai_voice_force_sleep_pins_low();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_task: ai_voice_force_sleep_pins_low failed: %s", esp_err_to_name(err));
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

        // Clear ai_response buffer
        memset(ai_response, 0, sizeof(ai_response));

        const char *query = NULL;

        if (cmd.type == AI_CMD_KEYBOARD_START_REC) {
            if (!mic_recording) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGW(TAG, "START_REC received but mic_recording is false; ignoring");
#endif
                continue;
            }

            // Enable mic
            ESP_ERROR_CHECK(ai_voice_init());

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Starting ai_voice_record_pcm16_16k");
#endif

            ESP_ERROR_CHECK(ai_voice_record_pcm16_16k(&mic_recording, &pcm));
            continue;
        } else if (cmd.type == AI_CMD_KEYBOARD_DONE_REC) { // Process transcription
            memset(user_transcript, 0, sizeof(user_transcript)); // Clear previous contents

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "WS STT uploading PCM: samples=%u", (unsigned)pcm.samples);
#endif

            // Transcribe via xAI WebSocket STT
            esp_err_t err = ai_voice_stt_ws_transcribe_pcm16_xai(pcm.pcm16, pcm.samples, user_transcript, sizeof(user_transcript));

            if (err == ESP_OK) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Realtime Transcript: %s", user_transcript);
#endif
                // Check if username or password query
                cmd.type = parse_kind_and_query(user_transcript, &query);
                if (cmd.type == AI_CMD_CRED_USERNAME || cmd.type == AI_CMD_CRED_PASSWORD) {
                    // Lookup credentials via AI
                    err = ai_utils_lookup_creds(cmd.type, query, ai_response, sizeof(ai_response));
                } else { // Regular AI keyboard request
                    // Load autokey prompt
                    memset(prompt_buf, 0, sizeof(prompt_buf)); // Zero out previous contents
                    const char *prompt = ai_utils_get_autokey_prompt(prompt_buf, sizeof(prompt_buf));

#ifdef POLYCAST5_DEBUG
                    if (cmd.reasoning) {
                        ESP_LOGI(TAG, "Sending xAI cmd WITH reasoning");
                    } else {
                        ESP_LOGI(TAG, "Sending xAI cmd WITHOUT reasoning");
                    }
#endif
                    // Call chat API with SSE streaming (types each chunk over BLE as it arrives)
                    err = ai_utils_send_command_xai_stream(prompt, user_transcript, ai_response, sizeof(ai_response), cmd.reasoning, stream_to_bluetooth_cb, NULL);

                    // Send NULL sentinel so bluetooth_task flushes any buffered partial tag
                    // Only on success/normal completion - on abort the queue was already drained
                    if (!(xEventGroupGetBits(xBluetoothEventGroup) & BLUETOOTH_CANCEL_TYPING_BIT)) {
                        char *end_marker = NULL;
                        xQueueSend(xBluetoothAiStreamQueue, &end_marker, pdMS_TO_TICKS(5000));
                    }
                }
            } else {
                ESP_LOGE(TAG, "Realtime STT failed: %s", esp_err_to_name(err));

                // Signal error
                xEventGroupSetBits(xAiEventGroup, AI_THINKING_FAILED_BIT);
            }
            // Disable mic
            ai_voice_free_pcm(&pcm); // Free PCM buffer
            ESP_ERROR_CHECK(ai_voice_deinit());
        } else if (cmd.type == AI_CMD_RAW_FRAMES) { // Organizing raw Wi-Fi frames
#ifdef POLYCAST5_DEBUG
            size_t msg_len = (cmd.msg_len != 0) ? cmd.msg_len : strlen(cmd.msg);
            ESP_LOGI(TAG, "AI_CMD_RAW_FRAMES payload len=%u", (unsigned)msg_len);
#endif
            // Build autotype prompt (NVS override; fallback to compiled default)
            memset(prompt_buf, 0, sizeof(prompt_buf)); // Zero out previous contents
            const char *prompt = ai_utils_get_pkt_analysis_prompt(prompt_buf, sizeof(prompt_buf));

            // 'ai_response' output
            err = ai_utils_send_command_xai(prompt, cmd.msg, ai_response, sizeof(ai_response), cmd.reasoning);
        }

        // If good, log and send to bluetooth task
        if (err == ESP_OK) {
            if (cmd.type == AI_CMD_KEYBOARD_DONE_REC) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "AI keyboard script streamed (len=%u): %s", (unsigned)strlen(ai_response), ai_response);
#endif
                // Already streamed to BLE keyboard via stream_to_bluetooth_cb: nothing to queue
            } else if (cmd.type == AI_CMD_CRED_USERNAME || cmd.type == AI_CMD_CRED_PASSWORD) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Credential script resolved (len=%u)", (unsigned)strlen(ai_response));
#endif
                // Signal done thinking
                xEventGroupSetBits(xAiEventGroup, AI_DONE_THINKING_BIT);

                // Type out the crediential
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
    if (xTaskCreate(ai_task, "ai_task", 1024 * 4, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start ai_task");
    }
}

// void ai_task_create(void)
// {
//     // 1024 * 5 = 5120 words => 5120 * 4 = 20480 bytes (20KB)
//     #define AI_TASK_STACK_SIZE (1024 * 5)

//     // Allocate stack in PSRAM
//     StackType_t *task_stack = (StackType_t *)heap_caps_malloc(AI_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
//     if (task_stack == NULL) {
//         ESP_LOGE(TAG, "ai_task_create: Failed to allocate PSRAM stack");
//         return;
//     }

//     // Allocate TCB in internal SRAM for performance
//     StaticTask_t *task_buffer = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
//     if (task_buffer == NULL) {
//         heap_caps_free(task_stack); // Clean up
//         ESP_LOGE(TAG, "ai_task_create: Failed to allocate TCB");
//         return;
//     }

//     // Create the task
//     TaskHandle_t task_handle = xTaskCreateStatic(
//         ai_task,                   // Task function
//         "ai_task",                 // Name
//         AI_TASK_STACK_SIZE,        // Stack depth (in words)
//         NULL,                      // Parameters
//         POLYCAST5_PRIORITY_MEDIUM, // Priority
//         task_stack,                // Pre-allocated stack
//         task_buffer                // Pre-allocated TCB
//     );

//     if (task_handle == NULL) {
//         heap_caps_free(task_stack);
//         heap_caps_free(task_buffer);
//         ESP_LOGE(TAG, "ai_task_create: Failed to create ai_task");
//     }
// }