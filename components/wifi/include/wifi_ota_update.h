#ifndef WIFI_OTA_UPDATE_H
#define WIFI_OTA_UPDATE_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Checks GitHub page to see if a new firmware update is available
 *
 * @param [in] manifest_url URL to the OTA manifest file for comparing versions and getting the .bin update URL
 *
 * @returns True if ota_check_task was created successfully
 */
bool wifi_ota_update_check_start(const char *manifest_url);

/**
 * @brief Spawns OTA task to begin the update
 *
 * @param [in] url URL to the .bin update file
 *
 * @returns False on fail
 */
bool wifi_ota_update_start(const char *url);

/**
 * @brief Checks OTA task handle to see if it's active
 *
 * @returns True if in progress
 */
bool wifi_ota_update_in_progress(void);

/**
 * @brief Erases the firmware version keys older builds kept in NVS, so a
 *        downgrade to one of them reads its own image version again.
 *        The firmware version is esp_app_get_description()->version
 */
void wifi_ota_update_erase_legacy_version(void);

/**
 * @brief Marks current OTA app as valid (not boot-looping)
 */
void wifi_ota_update_mark_app_valid(void);


#endif // WIFI_OTA_UPDATE_H
