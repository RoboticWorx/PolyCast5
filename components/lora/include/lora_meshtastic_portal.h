#ifndef LORA_MESHTASTIC_PORTAL_H
#define LORA_MESHTASTIC_PORTAL_H

#include <stdbool.h>

#include "esp_err.h"

#define MESHTASTIC_PORTAL_SSID "PolyCast5-Meshtastic"

/**
 * @brief Loads (or first-time generates and saves) the random Wi-Fi portal password
 *
 * Call once at startup so the password is ready before the LCD displays it. Safe to
 * call more than once; subsequent calls are no-ops once the password is in memory.
 */
void lora_meshtastic_portal_pass_init(void);

/**
 * @brief Starts the SoftAP and HTTP server for the Meshtastic config web portal
 *
 * @returns ESP error status
 */
esp_err_t lora_meshtastic_portal_start(void);

/**
 * @brief Stops the SoftAP and HTTP server for the Meshtastic config web portal
 *
 * @returns ESP error status
 */
esp_err_t lora_meshtastic_portal_stop(void);

/**
 * @brief Gets the current web portal IP
 *
 * @returns Web portal IP string
 */
const char *lora_meshtastic_portal_get_ip(void);

/**
 * @brief Gets the SSID for the Meshtastic web portal
 *
 * @returns SSID string
 */
const char *lora_meshtastic_portal_get_ssid(void);

/**
 * @brief Gets the password for the Meshtastic web portal
 *
 * @returns Password string
 */
const char *lora_meshtastic_portal_get_pass(void);

/**
 * @brief Saves the Meshtastic enabled flag to NVS
 *
 * @param [in] enabled Whether Meshtastic is enabled
 *
 * @returns ESP error status
 */
esp_err_t lora_meshtastic_portal_enabled_save_nvs(bool enabled);

/**
 * @brief Loads the persisted Meshtastic enabled flag from NVS
 *
 * @returns true if enabled, false if disabled or not yet stored
 */
bool lora_meshtastic_portal_enabled_load_nvs(void);

#endif // LORA_MESHTASTIC_PORTAL_H
