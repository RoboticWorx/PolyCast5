#ifndef WIFI_FUNCS_H
#define WIFI_FUNCS_H

#include <stdbool.h>

#include "esp_err.h"

#define WIFI_MAX_NETWORKS 20

#define MAX_MAC_CLIENTS 100

typedef struct {
    uint8_t ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;
} wifi_scan_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    char password[33];
    bool locked; // If network requires a password
    bool prev; // If connecting to the last known network
} wifi_login_t;

typedef struct {
	uint8_t type;
    uint8_t channel;
    int mask;
    uint8_t target_bssid[6];
} wifi_sniff_t;

typedef struct {
	char ssid[33];
    int channel;
    int freq;
    int rssi;
    int snr;
    bool rsn;
    bool wpa;
    uint16_t cap_info;
    uint16_t interval;
    uint64_t timestamp;
} wifi_beacon_t;

typedef struct {
    uint8_t mac[6];
    int8_t  rssi;
    uint32_t last_seen; // In ticks
} wifi_data_clients_t;

typedef struct {
    wifi_data_clients_t clients[MAX_MAC_CLIENTS];
    uint32_t client_count;
    uint32_t rate;
    uint32_t channel;
} wifi_data_t;

//extern wifi_scan_t wifi_scan[WIFI_MAX_NETWORKS];

/**
 * @brief Scan and print available networks
 *
 * @param [in] wifi_scan Wi-Fi scan structure
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_scan(wifi_scan_t *wifi_scan);

/**
 * @brief Connect to a given Wi-Fi network
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_connect(void);

/**
 * @brief Configure and start the radio to join a given network
 *
 * @param [in] ssid Network SSID
 * @param [in] bssid Network BSSID
 * @param [in] password Network password
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_radio_start(const char *ssid, const uint8_t* bssid, const char *password);

void wifi_funcs_wifi_event_init(void);
esp_err_t wifi_funcs_radio_stop(void);
void wifi_funcs_get_current_date_time(void);
wifi_login_t wifi_funcs_get_prev(void);
void wifi_funcs_init_promiscuous(wifi_sniff_t *network);


#endif // WIFI_FUNCS_H