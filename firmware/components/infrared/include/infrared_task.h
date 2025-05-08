#ifndef INFRARED_TASK_H
#define INFRARED_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern SemaphoreHandle_t xInfraredRXEventSemaphore;

// Function to create the infrared task
void infrared_task_create(void);

#endif // INFRARED_TASK_H