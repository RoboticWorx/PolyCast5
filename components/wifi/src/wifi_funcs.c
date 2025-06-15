#include "polycast5_macros.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_err.h"

#include "wifi_funcs.h"
#include "wifi_task.h"

#define TAG "WIFI_FUNCS"

esp_err_t wifi_funcs_scan(wifi_scan_t *wifi_scan)
{
    esp_err_t err;
    // Scan all SSIDs, all channels, include hidden networks
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0, // 0 = scan all channels
        .show_hidden = true
    };

    // Start scan (true = block until scan done)
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // How many APs were found
    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
        return err;
    }

    // Allocate array to hold results
    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_list) {
        ESP_LOGE(TAG, "malloc for ap_list failed");
        return ESP_ERR_NO_MEM;
    }

    // Pull the records
    err = esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        free(ap_list);
        return err;
    }
	
	#ifdef POLYCAST5_DEBUG
	    ESP_LOGI(TAG, "Found %d access point(s):", ap_num);
	    for (int i = 0; i < ap_num; i++) {
	        ESP_LOGI(TAG, "[%d] SSID: %-32s RSSI: %3d  CH: %2d  AUTH: %d",
	        		i, (char*)ap_list[i].ssid, ap_list[i].rssi, ap_list[i].primary, ap_list[i].authmode);
	    }
    #endif
    
    // Fill wifi_scan_t struct
    size_t count = MIN(ap_num, WIFI_MAX_NETWORKS);
	for (size_t i = 0; i < count; i++) {
	    // Copy the SSID
	    strlcpy((char*)wifi_scan[i].ssid, (char*)ap_list[i].ssid, sizeof(wifi_scan[i].ssid));
	
	    // Fill the rest
	    wifi_scan[i].rssi = ap_list[i].rssi;
	    wifi_scan[i].channel = ap_list[i].primary;
	    wifi_scan[i].auth = ap_list[i].authmode;
	
	    // Send to LCD
	    if (xQueueSend(xWifiScanQueue, &wifi_scan[i], portMAX_DELAY) != pdPASS) {
	        ESP_LOGE(TAG, "xWifiScanQueue: Failed to enqueue #%u", i);
	    }
	}

    free(ap_list);
    return ESP_OK;
}

esp_err_t wifi_funcs_connect(const char *ssid, const char *password)
{
    wifi_config_t cfg = {0};
    
    // Copy in SSID and password
    strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password));

	#ifdef POLYCAST5_DEBUG
    	ESP_LOGI(TAG, "Setting Wi-Fi config SSID='%s'", ssid);
    	ESP_LOGI(TAG, "Setting Wi-Fi config password='%s'", password);
    #endif
    
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
		return err;
	}

    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
    if (err != ESP_OK) {
		return err;
	}

	#ifdef POLYCAST5_DEBUG
    	ESP_LOGI(TAG, "Connecting to '%s' ...", ssid);
    #endif
    
    return esp_wifi_connect();
}


