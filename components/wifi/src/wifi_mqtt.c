#include "polycast5_macros.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_err.h"

#include "wifi_utils.h"
#include "wifi_mqtt.h"

#include "wifi_task.h"

#define TAG "WIFI_MQTT"

#define EXPECTED_MQTT_RX "PolyCast5MQTTRxSuccess"

static esp_mqtt_client_handle_t mqtt_client;

static char mqtt_active_ack_topic[80] = {0};

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Connected to MQTT");
#endif
            
            // Subscribe to any polycast5/.../ack
            esp_mqtt_client_subscribe(event->client, "polycast5/+/ack", 0);
            
            xEventGroupSetBits(xWifiEventGroup, WIFI_MQTT_CONNECTED_BIT); // Notify LCD we connected
            break;
            
        case MQTT_EVENT_DISCONNECTED:
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "Disconnected from MQTT");
#endif
            
            xEventGroupClearBits(xWifiEventGroup, WIFI_MQTT_CONNECTED_BIT); // Notify LCD we disconnected
            break;
            
        case MQTT_EVENT_PUBLISHED:
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Broker ACKed message ID %d on topic %.*s", event->msg_id, event->topic_len, event->topic);
#endif
            
            break;
            
        case MQTT_EVENT_DATA:
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "MQTT_EVENT_DATA triggered");
#endif

            // If received on active topic
            if (event->topic_len == strlen(mqtt_active_ack_topic) && strncmp(event->topic, mqtt_active_ack_topic, event->topic_len) == 0) {
                // Reject oversized payloads to avoid stack overflow
                if (event->data_len < 0 || event->data_len > 128) {
                    ESP_LOGW(TAG, "MQTT payload too large (%d), ignoring", event->data_len);
                    break;
                }
                // Format received
                char payload[128 + 1];
                memcpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';
                
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Received MQTT receipt='%s'", payload);
#endif
                
                // If matches expected format
                if (strcmp(payload, EXPECTED_MQTT_RX) == 0) {
#ifdef POLYCAST5_DEBUG
                    ESP_LOGI(TAG, "Received MQTT receipt matches!");
#endif

                    // Notify user of successful send
                    xEventGroupSetBits(xWifiEventGroup, WIFI_MQTT_SUCCESS_BIT);
                } else {
#ifdef POLYCAST5_DEBUG
                    ESP_LOGI(TAG, "Received MQTT receipt did not match (len=%d)", event->data_len);
#endif
                }
            }
            break;
            
        default:
            break;
    }
}

void wifi_mqtt_client_init(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address = {
                .uri = "mqtt://test.mosquitto.org"
            }
        },
        .session = {
            .keepalive = 60
        }
    };
    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
}

void wifi_mqtt_client_destroy(void)
{
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
}

void wifi_mqtt_client_stop(void)
{
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
    }
}

void wifi_mqtt_client_start(void)
{
    if (mqtt_client) {
        esp_mqtt_client_start(mqtt_client);
    }
}

void wifi_mqtt_client_publish(char *payload, const uint8_t key[16])
{
    // Sender and receiver topic
    char topic_cmd[80];

    // Build topics from the raw key
    snprintf(topic_cmd, sizeof(topic_cmd),
            "polycast5/%02X%02X%02X%02X%02X%02X%02X%02X"
            "%02X%02X%02X%02X%02X%02X%02X%02X/cmd",
            key[0],  key[1],  key[2],  key[3],
            key[4],  key[5],  key[6],  key[7],
            key[8],  key[9],  key[10], key[11],
            key[12], key[13], key[14], key[15]);
    
    // Ack topic is the same but with "ack" suffix
    snprintf(mqtt_active_ack_topic, sizeof(mqtt_active_ack_topic),
            "polycast5/%02X%02X%02X%02X%02X%02X%02X%02X"
            "%02X%02X%02X%02X%02X%02X%02X%02X/ack",
            key[0],  key[1],  key[2],  key[3],
            key[4],  key[5],  key[6],  key[7],
            key[8],  key[9],  key[10], key[11],
            key[12], key[13], key[14], key[15]);
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Active MQTT ACK:%s", mqtt_active_ack_topic);
#endif
    
    // Send the data
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic_cmd, payload, 0, 0, 0);
    
    if (msg_id != -1) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "MQTT send success: %d", msg_id);
        ESP_LOGI(TAG, "Sent '%s' to topic '%s'", payload, topic_cmd);
#endif
    } else {
        ESP_LOGE(TAG, "MQTT send FAILED: %d", msg_id);
    }
}
