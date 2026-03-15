#ifndef AI_ANALYSIS_PORTAL_H
#define AI_ANALYSIS_PORTAL_H

#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Store the latest AI analysis result to be served by the portal.
 *        The portal makes an internal copy.
 *
 * @param [in] result NUL-terminated analysis string (may be NULL/empty)
 *
 * @returns ESP error status
 */
esp_err_t ai_analysis_portal_set_result(const char *result);

/**
 * @brief Start the AI analysis results web portal (SoftAP + HTTP server)
 *
 * @returns ESP error status
 */
esp_err_t ai_analysis_portal_start(void);

/**
 * @brief Stop the AI analysis results web portal
 *
 * @returns ESP error status
 */
esp_err_t ai_analysis_portal_stop(void);

/**
 * @brief Get AI analysis portal SSID
 */
const char *ai_analysis_portal_get_ssid(void);

/**
 * @brief Get AI analysis portal IP address
 */
const char *ai_analysis_portal_get_ip(void);


#endif // AI_ANALYSIS_PORTAL_H
