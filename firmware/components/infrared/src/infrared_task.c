#include "infrared_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "infrared_funcs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "IR_TASK";

SemaphoreHandle_t xInfraredRXEventSemaphore;

SemaphoreHandle_t xStartInfraredRXSemaphore;
SemaphoreHandle_t xEnableInfraredSemaphore;
SemaphoreHandle_t xDisableInfraredSemaphore;

SemaphoreHandle_t xSignalSavedSemaphore;
SemaphoreHandle_t xSignalReceivedSemaphore;

QueueHandle_t xSignalToTXQueue;

ir_signal_t **stored_signals;
rmt_symbol_word_t ir_signal[MAX_PULSES];

size_t stored_signals_capacity = INITIAL_CAPACITY;
size_t num_stored_signals = 0;
size_t ir_signal_length = 0;

int menu_idx;


static void infrared_task(void *pvParameters) {
	// Create semaphores
	xEnableInfraredSemaphore = xSemaphoreCreateBinary();
	xDisableInfraredSemaphore = xSemaphoreCreateBinary();
	xStartInfraredRXSemaphore = xSemaphoreCreateBinary();
	xSignalSavedSemaphore = xSemaphoreCreateBinary();
	xSignalReceivedSemaphore = xSemaphoreCreateBinary();
	
	xSignalToTXQueue = xQueueCreate(1, sizeof(int));
	
	
	// Wait until enabled
	xSemaphoreTake(xEnableInfraredSemaphore, portMAX_DELAY);
		
    ESP_LOGI(TAG, "Initializing IR system...");
    
    infrared_init_rx();
    infrared_init_tx();

	// Allocate initial
    stored_signals_capacity = INITIAL_CAPACITY;
    stored_signals = calloc(stored_signals_capacity, sizeof(ir_signal_t *));
    if (!stored_signals) {
        ESP_LOGE(TAG, "Failed to allocate stored_signals");
        vTaskDelete(NULL);
    }
    
    // Load signals from NVS
    infrared_load_stored_signals();
    //infrared_clear_stored_signals();
    ESP_LOGI(TAG, "Loaded %d signals from NVS", num_stored_signals);

    while (1) {
		
		if (xSemaphoreTake(xStartInfraredRXSemaphore, 1) == pdTRUE) {
			infrared_restart_rx();
		}
		
        // Wait for IR signal
        if (xSemaphoreTake(xInfraredRXEventSemaphore, 1) == pdTRUE) {

            // Filter out noise
            if (ir_signal_length < MIN_VALID_PULSES) {
                //ESP_LOGI(TAG, "Ignoring noise (only %d pulses)", ir_signal_length);
                infrared_restart_rx();
                continue;
            }
            
            ESP_LOGI(TAG, "Received IR signal with %d pulses", ir_signal_length);

            // Pad final gap
            if (ir_signal[ir_signal_length - 1].duration1 < FINAL_GAP_US) {
                ir_signal[ir_signal_length - 1].duration1 = FINAL_GAP_US;
                //ESP_LOGI(TAG, "Padded final gap to %dus", FINAL_GAP_US);
            }

			// Check if able to add more signals
            if (!ensure_capacity()) {
                infrared_restart_rx();
                continue;
            }
            
            // Allocate memory for signal
            ir_signal_t *sig = malloc(sizeof(ir_signal_t));
            if (!sig) {
                ESP_LOGE(TAG, "Out of heap for new signal");
                infrared_restart_rx();
                continue;
            }

			// Move into data structure
			memcpy(sig->pulses, ir_signal, ir_signal_length * sizeof(rmt_symbol_word_t));
            sig->length = ir_signal_length;
            stored_signals[num_stored_signals] = sig;
            
            // Save to flash
            infrared_save_stored_signal();
            num_stored_signals++;
			
			ESP_LOGI(TAG,
                     "Stored signal %zu (%zu pulses), SRAM=%zu/%zu",
                     num_stored_signals, sig->length,
                     num_stored_signals, stored_signals_capacity);
                     
            xSemaphoreGive(xSignalSavedSemaphore);

        }
        
        if (xSemaphoreTake(xDisableInfraredSemaphore, 1) == pdTRUE) {
			infrared_disable_rx();
		}	
        
        if (xQueueReceive(xSignalToTXQueue, &menu_idx, 1) == pdTRUE) {
			if (menu_idx < 0) {
				menu_idx = -menu_idx; // Make pos
				size_t sig_idx = (size_t) menu_idx - 2; // 0-based user list
				
				infrared_delete_stored_signal(sig_idx);
			}
			else {
				size_t sig_idx = (size_t) menu_idx - 2; // 0-based user list
			
			    ir_signal_t *sig = stored_signals[sig_idx];
			    ESP_LOGI(TAG, "Replaying signal %zu (%zu pulses)", sig_idx, sig->length);
			    infrared_transmit_ir(sig->pulses, sig->length);
		    }
		    
		}

        // Random transmission after threshold
        /*if (num_stored_signals >= RANDOM_TX_THRESHOLD) {
            vTaskDelay(pdMS_TO_TICKS(RANDOM_TX_DELAY_MS));

            size_t random_idx = 3;                     // fixed for now 
            ir_signal_t *sig  = stored_signals[random_idx];

            if (sig) {
                ESP_LOGI(TAG, "Replaying signal %zu (%zu pulses)",
                         random_idx + 1, sig->length);
                infrared_transmit_ir(sig->pulses, sig->length);
            }
        }*/
    }
}

void infrared_task_create(void) {
    if (xTaskCreate(infrared_task, "infrared_task", 4096 * 2, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start infrared_task");
	}
}