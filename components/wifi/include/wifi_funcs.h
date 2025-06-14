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

extern wifi_scan_t wifi_scan[WIFI_MAX_NETWORKS];

/**
 * @brief Scan and print available networks
 *
 * @param [in] wifi_scan Wi-Fi scan structure
 *
 * @return ESP_ERR status
 */
esp_err_t wifi_funcs_wifi_scan(wifi_scan_t* wifi_scan);


#endif // WIFI_FUNCS_H