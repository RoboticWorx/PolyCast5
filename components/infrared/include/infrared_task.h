#ifndef INFRARED_TASK_H
#define INFRARED_TASK_H

#include "freertos/idf_additions.h"

#include "infrared_funcs.h"

extern SemaphoreHandle_t xInfraredRxEventSemaphore;
extern SemaphoreHandle_t xInfraredStartRxSemaphore;
extern SemaphoreHandle_t xInfraredDisableSemaphore;
extern SemaphoreHandle_t xInfraredSignalSavedSemaphore;

extern QueueHandle_t xInfraredSignalToTxQueue;

/** 
 * @brief Create infrared task
 */
void infrared_task_create(void);

#endif // INFRARED_TASK_H