#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#include "freertos/idf_additions.h"

#include "lcd_espnow_funcs.h"

#define WIFI_CHANNEL 1

typedef struct {
    uint8_t mac_selected[ESPNOW_MAC_SIZE];
    uint8_t cmd_to_send;
    bool enc; // If encryption was enabled
    uint8_t lmk[LMK_LEN]; // Local master key (if enc)
} espnow_cmd_t;

typedef struct {
    uint8_t key[16];
    char ssid[33];
    char password[65];
    uint8_t cmd_to_send;
} espnow_mqtt_t;

extern SemaphoreHandle_t xEspCmdRxStatusSemaphore;
extern SemaphoreHandle_t xEspCmdTxSuccessSemaphore;
extern SemaphoreHandle_t xEspCmdTxFailedSemaphore;

extern QueueHandle_t xEspSendEncKeyQueueNVS;
extern QueueHandle_t xEspSendEncKeyQueue;
extern QueueHandle_t xEspSendCmdQueue;
extern QueueHandle_t xEspSendMqttQueue;

/**
 * @brief Create the ESP-NOW task
 */
void espnow_task_create(void);


#endif // ESPNOW_TASK_H