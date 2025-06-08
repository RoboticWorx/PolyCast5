#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#define WIFI_CHANNEL 1

#include "lcd_espnow_funcs.h"

typedef struct {
    uint8_t mac_selected[ESPNOW_MAC_SIZE];
    uint8_t cmd_to_send;
} espnow_cmd_t;

extern SemaphoreHandle_t xEspCmdStatusSemaphore;

extern QueueHandle_t xEspSendEncKeyQueueNVS;
extern QueueHandle_t xEspSendEncKeyQueue;
extern QueueHandle_t xEspSendCmdQueue;

/**
 * @brief Create the ESP-NOW task
 */
void espnow_task_create(void);


#endif // ESPNOW_TASK_H