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

/**
 * @brief Start a continuous ping to the gateway as a CSI sounding source
 *
 * CSI only exists on packet reception, so sensing needs a steady stream of frames from a
 * transmitter that does not move. Replies from the associated AP are exactly that: fixed
 * position, fixed MAC, one channel. Unlike wifi_ping_gateway() this session never ends and
 * reports nothing, it exists purely to keep frames arriving.
 *
 * @param [in] interval_ms Gap between requests. Must be a multiple of the 10 ms FreeRTOS tick,
 *                         anything finer is silently rounded. 20 ms gives roughly 50 Hz.
 *
 * @returns ESP error status, ESP_FAIL if the gateway is not known yet
 */
esp_err_t wifi_ping_sound_start(uint32_t interval_ms);

/**
 * @brief Stop the sounding ping, safe to call when it is not running
 */
void wifi_ping_sound_stop(void);

#endif // WIFI_PING_H