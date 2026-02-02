#include "nvs.h"
#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_random.h"

#include "wifi_utils.h"
#include "wifi_ping.h"
#include "wifi_mqtt.h"
#include "wifi_deauth.h"
#include "wifi_task.h"
#include "wifi_ota_update.h"
#include "esp_app_desc.h"
#include "wifi_btc_web_portal.h"
#include "ai_key_web_portal.h"
#include "bluetooth_web_portal.h"
#include "ai_analysis_web_portal.h"

#define TAG "WIFI_TASK"

char btc_wifi_portal_pass[64];

POLYCAST5_USE_PSRAM static wifi_scan_t wifi_scan[WIFI_MAX_NETWORKS];
POLYCAST5_USE_PSRAM static wifi_scan_deauth_t wifi_scan_deauth[WIFI_MAX_NETWORKS];

static deauth_target_t deauth_target = {0};
static wifi_login_t selected_network = {0};
static wifi_sniff_t sniff_network = {0};

static wifi_mqtt_t wifi_mqtt = {0};

EventGroupHandle_t xWifiEventGroup;

QueueHandle_t xWifiScanQueue;
QueueHandle_t xWifiDeauthScanQueue;
QueueHandle_t xWifiDeauthTargetQueue;
QueueHandle_t xWifiDeauthStatsQueue;
QueueHandle_t xWifiSelectedNetworkQueue;
QueueHandle_t xWifiSniffQueue;
QueueHandle_t xWifiBeaconQueue;
QueueHandle_t xWifiDataQueue;
QueueHandle_t xWifiMqttCmdQueue;
QueueHandle_t xWifiPingQueue;
QueueHandle_t xWifiAiRawSniffQueue;

SemaphoreHandle_t xWifiCanSleepSemaphore;
SemaphoreHandle_t xWifiCycleSemaphore;
SemaphoreHandle_t xWifiPingSemaphore;

SemaphoreHandle_t xWifiRawFramesMutex;

EventGroupHandle_t xConnectionIconEventGroup;
EventGroupHandle_t xWiFiPortalEventGroup;

// OTA
QueueHandle_t xWifiOtaPctQueue;

static EventBits_t last_portal_bits = 0;
static EventBits_t last_wifi_event_bits = 0;

static TaskHandle_t wifi_time_task_handle = NULL;

static void wifi_time_task(void *param)
{
	(void)param;

	wifi_utils_get_current_date_time();

	// Mark not running and exit
	wifi_time_task_handle = NULL;
	vTaskDelete(NULL);
}

static void wifi_task(void *param)
{
    xWifiEventGroup = xEventGroupCreate();
    configASSERT(xWifiEventGroup);

    xWifiCanSleepSemaphore = xSemaphoreCreateBinary();
    configASSERT(xWifiCanSleepSemaphore);
    xWifiCycleSemaphore = xSemaphoreCreateBinary();
    configASSERT(xWifiCycleSemaphore);
    xWifiPingSemaphore = xSemaphoreCreateBinary();
    configASSERT(xWifiPingSemaphore);

    xWifiRawFramesMutex = xSemaphoreCreateMutex();
    configASSERT(xWifiRawFramesMutex);
    
    xWifiScanQueue = xQueueCreate(WIFI_MAX_NETWORKS, sizeof(wifi_scan_t));
    configASSERT(xWifiScanQueue);
    xWifiDeauthScanQueue = xQueueCreate(WIFI_MAX_NETWORKS, sizeof(wifi_scan_deauth_t));
    configASSERT(xWifiDeauthScanQueue);
    xWifiDeauthTargetQueue = xQueueCreate(1, sizeof(deauth_target_t));
    configASSERT(xWifiDeauthTargetQueue);
    xWifiDeauthStatsQueue = xQueueCreate(1, sizeof(deauth_stats_t));
    configASSERT(xWifiDeauthStatsQueue);
    xWifiSelectedNetworkQueue = xQueueCreate(1, sizeof(wifi_login_t));
    configASSERT(xWifiSelectedNetworkQueue);
    xWifiSniffQueue = xQueueCreate(1, sizeof(wifi_sniff_t));
    configASSERT(xWifiSniffQueue);
    xWifiBeaconQueue = xQueueCreate(1, sizeof(wifi_beacon_t));
    configASSERT(xWifiBeaconQueue);
    xWifiDataQueue = xQueueCreate(1, sizeof(wifi_data_t*));
    configASSERT(xWifiDataQueue);
    xWifiMqttCmdQueue = xQueueCreate(1, sizeof(wifi_mqtt_t));
    configASSERT(xWifiMqttCmdQueue);
    xWifiOtaPctQueue = xQueueCreate(1, sizeof(int));
    configASSERT(xWifiOtaPctQueue);
    xWifiPingQueue = xQueueCreate(1, sizeof(wifi_ping_t));
    configASSERT(xWifiPingQueue);
    xWifiAiRawSniffQueue = xQueueCreate(1, sizeof(char *));
    configASSERT(xWifiAiRawSniffQueue);

    xConnectionIconEventGroup = xEventGroupCreate();
    configASSERT(xConnectionIconEventGroup);
    xWiFiPortalEventGroup = xEventGroupCreate();
    configASSERT(xWiFiPortalEventGroup);
    
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

    // Initialize MQTT client
    wifi_utils_wifi_event_init();
    wifi_mqtt_client_init();

    // If Wi-Fi BTC portal password NVS doesn't exist yet, set it
    if (wifi_btc_pass_load_nvs(btc_wifi_portal_pass, sizeof(btc_wifi_portal_pass)) != ESP_OK) {
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
            btc_wifi_portal_pass[i] = alphabet[r % N];
        }
        btc_wifi_portal_pass[PASS_LEN] = '\0';
        
        // Save that version to NVS
        wifi_btc_pass_save_nvs(btc_wifi_portal_pass);
        
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGW(TAG, "Setting first time BTC Wi-Fi portal password: %s", btc_wifi_portal_pass);
#endif
    } else {
#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGI(TAG, "Using pre-set BTC Wi-Fi portal password: '%s'", btc_wifi_portal_pass);
#endif
    }

    // Let everything else initialize
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Get here without crashing -> This is a valid OTA app
    wifi_ota_update_mark_app_valid();

    /* Update NVS FW version */
    // If NVS version doesn't exist yet, set it
    char dummy[64];
    if (wifi_ota_update_get_nvs_version(dummy, sizeof(dummy)) != ESP_OK) {
        // Read the current app's version string from the embedded app descriptor
        const esp_app_desc_t *running = esp_app_get_description();
        const char *cur = running ? running->version : "";
        
        // Save that version to NVS
        wifi_ota_update_set_nvs_version(cur);
        
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Setting first time FW version: %s", cur);
#endif
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Using pre-set PolyCast5 FW version '%s'", dummy);
#endif
    }
    
    while (1) {
        // Check if need to cycle Wi-Fi radio (BT edge case)
        if (xSemaphoreTake(xWifiCycleSemaphore, 0) == pdTRUE) {
            esp_err_t err = wifi_utils_radio_cycle();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "wifi_utils_radio_cycle failed: %s", esp_err_to_name(err));
            }
        }
        
        // Specific network to connect selected
        if (xQueueReceive(xWifiSelectedNetworkQueue, &selected_network, 0) == pdTRUE) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "xWifiSelectedNetworkQueue received: ssid='%s', pass='%s'", selected_network.ssid, selected_network.password);
            ESP_LOGI(TAG, "xWifiSelectedNetworkQueue received: bssid='%02x:%02x:%02x:%02x:%02x:%02x'",
                    selected_network.bssid[0], selected_network.bssid[1], selected_network.bssid[2],
                    selected_network.bssid[3], selected_network.bssid[4], selected_network.bssid[5]);
#endif
            
            if (selected_network.prev && strlen(selected_network.ssid) == 0) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGW(TAG, "No previous network to connect to");
#endif
                
                xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTED_BIT | WIFI_CONNECTING_BIT); // Not connected / not connecting
            } else {
                xEventGroupSetBits(xWifiEventGroup, WIFI_CONNECTING_BIT); // Tell LCD we're trying
                
                ESP_ERROR_CHECK(wifi_utils_radio_start(selected_network.ssid, selected_network.bssid, selected_network.password));
                
                ESP_ERROR_CHECK(wifi_utils_connect());
            }
        }
        
        // Send data over MQTT
        if (xQueueReceive(xWifiMqttCmdQueue, &wifi_mqtt, 0) == pdTRUE) {
            // wifi_mqtt_client_publish() builds the topic safely
            wifi_mqtt_client_publish(wifi_mqtt.payload, wifi_mqtt.key);
        }
        
        // Received channel to sniff
        if (xQueueReceive(xWifiSniffQueue, &sniff_network, 0) == pdTRUE) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "xWifiSniffQueue received: mask='%d', channel='%u'", sniff_network.mask, sniff_network.channel);
            ESP_LOGI(TAG, "xWifiSniffQueue received: bssid='%02x:%02x:%02x:%02x:%02x:%02x'",
                    sniff_network.target_bssid[0], sniff_network.target_bssid[1], sniff_network.target_bssid[2],
                    sniff_network.target_bssid[3], sniff_network.target_bssid[4], sniff_network.target_bssid[5]);
#endif
            
            wifi_utils_init_promiscuous(&sniff_network);
        }

        // Received target to deauth
        if (xQueueReceive(xWifiDeauthTargetQueue, &deauth_target, 0) == pdTRUE) {
            esp_err_t err = wifi_deauth_send_for_duration(&deauth_target);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "xWifiDeauthTargetQueue: wifi_deauth_send_for_duration failed: %s", esp_err_to_name(err));
            }
        }

        // Ping the network
        if (xSemaphoreTake(xWifiPingSemaphore, 0) == pdTRUE) {
            // If not connected to Wi-Fi, skip pings
            if (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGW(TAG, "xWifiPingSemaphore: Skipping ping, not connected to Wi-Fi");
#endif
                continue;
            }

            // Initial pings
            wifi_ping_t wifi_ping = {0};

            // Ping the gateway
            esp_err_t err = wifi_ping_gateway(&wifi_ping.rtt_gateway);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Initial gateway ping failed: %s", esp_err_to_name(err));
            } else {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Initial gateway ping RTT: %d ms", wifi_ping.rtt_gateway);
#endif
            }

            // Ping a public DNS server
            err = wifi_ping_dns("8.8.8.8", &wifi_ping.rtt_dns); // Google Public DNS
            if (err == ESP_OK) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Ping 8.8.8.8: %ld ms", (long)wifi_ping.rtt_dns);
#endif
            } else {
                ESP_LOGW(TAG, "Ping to 8.8.8.8 failed : %s", esp_err_to_name(err));
            }

            // Send the ping results
            if (xQueueSend(xWifiPingQueue, &wifi_ping, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGE(TAG, "xWifiPingQueue: Failed to enqueue ping results");
            }
        }

        // Check for Wi-Fi events
        EventBits_t wifi_event_bits = xEventGroupGetBits(xWifiEventGroup);
        if (wifi_event_bits != last_wifi_event_bits) { // Only act on changes
            esp_err_t err;

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Received Wi-Fi event: %u", (unsigned int)wifi_event_bits);
#endif

            // If Wi-Fi reconnect bit transitioned 0 -> 1
            if ((wifi_event_bits & WIFI_RECONNECT_BIT) && !(last_wifi_event_bits & WIFI_RECONNECT_BIT)) {
                // Get previous network credentials
                selected_network = wifi_utils_get_prev();

                xEventGroupSetBits(xWifiEventGroup, WIFI_CONNECTING_BIT); // Tell LCD we're trying

                // Start radio and connect
                err = wifi_utils_radio_start(selected_network.ssid, selected_network.bssid, selected_network.password);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_RECONNECT_BIT: wifi_utils_radio_start failed: %s", esp_err_to_name(err));
                }
                err = wifi_utils_connect();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_RECONNECT_BIT: wifi_utils_connect failed: %s", esp_err_to_name(err));
                }
                xEventGroupClearBits(xWifiEventGroup, WIFI_RECONNECT_BIT); // Reset for next time
            }
            // If Wi-Fi disconnect bit transitioned 0 -> 1
            if ((wifi_event_bits & WIFI_DISCONNECT_BIT) && !(last_wifi_event_bits & WIFI_DISCONNECT_BIT)) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Disconnecting Wi-Fi...");
#endif

                err = wifi_utils_radio_stop();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_DISCONNECT_BIT: wifi_utils_radio_stop failed: %s", esp_err_to_name(err));
                }
                xEventGroupClearBits(xWifiEventGroup, WIFI_DISCONNECT_BIT); // Reset for next time
            }
            // If Wi-Fi scan networks bit transitioned 0 -> 1
            if ((wifi_event_bits & WIFI_SCAN_NETWORKS_BIT) && !(last_wifi_event_bits & WIFI_SCAN_NETWORKS_BIT)) {
                err = esp_wifi_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_SCAN_NETWORKS_BIT: esp_wifi_start failed: %s", esp_err_to_name(err));
                }
                err = wifi_utils_scan(wifi_scan);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_SCAN_NETWORKS_BIT: wifi_utils_scan failed: %s", esp_err_to_name(err));
                }
                err = wifi_utils_radio_stop();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_SCAN_NETWORKS_BIT: wifi_utils_radio_stop failed: %s", esp_err_to_name(err));
                }
                xEventGroupClearBits(xWifiEventGroup, WIFI_SCAN_NETWORKS_BIT); // Reset for next time
            }
            // If Wi-Fi scan networks for deauth bit transitioned 0 -> 1
            if ((wifi_event_bits & WIFI_SCAN_DEAUTH_BIT) && !(last_wifi_event_bits & WIFI_SCAN_DEAUTH_BIT)) {
                err = esp_wifi_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_SCAN_DEAUTH_BIT: esp_wifi_start failed: %s", esp_err_to_name(err));
                }
                err = wifi_deauth_scan(wifi_scan_deauth);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_SCAN_DEAUTH_BIT: wifi_deauth_scan failed: %s", esp_err_to_name(err));
                }
                err = wifi_utils_radio_stop();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "WIFI_SCAN_DEAUTH_BIT: wifi_utils_radio_stop failed: %s", esp_err_to_name(err));
                }
                xEventGroupClearBits(xWifiEventGroup, WIFI_SCAN_DEAUTH_BIT); // Reset for next time
            }
            // If Wi-Fi get date and time bit transitioned 0 -> 1
            if ((wifi_event_bits & WIFI_GET_DATE_TIME_BIT) && !(last_wifi_event_bits & WIFI_GET_DATE_TIME_BIT)) {
                // Run time sync in a dedicated task (TLS/HTTP client + JSON parsing stack can be deep)
                if (wifi_time_task_handle == NULL) {
                    if (xTaskCreate(wifi_time_task, "wifi_time_task", 1024 * 6, NULL,
                            POLYCAST5_PRIORITY_MEDIUM, &wifi_time_task_handle) != pdPASS) {
                        ESP_LOGE(TAG, "Failed to start wifi_time_task");
                        wifi_time_task_handle = NULL;
                    }
                }
                xEventGroupClearBits(xWifiEventGroup, WIFI_GET_DATE_TIME_BIT); // Reset for next time
            }

            last_wifi_event_bits = wifi_event_bits;
        }

        // Check for web portal events
        EventBits_t current_portal_bits = xEventGroupGetBits(xWiFiPortalEventGroup);
        if (current_portal_bits != last_portal_bits) { // Only act on changes
            esp_err_t err;

            // If AI web portal bit transitioned 0 -> 1
            if ((current_portal_bits & WIFI_PORTAL_AI_KEY_START_BIT) &&
                    !(last_portal_bits & WIFI_PORTAL_AI_KEY_START_BIT)) {
                err = ai_key_portal_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "ai_key_portal_start failed: %s", esp_err_to_name(err));
                }
            }
            // If AI web portal bit transitioned 1 -> 0
            if ((last_portal_bits & WIFI_PORTAL_AI_KEY_START_BIT) &&
                    !(current_portal_bits & WIFI_PORTAL_AI_KEY_START_BIT)) {
                err = ai_key_portal_stop();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "ai_key_portal_stop failed: %s", esp_err_to_name(err));
                }
            }

            // If AI packet analysis web portal bit transitioned 0 -> 1
            if ((current_portal_bits & WIFI_PORTAL_AI_PKT_ANALYSIS_START_BIT) &&
                    !(last_portal_bits & WIFI_PORTAL_AI_PKT_ANALYSIS_START_BIT)) {
                err = ai_analysis_portal_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "ai_analysis_portal_start failed: %s", esp_err_to_name(err));
                }
            }
            // If AI packet analysis web portal bit transitioned 1 -> 0
            if ((last_portal_bits & WIFI_PORTAL_AI_PKT_ANALYSIS_START_BIT) &&
                    !(current_portal_bits & WIFI_PORTAL_AI_PKT_ANALYSIS_START_BIT)) {
                err = ai_analysis_portal_stop();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "ai_analysis_portal_stop failed: %s", esp_err_to_name(err));
                }
            }

            // If BTC web portal bit transitioned 0 -> 1
            if ((current_portal_bits & WIFI_PORTAL_BTC_START_BIT) &&
                    !(last_portal_bits & WIFI_PORTAL_BTC_START_BIT)) {
                err = wifi_btc_portal_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "wifi_btc_portal_start failed: %s", esp_err_to_name(err));
                }
            }
            // If BTC web portal bit transitioned 1 -> 0
            if ((last_portal_bits & WIFI_PORTAL_BTC_START_BIT) &&
                    !(current_portal_bits & WIFI_PORTAL_BTC_START_BIT)) {
                err = wifi_btc_portal_stop();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "wifi_btc_portal_stop failed: %s", esp_err_to_name(err));
                }
            }
            
            // If BT web portal bit transitioned 0 -> 1
            if ((current_portal_bits & WIFI_PORTAL_BT_START_BIT) &&
                    !(last_portal_bits & WIFI_PORTAL_BT_START_BIT)) {
                err = bluetooth_web_portal_start();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "bluetooth_web_portal_start failed: %s", esp_err_to_name(err));
                }
            }
            // If BT web portal bit transitioned 1 -> 0
            if ((last_portal_bits & WIFI_PORTAL_BT_START_BIT) &&
                    !(current_portal_bits & WIFI_PORTAL_BT_START_BIT)) {
                err = bluetooth_web_portal_stop();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "bluetooth_web_portal_stop failed: %s", esp_err_to_name(err));
                }
            }

            last_portal_bits = current_portal_bits;
        }
    
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void wifi_task_create(void)
{
    if (xTaskCreate(wifi_task, "wifi_task", 1024 * 4, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start wifi_task");
    }
}
