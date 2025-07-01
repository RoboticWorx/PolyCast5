#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "espnow_task.h"
#include "espnow_funcs.h"
#include "gpio_task.h"

#define TAG "WIFI_TASK"

extern bool wifi_connected;

static wifi_scan_t wifi_scan[WIFI_MAX_NETWORKS];
static wifi_login_t selected_network;
static wifi_sniff_t sniff_network;

static wifi_mqtt_t wifi_mqtt;

QueueHandle_t xWifiScanQueue;
QueueHandle_t xWifiSelectedNetworkQueue;
QueueHandle_t xWifiSniffQueue;
QueueHandle_t xWifiBeaconQueue;
QueueHandle_t xWifiDataQueue;
QueueHandle_t xWifiMqttCmdQueue;

SemaphoreHandle_t xWifiStartScanSemaphore;
SemaphoreHandle_t xWifiNetworkConnectedSemaphore;
SemaphoreHandle_t xWifiNetworkDisconnectedSemaphore;
SemaphoreHandle_t xWifiDisconnectSemaphore;
SemaphoreHandle_t xWifiConnectingSemaphore;
SemaphoreHandle_t xWifiReconnectSemaphore;
SemaphoreHandle_t xWifiCanSleepSemaphore;
SemaphoreHandle_t xWifiMqttSuccessSemaphore;
SemaphoreHandle_t xWifiMqttConnectedSemaphore;
SemaphoreHandle_t xWifiMqttDisconnectedSemaphore;

static void wifi_task(void *param)
{
	xWifiStartScanSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiStartScanSemaphore);
	xWifiNetworkConnectedSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiNetworkConnectedSemaphore);
	xWifiNetworkDisconnectedSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiNetworkDisconnectedSemaphore);
	xWifiDisconnectSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiDisconnectSemaphore);
	xWifiConnectingSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiConnectingSemaphore);
	xWifiReconnectSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiReconnectSemaphore);
	xWifiCanSleepSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiCanSleepSemaphore);
	xWifiMqttSuccessSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiMqttSuccessSemaphore);
	xWifiMqttConnectedSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiMqttConnectedSemaphore);
	xWifiMqttDisconnectedSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiMqttDisconnectedSemaphore);
	
	xWifiScanQueue = xQueueCreate(WIFI_MAX_NETWORKS, sizeof(wifi_scan_t));
	configASSERT(xWifiScanQueue);
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
	
	uint8_t my_mac[6];
	esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
		
	wifi_funcs_wifi_event_init();
	wifi_funcs_mqtt_client_init();
	
	while (1) {
		// Start a Wi-Fi scan
		if (xSemaphoreTake(xWifiStartScanSemaphore, 0) == pdTRUE) {
			ESP_ERROR_CHECK(esp_wifi_start());
			
			wifi_funcs_scan(wifi_scan);
			
			wifi_funcs_radio_stop();
		}
		
		// Disconnect from Wi-Fi
		if (xSemaphoreTake(xWifiDisconnectSemaphore, 0) == pdTRUE) {			
			wifi_funcs_radio_stop();
		}
		
		// Specific network to connect selected
		if (xQueueReceive(xWifiSelectedNetworkQueue, &selected_network, 0) == pdTRUE) {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "xWifiSelectedNetworkQueue received: ssid='%s', pass='%s'", selected_network.ssid, selected_network.password);
				ESP_LOGI(TAG, "xWifiSelectedNetworkQueue received: bssid='%02x:%02x:%02x:%02x:%02x:%02x'",
				    selected_network.bssid[0], selected_network.bssid[1], selected_network.bssid[2],
				    selected_network.bssid[3], selected_network.bssid[4], selected_network.bssid[5]);
			#endif
			
			xQueueReset(xWifiNetworkDisconnectedSemaphore); // Reset previous gives
			
			if (selected_network.prev && strlen(selected_network.ssid) == 0) {
				#ifdef POLYCAST5_DEBUG
				    ESP_LOGW(TAG, "No previous network to connect to");
			    #endif
			    
			    xSemaphoreGive(xWifiNetworkDisconnectedSemaphore);
			}
			else {
				xSemaphoreGive(xWifiConnectingSemaphore); // Tell LCD we're trying
				
				ESP_ERROR_CHECK(wifi_funcs_radio_start(selected_network.ssid, selected_network.bssid, selected_network.password));
				
				ESP_ERROR_CHECK(wifi_funcs_connect());
			}
		}
		
		// Reconnect to last known network
		if (xSemaphoreTake(xWifiReconnectSemaphore, 0) == pdTRUE) {
			xQueueReset(xWifiNetworkDisconnectedSemaphore); // Reset previous gives
			
			xSemaphoreGive(xWifiConnectingSemaphore); // Tell LCD we're trying
			
			ESP_ERROR_CHECK(wifi_funcs_radio_start(selected_network.ssid, selected_network.bssid, selected_network.password));
							
			ESP_ERROR_CHECK(wifi_funcs_connect());
		}
		
		// Wi-Fi is actively connected
		if (wifi_connected) {
			//wifi_funcs_get_current_date_time();
		}
		
		// Send data over MQTT
		if (xQueueReceive(xWifiMqttCmdQueue, &wifi_mqtt, 0) == pdTRUE) {
			// Build the topic string
		    char topic[128];
		    int len = snprintf(
			    topic, sizeof(topic),
			    "polycast5/%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X/cmd",
			    wifi_mqtt.key[0], wifi_mqtt.key[1], wifi_mqtt.key[2], wifi_mqtt.key[3],
			    wifi_mqtt.key[4], wifi_mqtt.key[5], wifi_mqtt.key[6], wifi_mqtt.key[7],
			    wifi_mqtt.key[8], wifi_mqtt.key[9], wifi_mqtt.key[10], wifi_mqtt.key[11],
			    wifi_mqtt.key[12], wifi_mqtt.key[13], wifi_mqtt.key[14], wifi_mqtt.key[15]
			);
			strlcpy(topic, topic, len + 1);
	         
			wifi_funcs_mqtt_client_publish(wifi_mqtt.payload, wifi_mqtt.key);
		}
		
		// Received channel to sniff
		if (xQueueReceive(xWifiSniffQueue, &sniff_network, 0) == pdTRUE) {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "xWifiSniffQueue received: mask='%d', channel='%u'", sniff_network.mask, sniff_network.channel);
				ESP_LOGI(TAG, "xWifiSniffQueue received: bssid='%02x:%02x:%02x:%02x:%02x:%02x'",
				    sniff_network.target_bssid[0], sniff_network.target_bssid[1], sniff_network.target_bssid[2],
				    sniff_network.target_bssid[3], sniff_network.target_bssid[4], sniff_network.target_bssid[5]);
			#endif
			
			wifi_funcs_init_promiscuous(&sniff_network);
		}
    
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

void wifi_task_create(void)
{
    if (xTaskCreate(wifi_task, "wifi_task", 1024 * 3, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start wifi_task");
	}
}
