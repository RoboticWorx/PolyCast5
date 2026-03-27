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

SemaphoreHandle_t xEspCmdRxStatusSemaphore;
SemaphoreHandle_t xEspCmdTxSuccessSemaphore;
SemaphoreHandle_t xEspCmdTxFailedSemaphore;

QueueHandle_t xEspSendEncKeyQueueNVS;
QueueHandle_t xEspSendEncKeyQueue;
QueueHandle_t xEspSendCmdQueue;
QueueHandle_t xEspSendMqttQueue;

static void espnow_task(void *param)
{
    xEspCmdRxStatusSemaphore = xSemaphoreCreateBinary();
    configASSERT(xEspCmdRxStatusSemaphore);
    xEspCmdTxSuccessSemaphore = xSemaphoreCreateBinary();
    configASSERT(xEspCmdTxSuccessSemaphore);
    xEspCmdTxFailedSemaphore = xSemaphoreCreateBinary();
    configASSERT(xEspCmdTxFailedSemaphore);
    
    xEspSendEncKeyQueueNVS = xQueueCreate(1, LORA_PCP_ENC_KEY_LEN);
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
    
    while (1) {

        // Key generated and requesting send for LoRa handshake
        if (xQueueReceive(xEspSendEncKeyQueue, received_enc_key, 0) == pdPASS) {
            // Start radio and initialize ESP-NOW
            ESP_ERROR_CHECK(espnow_utils_wifi_radio_start(WIFI_CHANNEL));
            ESP_ERROR_CHECK(espnow_utils_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL, false, NULL));
            
            // Send the data
            espnow_utils_send_data(UNIVERSAL_MAC, received_enc_key, LORA_PCP_ENC_KEY_LEN);
            
            // Stop radio and de-initialize ESP-NOW
            ESP_ERROR_CHECK(espnow_utils_espnow_deinit());
            ESP_ERROR_CHECK(espnow_utils_wifi_radio_stop());
            
            // Send the data to LCD task to save to NVS under given option
            xQueueSend(xEspSendEncKeyQueueNVS, received_enc_key, portMAX_DELAY);
        }
        
        // Sharing MAC address as unique token for MQTT commands
        if (xQueueReceive(xEspSendMqttQueue, &espnow_mqtt, 0) == pdPASS) {
            wifi_utils_radio_stop();
            
            // Start radio and initialize ESP-NOW
            ESP_ERROR_CHECK(espnow_utils_wifi_radio_start(WIFI_CHANNEL));
            ESP_ERROR_CHECK(espnow_utils_espnow_init(UNIVERSAL_MAC, WIFI_CHANNEL, false, NULL));
            
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
            
            // Stop radio and de-initialize ESP-NOW
            ESP_ERROR_CHECK(espnow_utils_espnow_deinit());
            ESP_ERROR_CHECK(espnow_utils_wifi_radio_stop());
            
            // Reconnect to previous network
            xEventGroupSetBits(xWifiEventGroup, WIFI_RECONNECT_BIT);
        }
        
        // Sending ESP32 -> ESP32 command via ESP-NOW
        if (xQueueReceive(xEspSendCmdQueue, &espnow_cmd, 0) == pdPASS) {
            // Start radio and initialize ESP-NOW
            ESP_ERROR_CHECK(espnow_utils_wifi_radio_start(WIFI_CHANNEL));
            if (espnow_utils_espnow_init(espnow_cmd.mac_selected, WIFI_CHANNEL, espnow_cmd.enc, espnow_cmd.enc ? espnow_cmd.lmk : NULL) != ESP_OK) {
                xSemaphoreGive(xEspCmdTxFailedSemaphore); // Mark as failed TX for LCD
                
                // Stop radio and de-initialize ESP-NOW
                ESP_ERROR_CHECK(espnow_utils_espnow_deinit());
                ESP_ERROR_CHECK(espnow_utils_wifi_radio_stop());
            
                continue;
            }
            
            // Build a text payload from the cmd (more secure)
            char tx_payload[ESP_NOW_MAX_DATA_LEN];
            int tx_payload_len = snprintf(tx_payload, sizeof(tx_payload), "PolyCast5_Command_Value: %u", espnow_cmd.cmd_to_send); // Send only number of bytes needed
            // Check payload
            if (tx_payload_len < 0 || tx_payload_len >= sizeof(tx_payload)) {
                ESP_LOGE(TAG, "Payload snprintf failed or too long.");
                tx_payload_len = 0;
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
            ESP_ERROR_CHECK(espnow_utils_espnow_deinit());
            ESP_ERROR_CHECK(espnow_utils_wifi_radio_stop());
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
