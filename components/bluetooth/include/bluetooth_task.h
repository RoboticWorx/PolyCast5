#ifndef BLUETOOTH_TASK_H
#define BLUETOOTH_TASK_H

#include "freertos/idf_additions.h"

extern QueueHandle_t xBluetoothMediaCmdQueue;

/**
 * @brief Creates bluetooth task at shared priority
 */
void bluetooth_task_create(void);

#endif // BLUETOOTH_TASK_H
