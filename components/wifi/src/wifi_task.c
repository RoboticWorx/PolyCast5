#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "espnow_task.h"
#include "espnow_funcs.h"

#define TAG "WIFI_TASK"

static wifi_scan_t wifi_scan[WIFI_MAX_NETWORKS];
static wifi_login_t selected_network;

QueueHandle_t xWifiScanQueue;
QueueHandle_t xWifiSelectedNetworkQueue;

SemaphoreHandle_t xWifiStartScanSemaphore;
SemaphoreHandle_t xWifiNetworkConnectedSemaphore;
SemaphoreHandle_t xWifiNetworkDisconnectedSemaphore;
SemaphoreHandle_t xWifiDisconnectSemaphore;
SemaphoreHandle_t xWifiConnectingSemaphore;

static void wifi_task(void *param)
{
	xWifiStartScanSemaphore = xSemaphoreCreateBinary();
	xWifiNetworkConnectedSemaphore = xSemaphoreCreateBinary();
	xWifiNetworkDisconnectedSemaphore = xSemaphoreCreateBinary();
	xWifiDisconnectSemaphore = xSemaphoreCreateBinary();
	xWifiConnectingSemaphore = xSemaphoreCreateBinary();
	
	xWifiScanQueue = xQueueCreate(WIFI_MAX_NETWORKS, sizeof(wifi_scan_t));
	xWifiSelectedNetworkQueue = xQueueCreate(1, sizeof(wifi_login_t));
	
	wifi_funcs_wifi_event_init();
	
	/*wifi_config_t current;
	ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &current));
	ESP_LOGI(TAG, "At boot config SSID='%s', pass='%s'", current.sta.ssid, current.sta.password);*/
    
	while (1) {
		if (xSemaphoreTake(xWifiStartScanSemaphore, 0) == pdTRUE) {
			ESP_ERROR_CHECK(esp_wifi_start());
			
			wifi_funcs_scan(wifi_scan);
			
			wifi_funcs_radio_stop();
		}
		
		if (xSemaphoreTake(xWifiDisconnectSemaphore, 0) == pdTRUE) {			
			wifi_funcs_radio_stop();
		}
		
		if (xQueueReceive(xWifiSelectedNetworkQueue, &selected_network, 0) == pdTRUE) {
			#ifdef POLYCAST5_DEBUG
				ESP_LOGI(TAG, "xWifiSelectedNetworkQueue received: ssid='%s', pass='%s'", selected_network.ssid, selected_network.password);
				ESP_LOGI(TAG, "xWifiSelectedNetworkQueue received: bssid='%02x:%02x:%02x:%02x:%02x:%02x'",
				    selected_network.bssid[0], selected_network.bssid[1], selected_network.bssid[2],
				    selected_network.bssid[3], selected_network.bssid[4], selected_network.bssid[5]);
			#endif
			
			xQueueReset(xWifiNetworkDisconnectedSemaphore); // Reset previous gives
			xSemaphoreGive(xWifiConnectingSemaphore); // Tell LCD we're trying
			
			ESP_ERROR_CHECK(wifi_funcs_radio_start(selected_network.ssid, selected_network.bssid, selected_network.password));
						
			ESP_ERROR_CHECK(wifi_funcs_connect());
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
