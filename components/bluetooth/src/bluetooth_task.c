#include "bluetooth_portal.h"
#include "esp_err.h"
#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_random.h"

#include "bluetooth_utils.h"
#include "portmacro.h"
#include "bluetooth_task.h"
#include "bluetooth_nvs.h"
#include "gpio_task.h"
#include "gpio_utils.h"
#include "ai_utils.h"

#define TAG "BLUETOOTH_TASK"

EventGroupHandle_t xBluetoothEventGroup;

QueueHandle_t xBluetoothMediaCmdQueue;
QueueHandle_t xBluetoothAiCmdQueue;
QueueHandle_t xBluetoothAiStreamQueue;

extern volatile bluetooth_state_t bluetooth_state;

char bt_wifi_portal_pass[64];

POLYCAST5_USE_PSRAM_BSS static char send_buf[2048];
static char *ai_script;

static uint16_t bluetooth_cmd = 0;
static uint8_t battery_percentage = 100;
static const TickType_t battery_timer_interval = pdMS_TO_TICKS(1000);

// Buffer for reassembling streamed AI chunks so <tag> tokens and !END! aren't split across chunks
#define STREAM_BUF_SZ 512
#define STREAM_END_MARKER "!END!"
#define STREAM_END_MARKER_LEN 5
POLYCAST5_USE_PSRAM_BSS static char stream_buf[STREAM_BUF_SZ];
static size_t stream_buf_len = 0;
static bool stream_end_detected = false;

// Append text to stream_buf, flush complete segments (no partial <tag> or !END!) via send_script
static void stream_buf_append(const char *text, size_t len)
{
    if (stream_end_detected) {
        return; // Already hit !END!, ignore further chunks
    }

    // Append as much as fits
    size_t space = STREAM_BUF_SZ - 1 - stream_buf_len;
    if (len > space) {
        len = space;
    }
    memcpy(stream_buf + stream_buf_len, text, len);
    stream_buf_len += len;
    stream_buf[stream_buf_len] = '\0';

    // Check for complete !END! marker in buffer
    char *end_marker = strstr(stream_buf, STREAM_END_MARKER);
    if (end_marker) {
        // Send everything before !END!
        if (end_marker > stream_buf) {
            *end_marker = '\0';
            bluetooth_utils_send_script(stream_buf, 2);
        }
        stream_buf_len = 0;
        stream_buf[0] = '\0';
        stream_end_detected = true;
        xEventGroupSetBits(xBluetoothEventGroup, BLUETOOTH_DONE_TYPING_BIT); // Signal done typing
        return;
    }

    // Find the last '<' that has no matching '>'
    // Everything before it is safe to send; keep the rest
    const char *last_open = NULL;
    for (size_t i = 0; i < stream_buf_len; ++i) {
        if (stream_buf[i] == '<') {
            last_open = &stream_buf[i];
        } else if (stream_buf[i] == '>') {
            last_open = NULL; // Closed, no longer partial
        }
    }

    size_t safe_len = last_open ? (size_t)(last_open - stream_buf) : stream_buf_len;

    // Hold back potential partial !END! prefix at the tail of the safe portion
    // e.g. "!", "!E", "!EN", "!END" could be the start of !END! split across chunks
    for (int plen = STREAM_END_MARKER_LEN - 1; plen >= 1; --plen) {
        if (safe_len >= (size_t)plen && memcmp(stream_buf + safe_len - plen, STREAM_END_MARKER, plen) == 0) {
            safe_len -= plen;
            break;
        }
    }

    if (safe_len > 0) {
        // Temporarily NULL-terminate the safe portion and send it
        char saved = stream_buf[safe_len];
        stream_buf[safe_len] = '\0';
        bluetooth_utils_send_script(stream_buf, 2);
        stream_buf[safe_len] = saved;

        // Shift remainder to front
        size_t remain = stream_buf_len - safe_len;
        if (remain > 0) {
            memmove(stream_buf, stream_buf + safe_len, remain);
        }
        stream_buf_len = remain;
        stream_buf[stream_buf_len] = '\0';
    }
}

// Flush whatever is left in stream_buf (called on end-of-stream)
static void stream_buf_flush(void)
{
    if (stream_buf_len > 0) {
        stream_buf[stream_buf_len] = '\0';
        bluetooth_utils_send_script(stream_buf, 2);
        stream_buf_len = 0;
        stream_buf[0] = '\0';
    }
}

// Drop any queued streamed AI chunks and reset local reassembly state
static void stream_buf_discard_all(void)
{
    char *chunk = NULL;

    while (xQueueReceive(xBluetoothAiStreamQueue, &chunk, 0) == pdTRUE) {
        if (chunk) {
            free(chunk);
        }
    }

    stream_buf_len = 0;
    stream_buf[0] = '\0';
    stream_end_detected = false;
}

static void bluetooth_task(void *arg)
{
    xBluetoothEventGroup = xEventGroupCreate();
    configASSERT(xBluetoothEventGroup);

    xBluetoothMediaCmdQueue = xQueueCreate(1, sizeof(uint16_t));
    configASSERT(xBluetoothMediaCmdQueue);
    xBluetoothAiCmdQueue = xQueueCreate(1, sizeof(char *));
    configASSERT(xBluetoothAiCmdQueue);
    xBluetoothAiStreamQueue = xQueueCreate(100, sizeof(char *));
    configASSERT(xBluetoothAiStreamQueue);
    
    TickType_t battery_timer_last = xTaskGetTickCount();

    // If Wi-Fi portal password NVS doesn't exist yet, set it
    if (bluetooth_portal_wifi_pass_load_nvs(bt_wifi_portal_pass, sizeof(bt_wifi_portal_pass)) != ESP_OK) {
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
            bt_wifi_portal_pass[i] = alphabet[r % N];
        }
        bt_wifi_portal_pass[PASS_LEN] = '\0';
        
        // Save that version to NVS
        bluetooth_portal_wifi_pass_save_nvs(bt_wifi_portal_pass);
        
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGW(TAG, "Setting first time BT Wi-Fi portal password: %s", bt_wifi_portal_pass);
#endif
    } else {
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGI(TAG, "Using pre-set BT Wi-Fi portal password: '%s'", bt_wifi_portal_pass);
#endif
    }

    // If 6 digit BT pairing passkey NVS doesn't exist yet, set that too
    uint32_t pairing_key = 0; // To be random 6 digit passkey
    if (bluetooth_nvs_pairing_key_load(&pairing_key) != ESP_OK) {
        // Create first time
        pairing_key = esp_random() % 1000000;
        
        // Save that version to NVS
        bluetooth_nvs_pairing_key_save(pairing_key);
        
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGW(TAG, "Setting first time BT pairing key: %d", pairing_key);
#endif
    } else {
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGI(TAG, "Using pre-set BT pairing key: '%d'", pairing_key);
#endif
    }
    
    while (1) {
        // If a bluetooth command is received
        if (xQueueReceive(xBluetoothMediaCmdQueue, &bluetooth_cmd, 0) == pdTRUE) {
            /* Initialization stuff */
            // Initialize command received
            if (bluetooth_cmd == BLUETOOTH_CMD_INIT) {
                bluetooth_utils_init();
            } else if (bluetooth_cmd == BLUETOOTH_CMD_DEINIT) { // De-initialize command received
                stream_buf_discard_all();
                bluetooth_utils_deinit(); // BLUETOOTH_CONNECTED_BIT cleared in deinit on success
                xEventGroupClearBits(xBluetoothEventGroup, BLUETOOTH_CANCEL_TYPING_BIT);
            } else if (bluetooth_cmd == BLUETOOTH_CMD_UNPAIR_ALL) { // Unpair all devices command received
                bluetooth_utils_forget_all_peers();

                // Clear remembered NVS
                esp_err_t err = bluetooth_nvs_clear_peers_list(false); // Clear all
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "bluetooth_nvs_clear_peers_list error: %s", esp_err_to_name(err));
                }

                bluetooth_utils_deinit();
                bluetooth_utils_init();
            } else if (bluetooth_cmd == BLUETOOTH_CMD_UNPAIR_ALL_NO_REINIT) { // Unpair all devices no reinit command received
                bluetooth_utils_forget_all_peers();

                // Clear remembered NVS
                esp_err_t err = bluetooth_nvs_clear_peers_list(false); // Clear all
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "bluetooth_nvs_clear_peers_list error: %s", esp_err_to_name(err));
                }

                bluetooth_utils_deinit();
            }
            /* Media commands */
            // Vol-up command received
            else if (bluetooth_cmd == BLUETOOTH_CMD_VOLUME_UP && bluetooth_state == BT_STATE_RUNNING) {
                bluetooth_utils_send_media(BLUETOOTH_CMD_VOLUME_UP, true);
                vTaskDelay(pdMS_TO_TICKS(100)); // Simulate press
                bluetooth_utils_send_media(BLUETOOTH_CMD_VOLUME_UP, false);
            } else if (bluetooth_cmd == BLUETOOTH_CMD_VOLUME_DOWN && bluetooth_state == BT_STATE_RUNNING) { // Vol-down command received
                bluetooth_utils_send_media(BLUETOOTH_CMD_VOLUME_DOWN, true);
                vTaskDelay(pdMS_TO_TICKS(100));
                bluetooth_utils_send_media(BLUETOOTH_CMD_VOLUME_DOWN, false);
            } else if (bluetooth_cmd == BLUETOOTH_CMD_NEXT_TRK && bluetooth_state == BT_STATE_RUNNING) { // Next track command received
                bluetooth_utils_send_media(BLUETOOTH_CMD_NEXT_TRK, true);
                vTaskDelay(pdMS_TO_TICKS(100));
                bluetooth_utils_send_media(BLUETOOTH_CMD_NEXT_TRK, false);
                vTaskDelay(pdMS_TO_TICKS(10));
                bluetooth_utils_send_script("<right>", 1); // Also send right for if using next to fast forward
            } else if (bluetooth_cmd == BLUETOOTH_CMD_PREV_TRK && bluetooth_state == BT_STATE_RUNNING) { // Previous track command received
                bluetooth_utils_send_media(BLUETOOTH_CMD_PREV_TRK, true);
                vTaskDelay(pdMS_TO_TICKS(100));
                bluetooth_utils_send_media(BLUETOOTH_CMD_PREV_TRK, false);
                vTaskDelay(pdMS_TO_TICKS(10));
                bluetooth_utils_send_script("<left>", 1); // Also send left for if using previous to rewind
            } else if (bluetooth_cmd == BLUETOOTH_CMD_PLAY_PAUSE && bluetooth_state == BT_STATE_RUNNING) { // Play pause command received
                bluetooth_utils_send_media(BLUETOOTH_CMD_PLAY_PAUSE, true);
                vTaskDelay(pdMS_TO_TICKS(100));
                bluetooth_utils_send_media(BLUETOOTH_CMD_PLAY_PAUSE, false);
            } else if (bluetooth_cmd == BLUETOOTH_CMD_MUTE && bluetooth_state == BT_STATE_RUNNING) { // Mute command received
                bluetooth_utils_send_media(BLUETOOTH_CMD_MUTE, true);
                vTaskDelay(pdMS_TO_TICKS(100));
                bluetooth_utils_send_media(BLUETOOTH_CMD_MUTE, false);
            }
            /* Keyboard scripts */
            else if (bluetooth_cmd >= BLUETOOTH_SCRIPT_OFFSET && bluetooth_state == BT_STATE_RUNNING) {
                // Menu index that was encoded by the UI
                uint16_t menu_idx = (uint16_t)(bluetooth_cmd - BLUETOOTH_SCRIPT_OFFSET);

#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Received cmd index: %u -> menu index: %u", (unsigned)bluetooth_cmd, (unsigned)menu_idx);
#endif
                // "Test" at menu index 1, handle it specially
                if (menu_idx == 1) {
                    const char *TEST_TXT =
                            "Thanks for choosing PolyCast5! As you can see, this autotype feature can be quite handy. "
                            "It's perfect for funny pranks, auto-filling long passwords, speeding up typing, coding, you name it! "
                            "If you see yourself more an ethical hacker, this is also basically a Bluetooth USB Rubber Ducky. "
                            "To start adding your own text scripts, just go to 'Add/Edit Script' and follow the few simple instructions.\n";

                    bluetooth_utils_send_script(TEST_TXT, 1);
                    continue;
                }
                /* If presentation mode command */
                  else if (bluetooth_cmd == BLUETOOTH_SCRIPT_PRESENTATION_START) {
                    bluetooth_utils_send_script("<f5>", 1);
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_PRESENTATION_ESC) {
                    bluetooth_utils_send_script("<esc>", 1);
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_PRESENTATION_BLANK) {
                    bluetooth_utils_send_script("b", 1);
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_PRESENTATION_LEFT) {
                    bluetooth_utils_send_script("<left>", 1);
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_PRESENTATION_RIGHT) {
                    bluetooth_utils_send_script("<right>", 1);
                    continue;
                }
                /* If social mode command */
                  else if (bluetooth_cmd == BLUETOOTH_SCRIPT_SCROLL_UP) {
                    bluetooth_utils_send_script("<up><up><up><up>", 1);
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_SCROLL_DOWN) {
                    bluetooth_utils_send_script("<down><down><down><down>", 1);
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_SCROLL_PG_UP) {
                    bluetooth_utils_send_script("<pgup>", 1);
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_SCROLL_PG_DOWN) {
                    bluetooth_utils_send_script("<pgdn>", 1);
                    continue;
                }
                /* If social media scroller command */
                  else if (bluetooth_cmd == BLUETOOTH_SCRIPT_SOCIALS_UP) {
                    bluetooth_utils_send_script("<up><delay=150>k", 1); // Try both
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_SOCIALS_DOWN) {
                    bluetooth_utils_send_script("<down><delay=150>j", 1); // Try both
                    continue;
                } else if (bluetooth_cmd == BLUETOOTH_SCRIPT_SOCIALS_LIKE) {
                    bluetooth_utils_send_script("l", 1);
                    continue;
                }

                // Menu has 2 fixed rows before user scripts:
                // So the first user script is menu_idx == 2 -> NVS index 0
                if (menu_idx >= BT_NUM_KEYBOARD_BASE) {
                    uint8_t script_idx = (uint8_t)(menu_idx - BT_NUM_KEYBOARD_BASE); // 0-based NVS slot

                    size_t blen = 0;

                    // Ask NVS for the stored body - Pass full sizeof(buf) so there's room for the NUL-terminator
                    esp_err_t err = bluetooth_portal_script_body_get_nvs(script_idx, send_buf, sizeof(send_buf), &blen);
                    if (err == ESP_OK && blen > 0 && send_buf[0] != '\0') {
                        // NVS returns a C-string: Just send it
#ifdef POLYCAST5_DEBUG
                        ESP_LOGI(TAG, "Sending script: %s", send_buf);
#endif
                        // Send the script
                        bluetooth_utils_send_script(send_buf, 1);
                    } else {
#ifdef POLYCAST5_DEBUG
                        ESP_LOGW(TAG, "Failed/no script body at idx=%u (err=%s, blen=%u)",
                                (unsigned)script_idx, esp_err_to_name(err), (unsigned)blen);
#endif
                    }
                } else {
#ifdef POLYCAST5_DEBUG
                    ESP_LOGW(TAG, "Unhandled menu_idx=%u for BLUETOOTH_SCRIPT_OFFSET", (unsigned)menu_idx);
#endif
                }
            }
        }

        // If a AI is typing (this is a cool comment lol)
        // Password type cmd not streamed, send as a whole
        if (xQueueReceive(xBluetoothAiCmdQueue, &ai_script, 0) == pdTRUE) {
            // Send the script
            bluetooth_utils_send_script(ai_script, 2);

            // Notify LCD we're done typing the credential
            xEventGroupSetBits(xBluetoothEventGroup, BLUETOOTH_DONE_TYPING_BIT);
        }

        // Drain streamed AI chunks (strdup'd by sender, freed here)
        // Uses stream_buf to reassemble partial <tag> tokens across chunk boundaries
        char *stream_chunk = NULL;
        while (xQueueReceive(xBluetoothAiStreamQueue, &stream_chunk, 0) == pdTRUE) {
            // Drain the queue and exit if cancel bit is set
            if (xEventGroupGetBits(xBluetoothEventGroup) & BLUETOOTH_CANCEL_TYPING_BIT) {
                if (stream_chunk) {
                    free(stream_chunk); // Free the received chunk if not NULL
                }
                stream_buf_discard_all();
                break;
            }

            if (stream_chunk) {
                stream_buf_append(stream_chunk, strlen(stream_chunk));
                free(stream_chunk);
            } else {
                // NULL sentinel = end of stream, flush remaining buffer
                stream_buf_flush();

                // If !END! was never detected (truncated response), signal done as fallback
                if (!stream_end_detected) {
                    xEventGroupSetBits(xBluetoothEventGroup, BLUETOOTH_DONE_TYPING_BIT);
                }
                stream_end_detected = false; // Reset for next stream
            }
        }

        // Get device battery level
        xQueueReceive(xAdcBatBluetoothQueue, &battery_percentage, 0);
        
        // Update bluetooth battery level every battery_timer_interval
        if (xTaskGetTickCount() - battery_timer_last >= battery_timer_interval && bluetooth_state == BT_STATE_RUNNING) {
            battery_timer_last = xTaskGetTickCount();
            
            bluetooth_utils_set_battery_level(battery_percentage);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void bluetooth_task_create(void)
{
    if (xTaskCreate(bluetooth_task, "bluetooth_task", 1024 * 4, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start bluetooth_task");
    }
}