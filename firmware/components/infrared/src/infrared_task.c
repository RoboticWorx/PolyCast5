#include "infrared_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "infrared_funcs.h"

static const char *TAG = "IR_TASK";

static void infrared_task(void *pvParameters) {
	ESP_LOGI(TAG, "Initializing IR system...");
	init_rx();
	init_tx();

    while (1) {
        // Wait indefinitely until an IR signal is captured.
        if (xSemaphoreTake(ir_rx_sem, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Received IR signal with %d pulses", ir_signal_length);
            
            // Filter out noise: ignore signals with too few pulses.
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
            
            // Wait a few seconds (e.g., 3 sec) before retransmitting.
            vTaskDelay(pdMS_TO_TICKS(3000));
            ESP_LOGI(TAG, "Retransmitting IR signal...");
            transmit_ir_signal(ir_signal, ir_signal_length);
            
            // Wait briefly for any self-generated noise to clear.
            vTaskDelay(pdMS_TO_TICKS(250));
            
            restart_rx();
        }
    }
}

void infrared_task_create(void) {
	xTaskCreate(infrared_task, "infrared_task", 4096, NULL, 5, NULL);
}
