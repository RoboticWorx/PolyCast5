#ifndef AI_KEY_PORTAL_H
#define AI_KEY_PORTAL_H

#include "esp_err.h"

/**
 * @brief Starts the AI key web portal (Grok/xAI API key entry)
 *
 * @returns ESP error status
 */
esp_err_t ai_key_portal_start(void);

/**
 * @brief Stops the AI key web portal
 * 
 * @returns ESP error status
 */
esp_err_t ai_key_portal_stop(void);

/**
 * @brief Get AI key web portal SSID
 */
const char *ai_key_portal_get_ssid(void);

/**
 * @brief Get AI key web portal password
 */
const char *ai_key_portal_get_pass(void);

/**
 * @brief Get AI key web portal IP address
 */
const char *ai_key_portal_get_ip(void);

/**
 * @brief Save a randomly generated portal Wi-Fi password to NVS
 *
 * @param [in] val Password string to save
 *
 * @returns ESP error status
 */
esp_err_t ai_key_portal_pass_save_nvs(const char *val);

/**
 * @brief Load the randomly generated portal Wi-Fi password from NVS
 *
 * @param [out] out Pointer to the string to load the password in to
 * @param [out] out_sz Size of password
 *
 * @returns ESP error status
 */
esp_err_t ai_key_portal_pass_load_nvs(char *out, size_t out_sz);

#endif // AI_KEY_PORTAL_H