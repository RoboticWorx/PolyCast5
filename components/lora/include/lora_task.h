#ifndef LORA_TASK_H
#define LORA_TASK_H

#include "freertos/idf_additions.h"

#include "lora_pcp.h" // lora_pcp_cmd_t, lora_receipt_t

/**
 * @brief One entry of xLoraSendEncQueue: a command plus where it came from.
 *
 * Only commands raised from a LoRa page (ui_origin) produce a receipt. A hotkey
 * fired from the homescreen must not paint its outcome onto whichever page the
 * user happens to open next, so its result is dropped instead.
 */
typedef struct {
    lora_pcp_cmd_t cmd;
    bool ui_origin;
} lora_send_req_t;

extern SemaphoreHandle_t xLoraGenerateEncKeySemaphore;

extern QueueHandle_t xLoraSendEncQueue;

// Outcome of the last UI-originated command (lora_receipt_t, length 1).
// Created by lora_task_create() so LCD pages can drain it before lora_task runs.
extern QueueHandle_t xLoraReceiptQueue;

// Total DIO1 rising edges seen by the ISR since boot. Read by
// lora_radio_log_health() so a silent radio (no IRQ ever latched) can be told
// apart from a broken interrupt line (IRQ latched but no edge delivered).
extern volatile uint32_t g_lora_dio1_isr_count;

/**
 * @brief Report a received ACK's relay-state byte to the pending command.
 *        Maps the byte to a lora_receipt_t and posts it for the UI.
 *
 * @param [in] ack_state LORA_PCP_ACK_STATE_ON/OFF, or LORA_PCP_ACK_STATE_NONE
 *                       when the PolyPlug reported no relay level.
 */
void lora_task_post_ack(uint8_t ack_state);

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