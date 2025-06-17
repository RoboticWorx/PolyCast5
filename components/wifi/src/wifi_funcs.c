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

#define WIFI_CONNECTED_BIT (1 << 0)
#define WIFI_DISCONNECTED_BIT (1 << 1)

/* Helpers to pull type/subtype from the 802.11 frame control */
#define FC_TYPE(fc)    (((fc) & 0x0C) >> 2)
#define FC_SUBTYPE(fc) (((fc) & 0xF0) >> 4)
#define TYPE_MGMT      0x00
#define SUBTYPE_BEACON 0x08

static uint8_t target_bssid[6] = { 0x60, 0x55, 0xF9, 0xFC, 0xDE, 0xA8 };
static EventGroupHandle_t wifi_event_group;

bool wifi_connected = false;

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

static void wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
	static volatile uint32_t frames_seen = 0;
	frames_seen++;
	
	#ifdef POLYCAST5_DEBUG
		if (frames_seen % 100 == 0) { // Every 100 frames
		    ESP_LOGI(TAG, "sniffer: %u frames so far\r\n", frames_seen);
		}
	#endif

	// Make sure only management frames
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    
    // Points to the raw 802.11 frame bytes
    uint8_t *frame = pkt->payload;
    
    // frame_ctrl == first two bytes
    uint16_t frame_ctrl = (frame[1] << 8) | frame[0];
    
    // bits [3:2] give Type (00=Mgmt, 01=Control, 10=Data)
	// bits [7:4] give Subtype (1000=Beacon)
	// Check if management beacon frame
    if (FC_TYPE(frame_ctrl) != TYPE_MGMT || FC_SUBTYPE(frame_ctrl) != SUBTYPE_BEACON) {
        return;
    }
    
    // Addresses: bytes 4–9 = SA, 10–15 = DA, 16–21 = BSSID (for beacon)
    uint8_t *bssid = &frame[16];
    
    // Filter for target BSSID
    if (memcmp(bssid, target_bssid, 6) != 0) {
        return;
    }

    // After the 24-byte MAC header comes:
    // 8 bytes timestamp | 2 bytes interval | 2 bytes capability info
    uint8_t *fixed = frame + 24;
    uint64_t timestamp;
    uint16_t interval, cap_info;
    memcpy(&timestamp, fixed + 0, sizeof(timestamp));
    memcpy(&interval, fixed + 8, sizeof(interval));
    memcpy(&cap_info, fixed + 10, sizeof(cap_info));
    
    // Convert timestamp from µs to days (time since last reboot)
    uint64_t timestamp_seconds = timestamp / 1000000; 
    uint64_t timestamp_days = timestamp_seconds / (24 * 60 * 60); 
    
    uint8_t *ie = fixed + 12; // Information element (IE) starts 8 + 2 + 2 from 24
	int rem = pkt->rx_ctrl.sig_len - (ie - frame); // Remaining length of packet
	
	int channel = pkt->rx_ctrl.channel; // Works on both 2.4GHz and 5GHz
	bool has_rsn = false, has_wpa = false; // Flags to see if they exist
	char ssid[33] = {0}; // SSID buffer
	
	// Walk through bytes
	while (rem >= 2) {
	    uint8_t id = ie[0], len = ie[1]; // Tag ID and packet length
	    
	    // Make sure tag valid
	    if (len + 2 > rem) {
			break;
		}
		
	    uint8_t *data = ie + 2; // Data pointer
	
	    switch (id) {
	        case 0: // SSID
	        	if (len < sizeof(ssid)) {
					memcpy(ssid, data, len);
				}
				break;
				
	        case 3: // DS Parameter Set (2.4 GHz channel)
	        	if (channel == 0) {
					channel = data[0];
				}
				break;
				
	        case 48: // RSN (WPA2/WPA3) IE
	        	has_rsn = true;
				break;
				
	        case 221: // Catch-all for vendor-specific IEs
	            if (len >= 4 && data[0] == 0x00 && data[1] == 0x50 && data[2] == 0xF2 && data[3] == 0x01) {
					has_wpa = true;
				}
	            break;
	    }
	    
	    // Iterate pointer and remainder is less
	    ie += len + 2;
	    rem -= len + 2;
	}
	
	// Get frequency
	int freq_mhz = 0;
	if (channel >= 1  && channel <= 14) {
		freq_mhz = 2412 + 5 * (channel - 1);
	}
	else if (channel >= 36 && channel <= 165) {
		freq_mhz = 5000 + (5 * channel);
	}
	
	// Get SNR
	int snr_db = pkt->rx_ctrl.rssi - pkt->rx_ctrl.noise_floor;
	
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "SSID: %s | Channel: %d (%d MHz) | RSSI: %d dBm | SNR: %d dB | Encryption: %s | TS=%llu days | intvl=%u ms | cap=0x%04x",
		    ssid, channel, freq_mhz, pkt->rx_ctrl.rssi, snr_db,
		    has_rsn ? "WPA2/3" : has_wpa ? "WPA" : (cap_info & 0x10) ? "WEP" : "Open",
		    timestamp_days, interval, cap_info);
	#endif
	
	// Populate and send
	wifi_beacon_t beacon;
	strlcpy((char*)beacon.ssid, ssid, sizeof(beacon.ssid));
	beacon.channel = channel;
	beacon.freq = freq_mhz;
	beacon.rssi = pkt->rx_ctrl.rssi;
	beacon.snr = snr_db;
	beacon.rsn = has_rsn;
	beacon.wpa = has_wpa;
	beacon.cap_info = cap_info;
	beacon.interval = interval;
	beacon.timestamp = timestamp_seconds;
	xQueueSend(xWifiSnrQueue, &beacon, 0);
    
    /*
    Capability Information (cap_info): A 16-bit bitmask of the AP’s capabilities, defined by IEEE 802.11
    Example: cap=0x1431 -> binary 0001 0100 0011 0001
    
    | Bit | Name                | Value  | Set? | Meaning                                 |
	| --- | ------------------- | ------ | ---- | --------------------------------------- |
	| 0   | ESS                 | 1      | 1    | Infrastructure network (not IBSS).      |
	| 1   | IBSS                | 2      | 0    | Ad hoc mode (not set).                  |
	| 2   | CF‐Pollable         | 4      | 0    | Contention‐free polling (not set).      |
	| 3   | CF‐PollRequest      | 8      | 0    | Contention‐free request (not set).      |
	| 4   | Privacy             | 0x10   | 1    | WEP/WPA/WPA2 encryption supported.      |
	| 5   | Short Preamble      | 0x20   | 1    | Supports “short” preamble (faster RX).  |
	| 6   | PBCC                | 0x40   | 0    | Packet Binary Convolutional Code (no).  |
	| 7   | Channel Agility     | 0x80   | 0    | Dynamic channel switching (no).         |
	| 8   | Spectrum Management | 0x100  | 0    | 5 GHz regulatory features (no).         |
	| 9   | QoS AP              | 0x200  | 0    | Quality‐of‐Service AP (no).             |
	| 10  | Short Slot Time     | 0x400  | 1    | 9 µs slot instead of 20 µs.             |
	| 11  | APSD                | 0x800  | 0    | Automatic Power‐Save Delivery (no).     |
	| 12  | Radio Measurement   | 0x1000 | 1    | 802.11k measurement (survey) supported. |
	| 13  | DSSS‐OFDM           | 0x2000 | 0    | Mixed‐mode DSSS/OFDM (no).              |
	| 14  | Delayed Block Ack   | 0x4000 | 0    | (802.11e feature) (no).                 |
	| 15  | Immediate Block Ack | 0x8000 | 0    | (802.11e feature) (no).                 |
	
	So 0x1431 tells you your AP is:
		- ESS (infrastructure AP, not ad-hoc)
		- Privacy (it’s encrypting traffic)
		- Short Preamble (can use faster preamble)
		- Short Slot Time (9 µs slots for faster contention)
		- Radio Measurement (it supports 802.11k measurement features)
    */
}

void wifi_funcs_init_promiscuous(wifi_sniff_t *network)
{
	// Start up the radio
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Fix the channel to the target AP
    ESP_ERROR_CHECK(esp_wifi_set_channel(network->channel, WIFI_SECOND_CHAN_NONE));
    
    // Only management frames
	wifi_promiscuous_filter_t filter = {
		// 1 = WIFI_PROMIS_FILTER_MASK_MGMT
        .filter_mask = (network->mask == 1) ? WIFI_PROMIS_FILTER_MASK_MGMT : WIFI_PROMIS_FILTER_MASK_DATA
    };
    
    // Copy passed bssid into global
    memcpy(target_bssid, network->target_bssid, 6);
    
    // Filter for matching packets
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));

    // Register callback and enable promiscuous mode
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

	#ifdef POLYCAST5_DEBUG
	    ESP_LOGI(TAG, "Sniffer initialized; filtering beacon frames from %02x:%02x:%02x:%02x:%02x:%02x",
	         network->target_bssid[0], network->target_bssid[1],
	         network->target_bssid[2], network->target_bssid[3],
	         network->target_bssid[4], network->target_bssid[5]);
    #endif
}

