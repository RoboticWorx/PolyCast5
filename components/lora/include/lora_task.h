#ifndef LORA_TASK_H
#define LORA_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xLoraGenerateEncKeySemaphore;
extern SemaphoreHandle_t xLoraReceiptValidSemaphore;

extern QueueHandle_t xLoraSendEncQueue;

/**
 * @brief Abort any pending LoRa transaction and reset radio to standby.
 *        Call before light sleep so the task doesn't wake up in a stale state.
 */
void lora_task_abort_pending(void);

// Function to create the LoRa task
void lora_task_create(void);

#endif // LORA_TASK_H