#ifndef WIFI_FUNCS_H
#define WIFI_FUNCS_H

#include "esp_err.h"

#define WIFI_MAX_NETWORKS 20

typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;
} wifi_scan_t;

typedef struct {
    uint8_t ssid[33];
    char password[33];
    bool locked;
} wifi_login_t;

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
 * @param [in] ssid Network SSID
 * @param [in] password Network password
 *
 * @return ESP_ERR
 */
esp_err_t wifi_funcs_connect(const char *ssid, const char *password);


#endif // WIFI_FUNCS_H