#ifndef WIFI_PING_H
#define WIFI_PING_H

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int32_t rtt_gateway;
    int32_t rtt_dns;
} wifi_ping_t;

/**
 * @brief Ping the current gateway to get RTT
 *
 * @param [out] rtt_ms Round-trip time in milliseconds
 * 
 * @returns ESP error status
 */
esp_err_t wifi_ping_gateway(int32_t *rtt_ms);

/**
 * @brief Ping a given host to get RTT
 *
 * @param [in] host Hostname or IP address to ping
 * @param [out] rtt_ms Round-trip time in milliseconds
 * 
 * @returns ESP error status
 */
esp_err_t wifi_ping_dns(const char *host, int32_t *rtt_ms);

#endif // WIFI_PING_H