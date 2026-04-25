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
 * @brief Saves the firmware version to NVS
 *
 * @param [in] val Version to save
 *
 * @returns ESP error status
 */
esp_err_t wifi_ota_update_set_nvs_version(const char *val);

/**
 * @brief Gets the firmware version from NVS
 *
 * @param [in] val Version to get
 *
 * @returns ESP error status
 */
esp_err_t wifi_ota_update_get_nvs_version(char *out, size_t out_sz);

/**
 * @brief Stages a pending firmware version in NVS prior to OTA reboot.
 *        Promoted to the canonical version key only after a healthy boot
 *        of the new image (see wifi_task.c). Discarded on rollback.
 */
esp_err_t wifi_ota_update_set_nvs_pending_version(const char *val);

/**
 * @brief Reads the staged pending firmware version from NVS.
 */
esp_err_t wifi_ota_update_get_nvs_pending_version(char *out, size_t out_sz);

/**
 * @brief Erases the staged pending firmware version from NVS.
 */
esp_err_t wifi_ota_update_erase_nvs_pending_version(void);

/**
 * @brief Marks current OTA app as valid (not boot-looping)
 */
void wifi_ota_update_mark_app_valid(void);


#endif // WIFI_OTA_UPDATE_H
