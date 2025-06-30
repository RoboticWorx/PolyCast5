#ifndef TOOLS_TASK_H
#define TOOLS_TASK_H

//#include "freertos/idf_additions.h"

//extern QueueHandle_t xWifiScanQueue;

//extern SemaphoreHandle_t xWifiStartScanSemaphore;

/**
 * @brief Create the tools task
 */
void tools_task_create(void);

#endif // TOOLS_TASK_H