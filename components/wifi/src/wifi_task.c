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

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "ota_update.h"
#include "esp_app_desc.h"
#include "btc_web_portal.h"

#define TAG "WIFI_TASK"

extern bool wifi_connected;

char btc_wifi_portal_pass[64];

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
QueueHandle_t xWifiPingQueue;

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
SemaphoreHandle_t xWifiCycleSemaphore;
SemaphoreHandle_t xWifiPingSemaphore;

EventGroupHandle_t xConnectionIconEventGroup;

// OTA
SemaphoreHandle_t xWifiOtaAvailableSemaphore; // Wi-Fi -> LCD
QueueHandle_t xWifiOtaPctQueue;

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
	xWifiOtaAvailableSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiOtaAvailableSemaphore);
	xWifiCycleSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiCycleSemaphore);
	xWifiPingSemaphore = xSemaphoreCreateBinary();
	configASSERT(xWifiPingSemaphore);
	
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
	xWifiOtaPctQueue = xQueueCreate(1, sizeof(int));
	configASSERT(xWifiOtaPctQueue);
	xWifiPingQueue = xQueueCreate(1, sizeof(wifi_ping_t));
	configASSERT(xWifiPingQueue);

	xConnectionIconEventGroup = xEventGroupCreate();
	configASSERT(xConnectionIconEventGroup);
	
	uint8_t my_mac[6];
	esp_read_mac(my_mac, ESP_MAC_WIFI_STA);

	// Initialize MQTT client
	wifi_funcs_wifi_event_init();
	wifi_funcs_mqtt_client_init();

	// If Wi-Fi BTC portal password NVS doesn't exist yet, set it
	if (btc_wifi_pass_load_nvs(btc_wifi_portal_pass, sizeof(btc_wifi_portal_pass)) != ESP_OK) {
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
		btc_wifi_pass_save_nvs(btc_wifi_portal_pass);
		
		#ifdef POLYCAST5_PASS_DEBUG
		ESP_LOGW(TAG, "Setting first time BT Wi-Fi portal password: %s", bt_wifi_portal_pass);
		#endif
	}
	else {
		#ifdef POLYCAST5_PASS_DEBUG
		ESP_LOGI(TAG, "Using pre-set BT Wi-Fi portal password: '%s'", bt_wifi_portal_pass);
		#endif
	}

	// Let everything else initialize
	vTaskDelay(pdMS_TO_TICKS(2000));

	// Get here without crashing -> This is a valid OTA app
	ota_update_mark_app_valid();

	/* Update NVS FW version */
	// If NVS version doesn't exist yet, set it
	char dummy[64];
	if (ota_update_get_nvs_version(dummy, sizeof(dummy)) != ESP_OK) {
		// Read the current app's version string from the embedded app descriptor
		const esp_app_desc_t *running = esp_app_get_description();
		const char *cur = running ? running->version : "";
		
		// Save that version to NVS
		ota_update_set_nvs_version(cur);
		
		#ifdef POLYCAST5_DEBUG
		ESP_LOGW(TAG, "Setting first time FW version: %s", cur);
		#endif
	}
	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Using pre-set PolyCast5 FW version '%s'", dummy);
		#endif
	}
	
	while (1) {
		// Check if need to cycle Wi-Fi radio (BT edge case)
		if (xSemaphoreTake(xWifiCycleSemaphore, 0) == pdTRUE) {
			esp_err_t err = wifi_funcs_radio_cycle();
			if (err != ESP_OK) {
				ESP_LOGW(TAG, "wifi_funcs_radio_cycle failed: %s", esp_err_to_name(err));
			}
		}
		
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

		// Ping the network
		if (xSemaphoreTake(xWifiPingSemaphore, 0) == pdTRUE) {
			if (!wifi_connected) {
				#ifdef POLYCAST5_DEBUG
				ESP_LOGW(TAG, "xWifiPingSemaphore: Skipping ping, not connected to Wi-Fi");
				#endif
				continue;
			}

			// Initial pings
			wifi_ping_t wifi_ping = {0};

			// Ping the gateway
			esp_err_t err = wifi_funcs_ping_gateway(&wifi_ping.rtt_gateway);
			if (err != ESP_OK) {
				ESP_LOGW(TAG, "Initial gateway ping failed: %s", esp_err_to_name(err));
			}
			else {
				#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "Initial gateway ping RTT: %d ms", wifi_ping.rtt_gateway);
				#endif
			}

			// Ping a public DNS server
			err = wifi_funcs_ping("8.8.8.8", &wifi_ping.rtt_dns); // Google Public DNS
			if (err == ESP_OK) {
				#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "Ping 8.8.8.8: %ld ms", (long)wifi_ping.rtt_dns);
				#endif
			}
			else {
				ESP_LOGW(TAG, "Ping to 8.8.8.8 failed : %s", esp_err_to_name(err));
			}

			// Send the ping results
			if (xQueueSend(xWifiPingQueue, &wifi_ping, pdMS_TO_TICKS(1000)) != pdTRUE) {
				ESP_LOGE(TAG, "xWifiPingQueue: Failed to enqueue ping results");
			}
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
