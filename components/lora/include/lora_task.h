#ifndef LORA_TASK_H
#define LORA_TASK_H

#include "sx126x.h"
#include "sx126x_hal.h"

#define ENC_KEY_LEN 16

extern SemaphoreHandle_t xGenerateEncKeySemaphore;
extern QueueHandle_t xSendEncKeyQueue;
extern QueueHandle_t xReceiveEncKeyQueue;
extern QueueHandle_t xReceiveEncIndexQueue;

typedef struct sx126x_s {
	void *context;
	sx126x_hal_status_t (*hal_write)(const void *context,
									 const uint8_t *command,
									 const uint16_t command_length,
									 const uint8_t *data,
									 const uint16_t data_length);
	sx126x_hal_status_t (*hal_read)(const void *context, const uint8_t *command,
									const uint16_t command_length,
									uint8_t *data, const uint16_t data_length);
	sx126x_hal_status_t (*hal_reset)(const void *context);
	sx126x_hal_status_t (*hal_wakeup)(const void *context);
} sx126x_t;

// Function to create the LoRa task
void lora_task_create(void);

#endif // LORA_TASK_H