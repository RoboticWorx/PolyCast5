#ifndef WIFI_BTC_PORTAL_H
#define WIFI_BTC_PORTAL_H

#include "esp_err.h"

/** 
 * @brief Starts BTC web portal
 *
 * @returns ESP error status
 */
esp_err_t wifi_btc_portal_start(void);

/** 
 * @brief Stops BTC web portal
 *
 * @returns ESP error status
 */
esp_err_t wifi_btc_portal_stop(void);

/** 
 * @brief Get BTC web portal SSID
 */
const char *wifi_btc_portal_get_ssid(void);

/** 
 * @brief Get BTC web portal password
 */
const char *wifi_btc_portal_get_pass(void);

/** 
 * @brief Get BTC web portal IP address
 */
const char *wifi_btc_portal_get_ip(void);

/** 
 * @brief Save BTC public address to NVS
 *
 * @param [in] addr BTC public address to save
 *
 * @returns ESP error status
 */
esp_err_t wifi_btc_addr_set_nvs(const char *addr);

/** 
 * @brief Get BTC public address from NVS
 *
 * @param [out] addr_out Buffer to save BTC public address to
 * @param [out] sz Size of BTC public address buffer
 *
 * @returns ESP error status
 */
esp_err_t wifi_btc_addr_get_nvs(char *addr_out, size_t sz);

/** 
 * @brief Save a randomly generated BTC Wi-Fi password to NVS
 *
 * @param [in] val Password string to save
 *
 * @returns ESP error status
 */
esp_err_t wifi_btc_pass_save_nvs(const char *val);

/** 
 * @brief Load the randomly generated BTC Wi-Fi password from NVS
 *
 * @param [out] out Pointer to the string to load the password in to
 * @param [out] out_sz Size of password
 *
 * @returns ESP error status
 */
esp_err_t wifi_btc_pass_load_nvs(char *out, size_t out_sz);


#endif // WIFI_BTC_PORTAL_H