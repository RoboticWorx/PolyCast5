#include "infrared_funcs.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "IR_FUNCS";

// Global capture buffer, pulse count, and a semaphore for RX completion.
rmt_symbol_word_t ir_signal[MAX_PULSES];
size_t ir_signal_length = 0;
SemaphoreHandle_t ir_rx_sem = NULL;

static rmt_channel_handle_t rx_channel = NULL;
static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t tx_encoder = NULL;

// Transmission state
volatile bool is_transmitting = false;

/*
 * Minimal RX callback.
 * In interrupt context, it copies received pulses into ir_signal,
 * sets ir_signal_length, and gives the semaphore.
 */
static bool IRAM_ATTR rmt_rx_callback(rmt_channel_handle_t channel,
									  const rmt_rx_done_event_data_t *edata,
									  void *user_data) {
	if (is_transmitting) {
		ESP_LOGD(TAG, "RX ISR ignored: transmission in progress");
		return false; // Ignore signal during TX
	}

	size_t len = edata->num_symbols;
	if (len > MAX_PULSES) {
		len = MAX_PULSES;
	}
	memcpy(ir_signal, edata->received_symbols, len * sizeof(rmt_symbol_word_t));
	ir_signal_length = len;

	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(ir_rx_sem, &xHigherPriorityTaskWoken);
	return (xHigherPriorityTaskWoken == pdTRUE);
}

/*
 * init_rx():
 *   - Creates a binary semaphore,
 *   - Configures the RX channel on RMT_RX_GPIO,
 *   - Registers the RX callback,
 *   - Starts the initial RX session.
 */
void init_rx(void) {
	ir_rx_sem = xSemaphoreCreateBinary();
	if (ir_rx_sem == NULL) {
		ESP_LOGE(TAG, "Failed to create RX semaphore");
		return;
	}

	rmt_rx_channel_config_t rx_config = {
		.gpio_num = RMT_RX_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RMT_RESOLUTION_HZ,
		.mem_block_symbols = PULSE_BLOCK,
	};
	ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &rx_channel));

	rmt_rx_event_callbacks_t callbacks = {
		.on_recv_done = rmt_rx_callback,
	};
	ESP_ERROR_CHECK(
		rmt_rx_register_event_callbacks(rx_channel, &callbacks, NULL));
	ESP_ERROR_CHECK(rmt_enable(rx_channel));

	rmt_receive_config_t rx_receive_config = {
		.signal_range_min_ns = 1000,	 // ignore pulses shorter than 1 µs
		.signal_range_max_ns = 24000000, // 24 ms timeout
	};
	ESP_ERROR_CHECK(rmt_receive(rx_channel, ir_signal,
								sizeof(rmt_symbol_word_t) * MAX_PULSES,
								&rx_receive_config));
}

/*
 * init_tx():
 *   - Configures the TX channel on RMT_TX_GPIO with a 38 kHz carrier,
 *   - Creates a copy encoder,
 *   - Enables the TX channel.
 */
void init_tx(void) {
	rmt_tx_channel_config_t tx_config = {
		.gpio_num = RMT_TX_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RMT_RESOLUTION_HZ,
		.mem_block_symbols = PULSE_BLOCK,
		.trans_queue_depth = 1,
		//.flags.invert_out = false,
	};
	ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &tx_channel));

	rmt_carrier_config_t carrier_config = {
		.frequency_hz = 38000,
		.duty_cycle = 0.33,
		.flags.polarity_active_low = true, // Invert carrier polarity
	};
	ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_config));

	rmt_copy_encoder_config_t encoder_config = {0};
	ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &tx_encoder));
	ESP_ERROR_CHECK(rmt_enable(tx_channel));
}

/*
 * transmit_ir_signal():
 *   - Transmits the provided IR signal using the TX channel.
 */
void transmit_ir_signal(rmt_symbol_word_t *signal, size_t length) {
	if (signal == NULL || length == 0 || length > MAX_PULSES) {
		ESP_LOGE(TAG, "Invalid signal: null=%d, length=%d", signal == NULL,
				 length);
		return;
	}
	if (tx_channel == NULL || tx_encoder == NULL) {
		ESP_LOGE(TAG, "TX channel or encoder not initialized");
		return;
	}

	is_transmitting = true;

	rmt_transmit_config_t tx_config = {.loop_count = 1};
	esp_err_t ret =
		rmt_transmit(tx_channel, tx_encoder, signal,
					 length * sizeof(rmt_symbol_word_t), &tx_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Transmit failed: %s", esp_err_to_name(ret));
	} else {
		ESP_LOGI(TAG, "Transmission complete (%d pulses)", length);
	}

	is_transmitting = false;
}

/*
 * restart_rx():
 *   - Restart RX to receive again.
 */
void restart_rx(void) {
    if (rx_channel == NULL) {
        ESP_LOGE(TAG, "Cannot restart RX: channel not initialized");
        return;
    }
    rmt_receive_config_t rx_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 24000000,
    };
    ESP_ERROR_CHECK(rmt_receive(rx_channel, ir_signal,
                                sizeof(rmt_symbol_word_t) * MAX_PULSES, &rx_config));
}