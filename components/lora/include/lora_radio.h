#ifndef LORA_RADIO_H
#define LORA_RADIO_H

#include <stdbool.h>
#include <stdint.h>

#include "sx126x_hal.h"

typedef struct sx126x_s {
    void *context;
    sx126x_hal_status_t (*hal_write)(const void *context, const uint8_t *command, const uint16_t command_length, const uint8_t *data, const uint16_t data_length);
    sx126x_hal_status_t (*hal_read)(const void *context, const uint8_t *command, const uint16_t command_length, uint8_t *data, const uint16_t data_length);
    sx126x_hal_status_t (*hal_reset)(const void *context);
    sx126x_hal_status_t (*hal_wakeup)(const void *context);
} sx126x_t;

/**
 * @brief Sets SX1262 radio in receive mode with timeout
 */
void lora_radio_set_rx_mode(void);

/**
 * @brief Log the radio's true state: latched IRQ flags, chip mode, command
 *        status, device errors (XOSC/PLL/calibration), and the DIO1 ISR edge
 *        count. Diagnostic for "TX started but no IRQ ever came back":
 *          - irq shows TX_DONE latched but isr count unchanged -> DIO1 line/ISR
 *            never fired (wiring / pin mapping)
 *          - chip_mode TX long after start -> TX never finishing (PA/power)
 *          - chip_mode STBY_RC + XOSC_START error -> oscillator (TCXO) never
 *            started, TX/RX silently aborted
 *          - everything clean -> the TX was never started at all
 *
 * @param [in] ctx Short context prefix for the log line
 */
void lora_radio_log_health(const char *ctx);

/**
 * @brief Transmits raw data over LoRa
 *
 * @param [in] tx_data Data to transmit
 * @param [in] data_len Length of data to transmit
 *
 * @returns True on success
 */
bool lora_radio_tx(uint8_t tx_data[], uint8_t data_len);

#endif // LORA_RADIO_H
