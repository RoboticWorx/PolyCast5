#ifndef BLUETOOTH_WEB_PORTAL_H
#define BLUETOOTH_WEB_PORTAL_H

#include "esp_err.h"

#define MAX_KEYBOARD_SCRIPTS 16
#define NUM_KEYBOARD_BASE 2

#define PORTAL_SSID "PolyCast5-BT-Scripts"
#define PORTAL_PASS "pc5script"

/** 
 * @brief Starts SoftAP and HTTP server for web bluetooth script entry portal
 *
 * @returns ESP error status
 */
esp_err_t bluetooth_web_portal_start(void);

/** 
 * @brief Stops SoftAP and HTTP server for web bluetooth script entry portal
 */
void bluetooth_web_portal_stop(void);

/** 
 * @brief Gets the current web portal IP
 *
 * @returns Web portal IP
 */
const char *bluetooth_web_portal_get_ip(void);

/** 
 * @brief Gets the number of user-added scripts
 *
 * @returns Number of user added scripts
 */
uint8_t bt_script_count_get(void);

/** 
 * @brief Gets the label of a given script
 *
 * @param [in] idx Index of script label to get
 * @param [in] buf Buffer to copy the label into
 * @param [in] buflen Length of buf
 *
 * @returns ESP error status
 */
esp_err_t bt_script_label_get(uint8_t idx, char *buf, size_t buflen);

/** 
 * @brief Gets the body of a given script
 *
 * @param [in] idx Index of script body to get
 * @param [in] buf Buffer to copy the body into
 * @param [in] buflen Length of buf
 * @param [in] outlen Length of the actual body string
 *
 * @returns ESP error status
 */
esp_err_t bt_script_body_get(uint8_t idx, char *buf, size_t buflen, size_t *outlen);

#endif // BLUETOOTH_WEB_PORTAL_H
