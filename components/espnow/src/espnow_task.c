#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_log_buffer.h"
#include "portmacro.h"

#include "lora_pcp.h"
#include "espnow_utils.h"
#include "espnow_task.h"
#include "wifi_utils.h"
#include "wifi_task.h"

#define TAG "ESPNOW_TASK"

static espnow_cmd_t espnow_cmd;
static espnow_mqtt_t espnow_mqtt;

static const uint8_t UNIVERSAL_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint8_t received_enc_key[LORA_PCP_ENC_KEY_LEN];

// Accelerometer streaming session state
static bool accel_streaming = false;
static uint8_t accel_stream_mac[ESPNOW_MAC_SIZE];

SemaphoreHandle_t xEspCmdRxStatusSemaphore;
SemaphoreHandle_t xEspCmdTxSuccessSemaphore;
SemaphoreHandle_t xEspCmdTxFailedSemaphore;

QueueHandle_t xEspSendEncKeyQueueNVS;
QueueHandle_t xEspSendEncKeyQueue;
QueueHandle_t xEspSendCmdQueue;
QueueHandle_t xEspSendMqttQueue;
QueueHandle_t xEspEcompassStreamCtrlQueue;
QueueHandle_t xEspEcompassStreamQueue;

static void espnow_task(void *param)
{
    xEspCmdRxStatusSemaphore = xSemaphoreCreateBinary();
    configASSERT(xEspCmdRxStatusSemaphore);
    xEspCmdTxSuccessSemaphore = xSemaphoreCreateBinary();
    configASSERT(xEspCmdTxSuccessSemaphore);
    xEspCmdTxFailedSemaphore = xSemaphoreCreateBinary();
    configASSERT(xEspCmdTxFailedSemaphore);
    
    xEspSendEncKeyQueueNVS = xQueueCreate(1, sizeof(espnow_enc_key_result_t));
    if (xEspSendEncKeyQueueNVS == NULL) {
        ESP_LOGE(TAG, "Failed to create xEspSendEncKeyQueueNVS");
    }
    configASSERT(xEspSendEncKeyQueueNVS);
    
    xEspSendEncKeyQueue = xQueueCreate(1, LORA_PCP_ENC_KEY_LEN);
    if (xEspSendEncKeyQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create xEspSendEncKeyQueue");
    }
    configASSERT(xEspSendEncKeyQueue);
    
    xEspSendCmdQueue = xQueueCreate(1, sizeof(espnow_cmd_t));
    if (xEspSendCmdQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create xEspSendCmdQueue");
    }
    configASSERT(xEspSendCmdQueue);
    
    xEspSendMqttQueue = xQueueCreate(1, sizeof(espnow_mqtt_t));
    if (xEspSendMqttQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create xEspSendMqttQueue");
    }
    configASSERT(xEspSendMqttQueue);

    // Accelerometer streaming: control (start/stop) + latest-sample queues
    xEspEcompassStreamCtrlQueue = xQueueCreate(2, sizeof(espnow_ecompass_ctrl_t));
    configASSERT(xEspEcompassStreamCtrlQueue);
    xEspEcompassStreamQueue = xQueueCreate(1, sizeof(espnow_ecompass_t));
    configASSERT(xEspEcompassStreamQueue);

    while (1) {
        // Key generated and requesting send for LoRa handshake
        if (xQueueReceive(xEspSendEncKeyQueue, received_enc_key, 0) == pdPASS) {
            esp_err_t err;

            // Always post a result so the LCD task is never left waiting
            espnow_enc_key_result_t key_result = { .success = false };
            memcpy(key_result.key, received_enc_key, LORA_PCP_ENC_KEY_LEN);

            // Sync frame: ESPNOW_MAGIC + 16-byte LoRa key + 1-byte spreading factor + 1-byte region
            // Magic tag lets the plug identify this frame by content; SF + region make the plug's RX match the remote's TX
            uint8_t sync_payload[ESPNOW_MAGIC_LEN + LORA_PCP_ENC_KEY_LEN + 2]; // +1 SF, +1 region
            memcpy(sync_payload, ESPNOW_MAGIC, ESPNOW_MAGIC_LEN);
            memcpy(sync_payload + ESPNOW_MAGIC_LEN, received_enc_key, LORA_PCP_ENC_KEY_LEN);
            sync_payload[ESPNOW_MAGIC_LEN + LORA_PCP_ENC_KEY_LEN]     = lora_pcp_load_sf_nvs();
            sync_payload[ESPNOW_MAGIC_LEN + LORA_PCP_ENC_KEY_LEN + 1] = (uint8_t)lora_pcp_load_region_nvs();

            // Start radio and initialize ESP-NOW
            err = espnow_utils_wifi_radio_start(WIFI_CHANNEL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "enc_key: radio_start failed: %s", esp_err_to_name(err));
                xQueueOverwrite(xEspSendEncKeyQueueNVS, &key_result); // Send failure
                continue;
            }
            err = espnow_utils_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL, false, NULL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "enc_key: espnow_init failed: %s", esp_err_to_name(err));
                espnow_utils_wifi_radio_stop();
                xQueueOverwrite(xEspSendEncKeyQueueNVS, &key_result); // Send failure
                continue;
            }

            // Send the data (magic + key + spreading factor + region)
            err = espnow_utils_send_data(UNIVERSAL_MAC, sync_payload, sizeof(sync_payload));
            if (err != ESP_OK) {
                key_result.success = false;
                ESP_LOGE(TAG, "espnow_utils_send_data: Failed to send encryption key: %s", esp_err_to_name(err));
            } else {
                key_result.success = true;
            }

            vTaskDelay(pdMS_TO_TICKS(100));

            // Stop radio and de-initialize ESP-NOW
            espnow_utils_espnow_deinit();
            espnow_utils_wifi_radio_stop();

            // Send the result to LCD task to save to NVS under given option
            // (overwrite: depth-1 queue, latest result wins, never blocks)
            xQueueOverwrite(xEspSendEncKeyQueueNVS, &key_result);
        }

        // Sharing MAC address as unique token for MQTT commands
        if (xQueueReceive(xEspSendMqttQueue, &espnow_mqtt, 0) == pdPASS) {
            esp_err_t err;
            wifi_utils_radio_stop();

            // Start radio and initialize ESP-NOW
            err = espnow_utils_wifi_radio_start(WIFI_CHANNEL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "mqtt: radio_start failed: %s", esp_err_to_name(err));
                xEventGroupSetBits(xWifiEventGroup, WIFI_RECONNECT_BIT);
                continue;
            }
            err = espnow_utils_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL, false, NULL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "mqtt: espnow_init failed: %s", esp_err_to_name(err));
                espnow_utils_wifi_radio_stop();
                xEventGroupSetBits(xWifiEventGroup, WIFI_RECONNECT_BIT);
                continue;
            }

            // Combine the info into a single string
            char payload[134];
            int len = snprintf(
                    payload, sizeof(payload),
                    "%s:%s:%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                    espnow_mqtt.ssid,
                    espnow_mqtt.password,
                    espnow_mqtt.key[0], espnow_mqtt.key[1], espnow_mqtt.key[2], espnow_mqtt.key[3],
                    espnow_mqtt.key[4], espnow_mqtt.key[5], espnow_mqtt.key[6], espnow_mqtt.key[7],
                    espnow_mqtt.key[8], espnow_mqtt.key[9], espnow_mqtt.key[10], espnow_mqtt.key[11],
                    espnow_mqtt.key[12], espnow_mqtt.key[13], espnow_mqtt.key[14], espnow_mqtt.key[15]
            );

#ifdef POLYCAST5_DEBUG
            ESP_LOG_BUFFER_HEX("Sending MQTT KEY", espnow_mqtt.key, 16);
            ESP_LOGI(TAG, "Sending MQTT: %s", payload);
#endif

            // Send the data
            espnow_utils_send_data(UNIVERSAL_MAC, (uint8_t*)payload, len);

            vTaskDelay(pdMS_TO_TICKS(100));

            // Stop radio and de-initialize ESP-NOW
            espnow_utils_espnow_deinit();
            espnow_utils_wifi_radio_stop();

            // Reconnect to previous network
            xEventGroupSetBits(xWifiEventGroup, WIFI_RECONNECT_BIT);
        }

        // Sending ESP32 -> ESP32 command via ESP-NOW
        if (xQueueReceive(xEspSendCmdQueue, &espnow_cmd, 0) == pdPASS) {
            esp_err_t err;

            // Start radio and initialize ESP-NOW
            err = espnow_utils_wifi_radio_start(WIFI_CHANNEL);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "cmd: radio_start failed: %s", esp_err_to_name(err));
                xSemaphoreGive(xEspCmdTxFailedSemaphore);
                continue;
            }
            if (espnow_utils_espnow_init(espnow_cmd.mac_selected, WIFI_CHANNEL, espnow_cmd.enc, espnow_cmd.enc ? espnow_cmd.lmk : NULL) != ESP_OK) {
                ESP_LOGE(TAG, "cmd: espnow_init failed");
                xSemaphoreGive(xEspCmdTxFailedSemaphore);
                espnow_utils_espnow_deinit();
                espnow_utils_wifi_radio_stop();
                continue;
            }

            // Build a text payload from the cmd (more secure)
            char tx_payload[ESP_NOW_MAX_DATA_LEN];
            int tx_payload_len = snprintf(tx_payload, sizeof(tx_payload), ESPNOW_MAGIC "%u", espnow_cmd.cmd_to_send); // Send only number of bytes needed
            // Check payload
            if (tx_payload_len < 0 || tx_payload_len >= sizeof(tx_payload)) {
                ESP_LOGE(TAG, "Payload snprintf failed or too long.");
                tx_payload_len = 0;
                continue;
            }

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Sending: %s", tx_payload);
            ESP_LOG_BUFFER_HEX("To MAC", espnow_cmd.mac_selected, ESPNOW_MAC_SIZE);
            if (espnow_cmd.enc) {
                ESP_LOG_BUFFER_HEX("LMK", espnow_cmd.lmk, LMK_LEN);
            }
#endif

            // Send the data
            if (espnow_utils_send_data(espnow_cmd.mac_selected, (uint8_t*)tx_payload, tx_payload_len) == ESP_OK) {
                // Notify the LCD that the transmission was successful
                xSemaphoreGive(xEspCmdTxSuccessSemaphore);
            } else {
                xSemaphoreGive(xEspCmdTxFailedSemaphore); // Mark as failed TX for LCD
            }

            // Wait for ACK frame
            vTaskDelay(pdMS_TO_TICKS(100));

            // Stop radio and de-initialize ESP-NOW
            espnow_utils_espnow_deinit();
            espnow_utils_wifi_radio_stop();
        }

        // Accelerometer streaming: start/stop a long-lived session
        espnow_ecompass_ctrl_t ecompass_ctrl;
        if (xQueueReceive(xEspEcompassStreamCtrlQueue, &ecompass_ctrl, 0) == pdPASS) {
            if (ecompass_ctrl.start && !accel_streaming) {
                // Make sure the radio is stopped first
                wifi_utils_radio_stop();

                // Bring the radio + peer up once for the whole session
                if (espnow_utils_wifi_radio_start(WIFI_CHANNEL) == ESP_OK &&
                        espnow_utils_espnow_init(ecompass_ctrl.mac_selected, WIFI_CHANNEL,
                        ecompass_ctrl.enc == true, ecompass_ctrl.enc ? ecompass_ctrl.lmk : NULL) == ESP_OK) {
                    memcpy(accel_stream_mac, ecompass_ctrl.mac_selected, ESPNOW_MAC_SIZE);
                    accel_streaming = true;

                    // Drop any sample left over from a previous session
                    xQueueReset(xEspEcompassStreamQueue);
                } else {
                    ESP_LOGE(TAG, "accel: stream start failed");
                    espnow_utils_espnow_deinit();
                    espnow_utils_wifi_radio_stop();
                }
            } else if (!ecompass_ctrl.start && accel_streaming) {
                // Tear the session down
                espnow_utils_espnow_deinit();
                espnow_utils_wifi_radio_stop();
                accel_streaming = false;

                // Streaming's per-frame send_cb repeatedly gives the delivery semaphore
                // Drain it so the command page doesn't later see a stale received without a command being sent
                xSemaphoreTake(xEspCmdRxStatusSemaphore, 0);
            }
        }

        // While streaming, transmit each fresh accel sample as it arrives
        if (accel_streaming) {
            espnow_ecompass_t ecompass_sample;
            if (xQueueReceive(xEspEcompassStreamQueue, &ecompass_sample, 0) == pdPASS) {
                char tx_payload[ESP_NOW_MAX_DATA_LEN];

                // Format payload to send: ESPNOW_MAGICx,y,z -> "PC5: x,y,z"
                // x/y = tilt (deg), z = compass heading (deg)
                int tx_len = snprintf(tx_payload, sizeof(tx_payload), ESPNOW_MAGIC "%.1f,%.1f,%.1f",
                        (double)ecompass_sample.x, (double)ecompass_sample.y, (double)ecompass_sample.z);
                
                // Send it if formatting succeeded
                if (tx_len > 0 && tx_len < (int)sizeof(tx_payload)) {
                    if (espnow_utils_send_data(accel_stream_mac, (uint8_t *)tx_payload, tx_len) != ESP_OK) {
#ifdef POLYCAST5_DEBUG
                        ESP_LOGW(TAG, "xEspEcompassStreamQueue: eCompass payload send failed.");
#endif
                    }
                } else {
                    ESP_LOGE(TAG, "xEspEcompassStreamQueue: eCompass payload snprintf failed or too long.");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void espnow_task_create(void)
{
    if (xTaskCreate(espnow_task, "espnow_task", 1024 * 3, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start espnow_task");
    }
}
