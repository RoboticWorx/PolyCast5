#ifndef LORA_TASK_H
#define LORA_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xLoraGenerateEncKeySemaphore;
extern SemaphoreHandle_t xLoraReceiptValidSemaphore;

extern QueueHandle_t xLoraSendEncQueue;

/**
 * @brief Abort any pending LoRa transaction and reset radio to standby.
 *        Call before light sleep so the task doesn't wake up in a stale state.
 */
void lora_task_abort_pending(void);

/**
 * @brief Restore the radio after returning from light sleep. In Meshtastic mode
 *        this re-arms the continuous RX that lora_task_abort_pending() idled; in
 *        PCP mode the radio rests in standby between commands, so this is a no-op.
 */
void lora_task_resume_after_sleep(void);

// Function to create the LoRa task
void lora_task_create(void);

#endif // LORA_TASK_H