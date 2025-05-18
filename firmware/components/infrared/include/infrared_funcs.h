#ifndef INFRARED_FUNCS_H
#define INFRARED_FUNCS_H

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stddef.h>

#include "infrared_task.h"

// Configuration macros
#define RMT_RX_GPIO 24
#define RMT_TX_GPIO 12
#define RMT_RESOLUTION_HZ 1000000 // 1us resolution

#define MAX_PULSES 128
#define PULSE_BLOCK 96
#define MIN_VALID_PULSES 30

#define FINAL_GAP_US 10000 // 10ms for final gap

#define RANDOM_TX_THRESHOLD 15
#define RANDOM_TX_DELAY_MS 3000

#define MAX_STORED_SIGNALS 100
#define INITIAL_CAPACITY 5 // Initial SRAM capacity

// Stored signal structure
typedef struct {
    rmt_symbol_word_t pulses[MAX_PULSES];
    size_t length;
} ir_signal_t;

// Global variables
extern size_t stored_signals_capacity;
extern ir_signal_t **stored_signals; // Array of pointers to signals
extern size_t num_stored_signals;
extern rmt_symbol_word_t ir_signal[MAX_PULSES];
extern size_t ir_signal_length;

/** 
 * @brief Initialise NVS flash and check for errors
 */
void init_nvs(void);

/** 
 * @brief Initialise RMT RX
 */
void infrared_init_rx(void);

/** 
 * @brief Initialise RMT TX
 */
void infrared_init_tx(void);

/** 
 * @brief Restart and re-initialize RX config
 */
void infrared_restart_rx(void);

/**
 * @brief Transmit infrared signal
 *
 * @param [in] rmt_symbol_word_t Signal to transmit
 * @param [in] length Length of signal to transmit
 */
void infrared_transmit_ir(rmt_symbol_word_t *signal, size_t length);

/** 
 * @brief Checks if space is available to store another signal. Else, makes more space.
 */
bool ensure_capacity(void);

/** 
 * @brief Loads signals from NVS flash
 */
void infrared_load_stored_signals(void);

/** 
 * @brief Saves signal to NVS flash
 */
void infrared_save_stored_signal(void);

/** 
 * @brief Deletes signal from NVS flash
 */
void infrared_delete_stored_signal(size_t index);

/** 
 * @brief Deletes all signals from NVS flash
 */
void infrared_clear_stored_signals(void);

#endif // INFRARED_FUNCS_H