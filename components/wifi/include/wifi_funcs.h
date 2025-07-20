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
    char password[65];
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
} wifi_data_clients_t;

typedef struct {
    wifi_data_clients_t clients[MAX_MAC_CLIENTS];
    uint32_t client_count;
    uint32_t rate;
    uint32_t channel;
} wifi_data_t;

typedef struct {
    uint8_t key[16];
    char payload[4];
} wifi_mqtt_t;

/**
 * @brief Gets previous Wi-Fi config from NVS
 *
 * @returns Wi-Fi login information
 */
wifi_login_t wifi_funcs_get_prev(void);

/**
 * @brief Scan and print available networks
 *
 * @param [in] wifi_scan Wi-Fi scan structure
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_scan(wifi_scan_t *wifi_scan);

/**
 * @brief Connect to a given Wi-Fi network
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_connect(void);

/**
 * @brief Configure and start the radio to join a given network
 *
 * @param [in] ssid Network SSID
 * @param [in] bssid Network BSSID
 * @param [in] password Network password
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_radio_start(const char *ssid, const uint8_t* bssid, const char *password);

/**
 * @brief Disconnects from MQTT and Wi-Fi, then stops Wi-Fi
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_radio_stop(void);

/**
 * @brief Creates MQTT ESP event group
 */
void wifi_funcs_wifi_event_init(void);

/**
 * @brief Initializes MQTT client
 */
void wifi_funcs_mqtt_client_init(void);\

/**
 * @brief Initializes Wi-Fi promiscuous mode to sniff packets
 *
 * @param [in] network Network to sniff
 */
void wifi_funcs_init_promiscuous(wifi_sniff_t *network);

/**
 * @brief Gets the current date and time from pool.ntp
 */
void wifi_funcs_get_current_date_time(void);

/**
 * @brief Sends data via MQTT to receiver
 *
 * @param [in] payload Data to send
 * @param [in] key Unique topic key to filter
 */
void wifi_funcs_mqtt_client_publish(char *payload, const uint8_t key[16]);

#endif // WIFI_FUNCS_H