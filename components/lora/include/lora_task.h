#ifndef LORA_TASK_H
#define LORA_TASK_H

#include "freertos/idf_additions.h"

extern SemaphoreHandle_t xLoraGenerateEncKeySemaphore;
extern SemaphoreHandle_t xLoraReceiptValidSemaphore;

extern QueueHandle_t xLoraSendEncQueue;

// Total DIO1 rising edges seen by the ISR since boot. Read by
// lora_radio_log_health() so a silent radio (no IRQ ever latched) can be told
// apart from a broken interrupt line (IRQ latched but no edge delivered).
extern volatile uint32_t g_lora_dio1_isr_count;

/**
 * @brief Abort any pending LoRa transaction and reset radio to standby.
 *        Call before light sleep so the task doesn't wake up in a stale state.
 */
void lora_task_abort_pending(void);

/**
 * @brief Restore the radio after returning from light sleep.
 *        In Meshtastic mode this is a defensive re-arm. In PCP mode the radio rests in standby
 *        between commands, so there is nothing to re-arm.
 */
void lora_task_resume_after_sleep(void);

// Function to create the LoRa task
void lora_task_create(void);

#endif // LORA_TASK_H