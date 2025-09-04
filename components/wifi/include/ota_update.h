#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Checks GitHub page to see if a new firmware update is available
 *
 * @param [in] manifest_url URL to the OTA manifest file for comparing versions and getting the .bin update URL
 *
 * @returns True if ota_check_task was created successfully
 */
bool ota_update_check_start(const char *manifest_url);

/**
 * @brief Spawns OTA task to begin the update
 *
 * @param [in] url URL to the .bin update file
 *
 * @returns False on fail
 */
bool ota_update_start(const char *url);

/**
 * @brief Checks OTA task handle to see if it's active
 *
 * @returns True if in progress
 */
bool ota_update_in_progress(void);

/**
 * @brief Saves the firmware version to NVS
 *
 * @param [in] val Version to save
 *
 * @returns ESP error status
 */
esp_err_t ota_update_set_nvs_version(const char *val);

/**
 * @brief Gets the firmware version from NVS
 *
 * @param [in] val Version to get
 *
 * @returns ESP error status
 */
esp_err_t ota_update_get_nvs_version(char *out, size_t out_sz);

/**
 * @brief Marks current OTA app as valid (not boot-looping)
 */
void ota_update_mark_app_valid(void);


#endif // OTA_UPDATE_H
