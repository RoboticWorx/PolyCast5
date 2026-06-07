#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xEspCmdRxStatusSemaphore;
extern SemaphoreHandle_t xEspCmdTxSuccessSemaphore;
extern SemaphoreHandle_t xEspCmdTxFailedSemaphore;

extern QueueHandle_t xEspSendEncKeyQueueNVS;
extern QueueHandle_t xEspSendEncKeyQueue;
extern QueueHandle_t xEspSendCmdQueue;
extern QueueHandle_t xEspSendMqttQueue;

// Accelerometer streaming
extern QueueHandle_t xEspAccelStreamCtrlQueue;
extern QueueHandle_t xEspAccelStreamQueue;

/**
 * @brief Create the ESP-NOW task
 */
void espnow_task_create(void);


#endif // ESPNOW_TASK_H