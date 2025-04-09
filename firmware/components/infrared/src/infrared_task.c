#include "infrared_task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "infrared_funcs.h"

static const char *TAG = "INFRARED_TASK";

// Signal storage
static rmt_symbol_word_t ir_signal[MAX_PULSES];
static size_t ir_signal_length = 0;
static SemaphoreHandle_t ir_data_sem = NULL;

static void infrared_task(void *pvParameters) {
	
	// Initialize semaphore
	ir_data_sem = xSemaphoreCreateBinary();
	if (ir_data_sem == NULL) {
		ESP_LOGE(TAG, "Semaphore creation failed");
		vTaskDelete(NULL);
	}

	// Prepare callback data
	ir_rx_callback_data_t callback_data = {.ir_signal = ir_signal,
										   .ir_signal_length =
											   &ir_signal_length,
										   .ir_data_sem = ir_data_sem};

	// Initialize RMT RX and TX
	rmt_rx_init(&callback_data);
	rmt_tx_init();

	// Signal storage
	rmt_symbol_word_t stored_signal[MAX_PULSES];
	size_t stored_length = 0;
	bool learning_mode = true;

	while (1) {
		// Check button state
		if (gpio_get_level(BUTTON_GPIO) == 0) {
			vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
			if (gpio_get_level(BUTTON_GPIO) == 0) {
				learning_mode = !learning_mode;
				ESP_LOGI(TAG, "Switched to %s mode",
						 learning_mode ? "learning" : "replay");
				vTaskDelay(pdMS_TO_TICKS(500)); // Prevent rapid toggling
			}
		}

		// Process IR data
		if (xSemaphoreTake(ir_data_sem, pdMS_TO_TICKS(10)) == pdTRUE) {
			if (learning_mode) {
				memcpy(stored_signal, ir_signal,
					   ir_signal_length * sizeof(rmt_symbol_word_t));
				stored_length = ir_signal_length;
				if (stored_length > 0 &&
					stored_signal[stored_length - 1].duration1 < FINAL_GAP_US) {
					stored_signal[stored_length - 1].duration1 = FINAL_GAP_US;
					ESP_LOGI(TAG, "Padded final gap to %dµs", FINAL_GAP_US);
				}
				ESP_LOGI(TAG, "Learned IR signal with %d pulses",
						 stored_length);
				learning_mode = false; // Auto-switch to replay mode
			} else {
				transmit_ir_signal(stored_signal, stored_length);
			}

			// Restart RX
			rmt_receive_config_t rx_config = {
				.signal_range_min_ns = 1000,
				.signal_range_max_ns = 24000000,
			};
			esp_err_t ret =
				rmt_receive(get_rx_channel(), ir_signal,
							sizeof(rmt_symbol_word_t) * MAX_PULSES, &rx_config);
			if (ret != ESP_OK) {
				ESP_LOGE(TAG, "RX restart failed: %s", esp_err_to_name(ret));
			}
		}
	}
}

void infrared_task_create(void) {
	xTaskCreate(infrared_task, "infrared_task", 4096, NULL, 5, NULL);
}