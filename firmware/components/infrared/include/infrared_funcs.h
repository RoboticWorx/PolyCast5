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
#define FINAL_GAP_US      10000

// Global variables
extern rmt_symbol_word_t ir_signal[MAX_PULSES];
extern size_t ir_signal_length;
extern SemaphoreHandle_t ir_rx_sem;
extern volatile bool is_transmitting;

// Function declarations
void init_rx(void);
void init_tx(void);
void restart_rx(void);
void transmit_ir_signal(rmt_symbol_word_t *signal, size_t length);

#endif // INFRARED_FUNCS_H