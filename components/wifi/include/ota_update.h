#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>

/**
 * @brief Spawns OTA task to begin the update
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
 * @brief 
 */
//void ota_mark_app_valid(void);


#endif // OTA_UPDATE_H
