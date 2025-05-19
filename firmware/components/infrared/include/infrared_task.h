#ifndef INFRARED_TASK_H
#define INFRARED_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern SemaphoreHandle_t xInfraredRXEventSemaphore;
extern SemaphoreHandle_t xStartInfraredRXSemaphore;
extern SemaphoreHandle_t xEnableInfraredSemaphore;
extern SemaphoreHandle_t xDisableInfraredSemaphore;

extern SemaphoreHandle_t xSignalSavedSemaphore;
extern SemaphoreHandle_t xSignalReceivedSemaphore;

extern QueueHandle_t xSignalToTXQueue;

// Function to create the infrared task
void infrared_task_create(void);

#endif // INFRARED_TASK_H