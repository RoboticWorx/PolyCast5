#ifndef WIFI_AUTOCONNECT_H
#define WIFI_AUTOCONNECT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <esp_err.h>

#include "wifi_utils.h"

#define WIFI_AUTOCONNECT_MAX_KNOWN 20 // Saved-network cap; a full list never evicts

/**
 * @brief Picks the last known network or if not available the network with the strongest RSSI from scan results
 *
 * @param [out] out Wi-Fi network to connect to (password preserved if known)
 */
esp_err_t wifi_autoconnect_pick_known_network(wifi_login_t *out);

/**
 * @brief Remembers the currently connected network in the known networks list
 *
 * @returns ESP_OK if saved or updated, ESP_ERR_NO_MEM if the list is full (not saved), else an error
 */
esp_err_t wifi_autoconnect_remember_current_network(void);

/**
 * @brief Takes a pending saved-list-full notice and clears WIFI_SAVE_LIST_FULL_BIT
 *
 * @param [out] ssid_out Destination for the SSID the full list turned away
 * @param [in] len Size of the destination buffer
 *
 * @returns true if a notice was pending, false otherwise
 */
bool wifi_autoconnect_take_list_full(char *ssid_out, size_t len);

/**
 * @brief Drops a pending saved-list-full notice (a new join supersedes it)
 */
void wifi_autoconnect_clear_list_full(void);

/**
 * @brief Loads persisted known networks from NVS (safe to call multiple times)
 */
void wifi_autoconnect_init(void);

/**
 * @brief Number of saved (known) Wi-Fi networks
 *
 * @returns Count of known networks currently stored
 */
size_t wifi_autoconnect_get_known_count(void);

/**
 * @brief Copies the SSID of the i-th known network into a buffer
 *
 * @param [in] i Index of the known network
 * @param [out] out Destination buffer for the SSID
 * @param [in] out_size Size of the destination buffer
 *
 * @returns true on success, false if the index is out of range or args are invalid
 */
bool wifi_autoconnect_get_known_ssid(size_t i, char *out, size_t out_size);

/**
 * @brief Looks up a saved network by SSID and copies its stored record
 *
 * @param [in] ssid SSID to look up
 * @param [out] out Destination for the stored record (SSID, BSSID, password, locked)
 *
 * @returns true if found, false otherwise
 */
bool wifi_autoconnect_get_known_info(const char *ssid, wifi_login_t *out);

/**
 * @brief Reports whether an SSID is in the saved (known) networks list
 *
 * @param [in] ssid SSID to check
 *
 * @returns true if the network is saved, false otherwise
 */
bool wifi_autoconnect_is_known(const char *ssid);

/**
 * @brief Forgets a saved network by SSID: removes it from the known networks list
 *        (and NVS), clears it from esp_wifi's remembered STA config if it matches,
 *        and clears the cached last pick so autoconnect won't reconnect to it
 *
 * @param [in] ssid SSID of the network to forget
 *
 * @returns ESP_OK if removed, ESP_ERR_NOT_FOUND if not in the list, else an error
 */
esp_err_t wifi_autoconnect_forget_network(const char *ssid);

#endif // WIFI_AUTOCONNECT_H