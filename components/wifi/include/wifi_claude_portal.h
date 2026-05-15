#ifndef WIFI_CLAUDE_PORTAL_H
#define WIFI_CLAUDE_PORTAL_H

#include "esp_err.h"

/**
 * @brief Start the Claude companion-host setup web portal (SoftAP + HTTP server).
 *        Accepts an IP + port from the user's phone/laptop browser and saves
 *        "http://<ip>:<port>/usage" via wifi_claude_save_config_nvs().
 */
esp_err_t wifi_claude_portal_start(void);

/**
 * @brief Stop the Claude companion-host setup web portal.
 */
esp_err_t wifi_claude_portal_stop(void);

/**
 * @brief Get the Claude setup portal SSID.
 */
const char *wifi_claude_portal_get_ssid(void);

/**
 * @brief Get the Claude setup portal IP address (typically 192.168.4.1).
 */
const char *wifi_claude_portal_get_ip(void);

#endif // WIFI_CLAUDE_PORTAL_H
