#ifndef BLUETOOTH_TASK_H
#define BLUETOOTH_TASK_H

#include "freertos/idf_additions.h"

#define BLUETOOTH_CONNECTED_BIT     (1U << 0)
#define BLUETOOTH_DONE_TYPING_BIT   (1U << 1)
#define BLUETOOTH_CANCEL_TYPING_BIT (1U << 2)
#define BLUETOOTH_U2F_ACTIVE_BIT    (1U << 3) // U2F persona is up and advertising
#define BLUETOOTH_U2F_BONDED_BIT    (1U << 4) // A host is connected and bonded
#define BLUETOOTH_U2F_PRESENCE_BIT  (1U << 5) // A host is waiting on the button
extern EventGroupHandle_t xBluetoothEventGroup;

extern QueueHandle_t xBluetoothMediaCmdQueue;
extern QueueHandle_t xBluetoothAiCmdQueue;
extern QueueHandle_t xBluetoothAiDictateQueue;
extern QueueHandle_t xBluetoothAiStreamQueue;

/**
 * @brief Creates bluetooth task at shared priority
 */
void bluetooth_task_create(void);

#endif // BLUETOOTH_TASK_H
