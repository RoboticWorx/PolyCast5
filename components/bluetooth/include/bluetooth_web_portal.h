#ifndef BLUETOOTH_WEB_PORTAL_H
#define BLUETOOTH_WEB_PORTAL_H

#include "esp_err.h"

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

#endif // BLUETOOTH_WEB_PORTAL_H
