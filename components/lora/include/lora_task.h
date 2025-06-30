#ifndef LORA_TASK_H
#define LORA_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xLoraGenerateEncKeySemaphore;
extern SemaphoreHandle_t xLoraReceiptValidSemaphore;

extern QueueHandle_t xLoraSendEncQueue;

// Function to create the LoRa task
void lora_task_create(void);

#endif // LORA_TASK_H