#ifndef ESPNOW_TASK_H
#define ESPNOW_TASK_H

#include "freertos/idf_additions.h"

#include "lora_pcp.h"

// Track success and failure so the LCD task never waits on a key that will never arrive
typedef struct {
    bool success; // false = radio bring-up/send failed, key not delivered
    uint8_t key[LORA_PCP_ENC_KEY_LEN];
} espnow_enc_key_result_t;

extern SemaphoreHandle_t xEspCmdRxStatusSemaphore;
extern SemaphoreHandle_t xEspCmdTxSuccessSemaphore;
extern SemaphoreHandle_t xEspCmdTxFailedSemaphore;

extern QueueHandle_t xEspSendEncKeyQueueNVS;
extern QueueHandle_t xEspSendEncKeyQueue;
extern QueueHandle_t xEspSendCmdQueue;
extern QueueHandle_t xEspSendMqttQueue;

// Accelerometer streaming
extern QueueHandle_t xEspEcompassStreamCtrlQueue;
extern QueueHandle_t xEspEcompassStreamQueue;

/**
 * @brief Create the ESP-NOW task
 */
void espnow_task_create(void);


#endif // ESPNOW_TASK_H