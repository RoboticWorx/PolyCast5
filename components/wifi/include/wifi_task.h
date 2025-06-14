#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include "freertos/idf_additions.h"

extern QueueHandle_t xWifiScanQueue;

extern SemaphoreHandle_t xWifiStartScanSemaphore;

/**
 * @brief Create the Wi-Fi task
 */
void wifi_task_create(void);


#endif // WIFI_TASK_H