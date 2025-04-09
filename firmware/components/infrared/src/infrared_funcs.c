#include "infrared_funcs.h"

static const char *TAG = "IR_FUNCS";

// Global handles (rx_channel no longer static)
rmt_channel_handle_t rx_channel = NULL; // Made non-static for external access
static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;

static bool IRAM_ATTR rmt_rx_callback(rmt_channel_handle_t channel,
									  const rmt_rx_done_event_data_t *edata,
									  void *user_data) {
	ir_rx_callback_data_t *callback_data = (ir_rx_callback_data_t *)user_data;
	size_t len =
		edata->num_symbols > MAX_PULSES ? MAX_PULSES : edata->num_symbols;
	memcpy(callback_data->ir_signal, edata->received_symbols,
		   len * sizeof(rmt_symbol_word_t));
	*callback_data->ir_signal_length = len;
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(callback_data->ir_data_sem,
						  &xHigherPriorityTaskWoken);
	return xHigherPriorityTaskWoken == pdTRUE;
}

void transmit_ir_signal(rmt_symbol_word_t *signal, size_t length) {
	rmt_transmit_config_t tx_config = {
		.loop_count = 1,
	};
	esp_err_t ret =
		rmt_transmit(tx_channel, copy_encoder, signal,
					 length * sizeof(rmt_symbol_word_t), &tx_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Transmit failed: %s", esp_err_to_name(ret));
	} else {
		ESP_LOGI(TAG, "Signal transmitted (%d pulses)", length);
	}
}

void rmt_rx_init(ir_rx_callback_data_t *callback_data) {
	rmt_rx_channel_config_t rx_config = {
		.gpio_num = RMT_RX_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RMT_RESOLUTION_HZ,
		.mem_block_symbols = PULSE_BLOCK,
	};
	ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &rx_channel));

	rmt_rx_event_callbacks_t cbs = {
		.on_recv_done = rmt_rx_callback,
	};
	ESP_ERROR_CHECK(
		rmt_rx_register_event_callbacks(rx_channel, &cbs, callback_data));
	ESP_ERROR_CHECK(rmt_enable(rx_channel));

	rmt_receive_config_t rx_receive_config = {
		.signal_range_min_ns = 1000,
		.signal_range_max_ns = 24000000,
	};
	ESP_ERROR_CHECK(rmt_receive(rx_channel, callback_data->ir_signal,
								sizeof(rmt_symbol_word_t) * MAX_PULSES,
								&rx_receive_config));
}

void rmt_tx_init(void) {
	rmt_tx_channel_config_t tx_config = {
		.gpio_num = RMT_TX_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RMT_RESOLUTION_HZ,
		.mem_block_symbols = PULSE_BLOCK,
		.trans_queue_depth = 1,
		.flags.invert_out = true,
	};
	ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &tx_channel));

	rmt_carrier_config_t carrier_config = {
		.frequency_hz = 38000,
		.duty_cycle = 0.33,
	};
	ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_config));

	rmt_copy_encoder_config_t copy_encoder_config = {};
	ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &copy_encoder));
	ESP_ERROR_CHECK(rmt_enable(tx_channel));
}

rmt_channel_handle_t get_rx_channel(void) { return rx_channel; }
