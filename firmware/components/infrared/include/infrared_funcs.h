#ifndef INFRARED_FUNCS_H
#define INFRARED_FUNCS_H

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RMT_RX_GPIO 4
#define RMT_TX_GPIO 5
#define RMT_RESOLUTION_HZ 1000000 // 1 MHz
#define MAX_PULSES 128
#define PULSE_BLOCK 64
#define TOLERANCE_US 50
#define FINAL_GAP_US 10000 // 10ms for typical IR protocols
#define BUTTON_GPIO 6	   // BOOT pin on ESP32

// Structure to pass data to RX callback
typedef struct {
	rmt_symbol_word_t *ir_signal;
	size_t *ir_signal_length;
	SemaphoreHandle_t ir_data_sem;
} ir_rx_callback_data_t;

// Function declarations
void transmit_ir_signal(rmt_symbol_word_t *signal, size_t length);
void rmt_rx_init(ir_rx_callback_data_t *callback_data);
void rmt_tx_init(void);
rmt_channel_handle_t get_rx_channel(void); // Getter for rx_channel

#endif // INFRARED_FUNCS_H