#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#define WIFI_CHANNEL 1

extern QueueHandle_t xEspSendEncKeyQueueNVS;
extern QueueHandle_t xEspSendEncKeyQueue;

/**
 * @brief Create the ESP-NOW task
 */
void espnow_task_create(void);


#endif // ESPNOW_TASK_H