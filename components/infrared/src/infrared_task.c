#include "polycast5_macros.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "infrared_task.h"
#include "infrared_funcs.h"

static const char *TAG = "IR_TASK";

SemaphoreHandle_t xInfraredRxEventSemaphore;
SemaphoreHandle_t xInfraredStartRxSemaphore;
SemaphoreHandle_t xInfraredDisableSemaphore;
SemaphoreHandle_t xInfraredSignalSavedSemaphore;

QueueHandle_t xInfraredSignalToTxQueue;

rmt_symbol_word_t ir_signal[MAX_PULSES]; // The active signal itself

ir_signal_t *stored_signals[MAX_STORED_SIGNALS]; // Signal struct with pulse len and data

size_t ir_signal_length = 0; // Global signal len buffer
size_t num_stored_signals = 0; // Global number of stored signals
volatile bool restart_rx_pending = false; // Global restart flag

int menu_idx; // Index received from menu


static void infrared_task(void *pvParameters) {
	// Create semaphores
	xInfraredDisableSemaphore = xSemaphoreCreateBinary();
	xInfraredStartRxSemaphore = xSemaphoreCreateBinary();
	xInfraredSignalSavedSemaphore = xSemaphoreCreateBinary();
	xInfraredRxEventSemaphore = xSemaphoreCreateBinary();
	
	xInfraredSignalToTxQueue = xQueueCreate(1, sizeof(int));
		
	#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Initializing IR system...");
    #endif
    
    infrared_init_rx();
    infrared_init_tx();
    
    // Load signals from NVS
    #ifdef POLYCAST5_IR_NVS_CLEAR
    	infrared_clear_nvs();
    #endif	
    
    infrared_load_stored_signals();
    
    #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Loaded %d signals from NVS", num_stored_signals);
    #endif
    

    while (1) {
		
		if (xSemaphoreTake(xInfraredDisableSemaphore, 0) == pdTRUE) {
			infrared_disable_rx();
		}	
		
		if (xSemaphoreTake(xInfraredStartRxSemaphore, 0) == pdTRUE) {
			infrared_restart_rx();
		}
		
        // Wait for IR signal
        if (xSemaphoreTake(xInfraredRxEventSemaphore, 0) == pdTRUE) {
			
			if (restart_rx_pending) {
				infrared_restart_rx(); // len < MIN_VALID_PULSES
			    restart_rx_pending = false;
			}
			
			if (!infrared_ensure_capacity()) {
				ESP_LOGW(TAG, "Max signals reached, dropping new signal");
			    infrared_restart_rx();
			    continue;
			}

            // Pad final gap
            if (ir_signal[ir_signal_length - 1].duration1 < FINAL_GAP_US) {
                ir_signal[ir_signal_length - 1].duration1 = FINAL_GAP_US;
                #ifdef POLYCAST5_DEBUG
		        	ESP_LOGI(TAG, "Padded final gap to %dus", FINAL_GAP_US);
		        #endif
            }
            
            // Compute exactly how many bytes we need:
			// Header (length field) + ir_signal_length entries
			size_t alloc_size = sizeof(ir_signal_t) + (ir_signal_length * sizeof(rmt_symbol_word_t));
			
			// Allocate that full size
			ir_signal_t *sig = malloc(alloc_size);
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
            
            #ifdef POLYCAST5_DEBUG
	        	ESP_LOGI(TAG, "Stored signal index %zu (%zu pulses)", num_stored_signals, sig->length);
	        #endif
	        
	        // One signal larger
            num_stored_signals++;
			
            xSemaphoreGive(xInfraredSignalSavedSemaphore);
        }
        
        if (xQueueReceive(xInfraredSignalToTxQueue, &menu_idx, 0) == pdTRUE) {
			if (menu_idx < 0) {
				menu_idx = -menu_idx; // Make pos
				size_t sig_idx = (size_t) menu_idx - 3; // 0-based user list with 3 non-sig entries
				
				infrared_delete_stored_signal(sig_idx);
			}
			else {
				size_t sig_idx = (size_t) menu_idx - 3; // 0-based user list with 3 non-sig entries
			
			    ir_signal_t *sig = stored_signals[sig_idx];
				#ifdef POLYCAST5_DEBUG
		        	ESP_LOGI(TAG, "Replaying signal %zu (%zu pulses)", sig_idx, sig->length);
		        #endif
			    
			    infrared_transmit_ir(sig->pulses, sig->length);
		    }
		    
		}
		
		vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void infrared_task_create(void) {
    if (xTaskCreate(infrared_task, "infrared_task", 1024 * 2, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
	    ESP_LOGE(TAG, "Failed to start infrared_task");
	}
}