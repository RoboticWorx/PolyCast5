#ifndef BLUETOOTH_TASK_H
#define BLUETOOTH_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xBleConnectedSemaphore;
extern SemaphoreHandle_t xBleConnectedIconSemaphore;
extern SemaphoreHandle_t xBleDisconnectedIconSemaphore;

extern QueueHandle_t xBluetoothMediaCmdQueue;
extern QueueHandle_t xBluetoothAiCmdQueue;

/**
 * @brief Creates bluetooth task at shared priority
 */
void bluetooth_task_create(void);

#endif // BLUETOOTH_TASK_H
