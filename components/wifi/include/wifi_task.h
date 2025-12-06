#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include "freertos/idf_additions.h"

extern QueueHandle_t xWifiScanQueue;
extern QueueHandle_t xWifiSelectedNetworkQueue;
extern QueueHandle_t xWifiSniffQueue;
extern QueueHandle_t xWifiBeaconQueue;
extern QueueHandle_t xWifiDataQueue;
extern QueueHandle_t xWifiMqttCmdQueue;
extern QueueHandle_t xWifiPingQueue;

extern SemaphoreHandle_t xWifiStartScanSemaphore;
extern SemaphoreHandle_t xWifiNetworkConnectedSemaphore;
extern SemaphoreHandle_t xWifiNetworkDisconnectedSemaphore;
extern SemaphoreHandle_t xWifiDisconnectSemaphore;
extern SemaphoreHandle_t xWifiConnectingSemaphore;
extern SemaphoreHandle_t xWifiReconnectSemaphore;
extern SemaphoreHandle_t xWifiCanSleepSemaphore;
extern SemaphoreHandle_t xWifiMqttSuccessSemaphore;
extern SemaphoreHandle_t xWifiMqttConnectedSemaphore;
extern SemaphoreHandle_t xWifiMqttDisconnectedSemaphore;
extern SemaphoreHandle_t xWifiCycleSemaphore;
extern SemaphoreHandle_t xWifiPingSemaphore;
extern SemaphoreHandle_t xWifiConnectedIconSemaphore;
extern SemaphoreHandle_t xWifiDisconnectedIconSemaphore;

// OTA
extern QueueHandle_t xWifiOtaPctQueue;
extern SemaphoreHandle_t xWifiOtaAvailableSemaphore;

/**
 * @brief Create the Wi-Fi task
 */
void wifi_task_create(void);


#endif // WIFI_TASK_H