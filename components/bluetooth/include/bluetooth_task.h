#ifndef BLUETOOTH_TASK_H
#define BLUETOOTH_TASK_H

#include "freertos/idf_additions.h"

#define BLUETOOTH_QUEUE_CMD_INIT 1
#define BLUETOOTH_QUEUE_CMD_DEINIT 2
#define BLUETOOTH_QUEUE_CMD_VOL_UP 3
#define BLUETOOTH_QUEUE_CMD_VOL_DOWN 4

extern QueueHandle_t xBluetoothCmdQueue;

/**
 * @brief Creates bluetooth task at shared priority
 */
void bluetooth_task_create(void);

#endif // BLUETOOTH_TASK_H
