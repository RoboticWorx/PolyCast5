#ifndef INFRARED_TASK_H
#define INFRARED_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xInfraredRxEventSemaphore;
extern SemaphoreHandle_t xInfraredStartRxSemaphore;
extern SemaphoreHandle_t xInfraredDisableSemaphore;
extern SemaphoreHandle_t xInfraredSignalTooLongSemaphore; // Learned signal filled the RX buffer: not saved

extern SemaphoreHandle_t xInfraredDataMutex;

extern QueueHandle_t xInfraredSignalToTxQueue;
extern QueueHandle_t xInfraredSignalSavedQueue; // esp_err_t: learned signal's NVS save result (not ESP_OK = dropped)

/** 
 * @brief Create infrared task
 */
void infrared_task_create(void);

#endif // INFRARED_TASK_H