#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include "freertos/idf_additions.h"

extern QueueHandle_t xWifiScanQueue;
extern QueueHandle_t xWifiSelectedNetworkQueue;
extern QueueHandle_t xWifiSniffQueue;
extern QueueHandle_t xWifiBeaconQueue;
extern QueueHandle_t xWifiDataQueue;
extern QueueHandle_t xWifiMqttCmdQueue;

extern SemaphoreHandle_t xWifiStartScanSemaphore;
extern SemaphoreHandle_t xWifiNetworkConnectedSemaphore;
extern SemaphoreHandle_t xWifiNetworkDisconnectedSemaphore;
extern SemaphoreHandle_t xWifiDisconnectSemaphore;
extern SemaphoreHandle_t xWifiConnectingSemaphore;
extern SemaphoreHandle_t xWifiReconnectSemaphore;
extern SemaphoreHandle_t xWifiCanSleepSemaphore;
extern SemaphoreHandle_t xWifiMqttSuccessSemaphore;

/**
 * @brief Create the Wi-Fi task
 */
void wifi_task_create(void);


#endif // WIFI_TASK_H