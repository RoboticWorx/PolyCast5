#include "infrared_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "infrared_funcs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "IR_TASK";

static void infrared_task(void *pvParameters) {
    ESP_LOGI(TAG, "Initializing IR system...");
    init_nvs();
    init_rx();
    init_tx();

    if (rx_channel == NULL || tx_channel == NULL) {
        ESP_LOGE(TAG, "RMT initialization failed, stopping task");
        vTaskDelete(NULL);
    }

    // Initialize dynamic storage
    stored_signals = malloc(INITIAL_CAPACITY * sizeof(ir_signal_t *));
    if (stored_signals == NULL) {
        ESP_LOGE(TAG, "Failed to allocate stored_signals");
        vTaskDelete(NULL);
    }
    stored_signals_capacity = INITIAL_CAPACITY;
    num_stored_signals = 0;

    // Load signals from NVS
    load_stored_signals();
    clear_stored_signals();
    ESP_LOGI(TAG, "Loaded %d signals from NVS", num_stored_signals);

    // Seed random number generator
    srand(xTaskGetTickCount());

    while (1) {
        // Wait for IR signal
        if (xSemaphoreTake(ir_rx_sem, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Received IR signal with %d pulses", ir_signal_length);

            // Filter out noise
            if (ir_signal_length < MIN_VALID_PULSES) {
                ESP_LOGI(TAG, "Ignoring noise (only %d pulses)", ir_signal_length);
                restart_rx();
                continue;
            }

            // Pad final gap
            if (ir_signal_length > 0 &&
                ir_signal[ir_signal_length - 1].duration1 < FINAL_GAP_US) {
                ir_signal[ir_signal_length - 1].duration1 = FINAL_GAP_US;
                ESP_LOGI(TAG, "Padded final gap to %dµs", FINAL_GAP_US);
            }

            // Store signal if not full
            if (num_stored_signals < MAX_STORED_SIGNALS) {
                ir_signal_t *new_signal = malloc(sizeof(ir_signal_t));
                if (new_signal == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate new signal");
                    restart_rx();
                    continue;
                }
                memcpy(new_signal->pulses, ir_signal, ir_signal_length * sizeof(rmt_symbol_word_t));
                new_signal->length = ir_signal_length;

                // Add to stored_signals
                stored_signals[num_stored_signals] = new_signal;
                save_stored_signal(num_stored_signals);
                num_stored_signals++;
                ESP_LOGI(TAG, "Stored signal %d with %d pulses, saved to NVS",
                         num_stored_signals, ir_signal_length);
            } else {
                ESP_LOGW(TAG, "Storage full (%d signals), cannot store signal", MAX_STORED_SIGNALS);
            }

            restart_rx();
        }

        // Random transmission after threshold
        if (num_stored_signals >= RANDOM_TX_THRESHOLD) {
            vTaskDelay(pdMS_TO_TICKS(RANDOM_TX_DELAY_MS));
            size_t random_idx = 3; //rand() % num_stored_signals;
            ESP_LOGI(TAG, "Transmitting random signal %d with %d pulses",
                     random_idx + 1, stored_signals[random_idx]->length);
            transmit_ir_signal(stored_signals[random_idx]->pulses,
                               stored_signals[random_idx]->length);
                               
        }
    }
}

void infrared_task_create(void) {
    xTaskCreate(infrared_task, "infrared_task", 8192, NULL, 5, NULL);
}