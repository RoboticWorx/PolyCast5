#ifndef BLUETOOTH_TASK_H
#define BLUETOOTH_TASK_H

#include "freertos/idf_additions.h"

// Define each sequentially (0, 1, 2, ...)
enum
{
	BLUETOOTH_QUEUE_CMD_INIT,
	BLUETOOTH_QUEUE_CMD_DEINIT,
	BLUETOOTH_QUEUE_CMD_VOL_UP,
	BLUETOOTH_QUEUE_CMD_VOL_DOWN,
	BLUETOOTH_QUEUE_CMD_NEXT_TRACK,
	BLUETOOTH_QUEUE_CMD_PREV_TRACK,
	BLUETOOTH_QUEUE_CMD_PLAY_PAUSE,
};

extern QueueHandle_t xBluetoothCmdQueue;

/**
 * @brief Creates bluetooth task at shared priority
 */
void bluetooth_task_create(void);

#endif // BLUETOOTH_TASK_H
