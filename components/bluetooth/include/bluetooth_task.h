#ifndef BLUETOOTH_TASK_H
#define BLUETOOTH_TASK_H

#include "freertos/idf_additions.h"

#define BLUETOOTH_CONNECTED_BIT   (1U << 0)
#define BLUETOOTH_DONE_TYPING_BIT (1U << 1)
extern EventGroupHandle_t xBluetoothEventGroup;

extern QueueHandle_t xBluetoothMediaCmdQueue;
extern QueueHandle_t xBluetoothAiCmdQueue;

/**
 * @brief Creates bluetooth task at shared priority
 */
void bluetooth_task_create(void);

#endif // BLUETOOTH_TASK_H
