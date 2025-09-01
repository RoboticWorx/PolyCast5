#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>

/**
 * @brief 
 *
 * @returns 
 */
bool ota_start(const char *url);

/**
 * @brief 
 *
 * @returns 
 */
bool ota_in_progress(void);

/**
 * @brief 
 */
void ota_mark_app_valid(void);


#endif // OTA_UPDATE_H
