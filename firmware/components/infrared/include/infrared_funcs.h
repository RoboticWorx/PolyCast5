#ifndef INFRARED_FUNCS_H
#define INFRARED_FUNCS_H

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stddef.h>

// Configuration macros
#define RMT_RX_GPIO       4
#define RMT_TX_GPIO       5
#define RMT_RESOLUTION_HZ 1000000  // 1 µs resolution
#define MAX_PULSES        128
#define PULSE_BLOCK       96
#define MIN_VALID_PULSES  30
#define FINAL_GAP_US      10000    // 10ms for final gap
#define RANDOM_TX_THRESHOLD 5      // Start random TX after 5 signals
#define RANDOM_TX_DELAY_MS 3000    // 3s delay before random TX
#define MAX_STORED_SIGNALS 7       // Max 7 signals for NVS (~4KB)
#define INITIAL_CAPACITY   7       // Initial SRAM capacity

// Stored signal structure
typedef struct {
    rmt_symbol_word_t pulses[MAX_PULSES];
    size_t length;
} ir_signal_t;

// Global variables
extern ir_signal_t **stored_signals; // Array of pointers to signals
extern size_t num_stored_signals;
extern size_t stored_signals_capacity;
extern rmt_symbol_word_t ir_signal[MAX_PULSES];
extern size_t ir_signal_length;
extern SemaphoreHandle_t ir_rx_sem;
extern volatile bool is_transmitting;
extern rmt_channel_handle_t rx_channel;
extern rmt_channel_handle_t tx_channel;

// Function declarations
void init_rx(void);
void init_tx(void);
void restart_rx(void);
void transmit_ir_signal(rmt_symbol_word_t *signal, size_t length);
void init_nvs(void);
void load_stored_signals(void);
void save_stored_signal(size_t index);
void delete_stored_signal(size_t index);
void clear_stored_signals(void);

#endif // INFRARED_FUNCS_H