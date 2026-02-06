#ifndef WIFI_AUTOCONNECT_H
#define WIFI_AUTOCONNECT_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "wifi_utils.h"

/**
 * @brief Picks the last known network or if not available the network with the strongest RSSI from scan results
 *
 * @param [out] out Wi-Fi network to connect to (password preserved if known)
 */
esp_err_t wifi_autoconnect_pick_known_network(wifi_login_t *out);

/**
 * @brief Remembers the currently connected network in the known networks list
 */
void wifi_autoconnect_remember_current_network(void);

/**
 * @brief Loads persisted known networks from NVS (safe to call multiple times)
 */
void wifi_autoconnect_init(void);

#endif // WIFI_AUTOCONNECT_H