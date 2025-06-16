#include "polycast5_macros.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "esp_sntp.h"

#include "wifi_funcs.h"
#include "wifi_task.h"

#include "espnow_funcs.h"

#define TAG "WIFI_FUNCS"

#define WIFI_CONNECTED_BIT    (1 << 0)
#define WIFI_DISCONNECTED_BIT (1 << 1)

bool wifi_connected = false;

static EventGroupHandle_t wifi_event_group;

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
	        ESP_LOGI(TAG,
			    "[%d] SSID: %-32s BSSID: %02x:%02x:%02x:%02x:%02x:%02x RSSI: %3d  CH:%2d  AUTH:%d",
			     i,
			     (char*)ap_list[i].ssid,
			     ap_list[i].bssid[0], ap_list[i].bssid[1],
			     ap_list[i].bssid[2], ap_list[i].bssid[3],
			     ap_list[i].bssid[4], ap_list[i].bssid[5],
			     ap_list[i].rssi,
			     ap_list[i].primary,
			     ap_list[i].authmode
			);
	    }
    #endif
    
    // Fill wifi_scan_t struct
    size_t count = MIN(ap_num, WIFI_MAX_NETWORKS);
	for (size_t i = 0; i < count; i++) {
	    // Copy the SSID
	    strlcpy((char*)wifi_scan[i].ssid, (char*)ap_list[i].ssid, sizeof(wifi_scan[i].ssid));
	    
	    // Copy the BSSID
	    memcpy(wifi_scan[i].bssid, ap_list[i].bssid, sizeof(ap_list[i].bssid));
	
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

void wifi_funcs_get_current_date_time(void)
{
	static bool initialized = false;
	
	if (!initialized) {
		// Tell SNTP client to poll for time
	    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
	
	    // Point STNP client at a given server
	    esp_sntp_setservername(0, "pool.ntp.org"); // or time.nist.gov
	
		// Init and start SNTP service
	    esp_sntp_init();
	    
	    initialized = true;
	}
	    
    time_t now = 0;
    struct tm timeinfo = {0};

    // Wait until the SNTP task clock has gone past 2025
    while (timeinfo.tm_year < (2025 - 1900)) {
        vTaskDelay(pdMS_TO_TICKS(100));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    char strftime_buf[64];
    
    // Configure the timezone environment
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
	tzset();
    // • Standard time = UTC–5 (“EST”)
    // • Daylight time = UTC–4 (“EDT”)
    // • DST starts 2nd Sunday in March at 2 AM
    // • DST ends 1st Sunday in November at 2 AM
    // `tzset()` makes the library re-read that TZ rule now.

	// Get the epoch time
    time(&now);
    
    // Convert to a local broken-out form
    localtime_r(&now, &timeinfo);
    // `time()` returns seconds since Jan 1 1970 UTC,
    // `localtime_r()` applies your TZ rules into a `struct tm`.

    // Render it as “YYYY-MM-DD HH:MM:SS” into our buffer
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

	#ifdef POLYCAST5_DEBUG
    	ESP_LOGI(TAG, "Current date/time 24h: %s", strftime_buf);
    #endif
    
    // 12-hour format with AM/PM:
	strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %I:%M:%S %p", &timeinfo);
	
	#ifdef POLYCAST5_DEBUG
    	ESP_LOGI(TAG, "Current date/time 12h: %s", strftime_buf);
    #endif
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
	// Disconnection event
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)data;
        
        #ifdef POLYCAST5_DEBUG
        	ESP_LOGW(TAG, "Disconnected, reason=%d", d->reason);
        #endif
        
        wifi_funcs_radio_stop();

        xEventGroupSetBits(wifi_event_group, WIFI_DISCONNECTED_BIT);
    }
    // Connected event
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        
        #ifdef POLYCAST5_DEBUG
        	ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        #endif
        
        xSemaphoreGive(xWifiNetworkConnectedSemaphore); // Notify LCD we connected

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        
		wifi_funcs_get_current_date_time();
		
		wifi_connected = true;
    }
}

void wifi_funcs_wifi_event_init(void)
{
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
    			 NULL, NULL));
    			 
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
    			 NULL, NULL));
}

static bool wait_for_connection(TickType_t timeout)
{
    // Wait for either bit
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT, pdTRUE,
     			pdFALSE, timeout);

	// Got IP
    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }
    
    // Disconnected
    if (bits & WIFI_DISCONNECTED_BIT) {
        return false;
    }
    
    // Timed out
    return false;
}

esp_err_t wifi_funcs_connect(void)
{
	xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
	
    esp_err_t err = esp_wifi_connect();
    
    // Check connetion
    if (wait_for_connection(pdMS_TO_TICKS(15000))) {
		#ifdef POLYCAST5_DEBUG
    		ESP_LOGI(TAG, "Wi-Fi connected and got IP!");
    	#endif
	}
	else {
	    ESP_LOGE(TAG, "Failed to connect");
	    // Notify LCD
	    wifi_funcs_radio_stop();
	}
	
	return err;
}

esp_err_t wifi_funcs_radio_start(const char *ssid, const uint8_t* bssid, const char *password)
{
	wifi_config_t cfg = {0};
    
    // Copy in SSID and password
    strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password));
    
    // Copy BSSID
    //cfg.sta.bssid_set = true;
	//memcpy(cfg.sta.bssid, bssid, sizeof(cfg.sta.bssid));
	
	cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // Weakest auth mode to accept in the fast scan mode 

	#ifdef POLYCAST5_DEBUG
    	ESP_LOGI(TAG, "Setting Wi-Fi config SSID='%s'", ssid);
    	ESP_LOGI(TAG, "Setting Wi-Fi config BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
		     bssid[0], bssid[1], bssid[2],
		     bssid[3], bssid[4], bssid[5]);
    	ESP_LOGI(TAG, "Setting Wi-Fi config password='%s'", password);
    #endif
    
    // Set mode
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
		return err;
	}
	
	// Set config
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &cfg);
    if (err != ESP_OK) {
		return err;
	}
	
	// Start the driver
    err = esp_wifi_start();
    
    return err;
}

esp_err_t wifi_funcs_radio_stop(void)
{
	esp_wifi_disconnect(); // Disconnect if connected
	
	// Stop Wi-Fi
    if (esp_wifi_stop() == ESP_OK) {
		xSemaphoreGive(xWifiNetworkDisconnectedSemaphore);
		wifi_connected = false;
		return ESP_OK;
	}
    
    ESP_LOGE(TAG, "wifi_funcs_radio_stop not ESP_OK");
    
    wifi_connected = false;
    
    return ESP_FAIL;
}

wifi_login_t wifi_funcs_get_prev(void)
{
	wifi_config_t current;
	ESP_ERROR_CHECK(esp_wifi_get_config(ESP_IF_WIFI_STA, &current));
	
	wifi_login_t prev;
	strlcpy(prev.ssid, (char*)current.sta.ssid, sizeof(current.sta.ssid));
	strlcpy(prev.password, (char*)current.sta.password, sizeof(current.sta.password));
	
	return prev;
}


