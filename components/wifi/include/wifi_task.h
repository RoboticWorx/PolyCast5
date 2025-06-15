#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include "freertos/idf_additions.h"

extern QueueHandle_t xWifiScanQueue;
extern QueueHandle_t xWifiSelectedNetworkQueue;

extern SemaphoreHandle_t xWifiStartScanSemaphore;
extern SemaphoreHandle_t xWifiNetworkConnectedSemaphore;
extern SemaphoreHandle_t xWifiNetworkDisconnectedSemaphore;
extern SemaphoreHandle_t xWifiDisconnectSemaphore;
extern SemaphoreHandle_t xWifiConnectingSemaphore;

/**
 * @brief Create the Wi-Fi task
 */
void wifi_task_create(void);


#endif // WIFI_TASK_H