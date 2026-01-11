#include "polycast5_macros.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "infrared_task.h"
#include "infrared_utils.h"

static const char *TAG = "IR_TASK";

extern size_t ir_signal_length;

SemaphoreHandle_t xInfraredRxEventSemaphore;
SemaphoreHandle_t xInfraredStartRxSemaphore;
SemaphoreHandle_t xInfraredDisableSemaphore;
SemaphoreHandle_t xInfraredSignalSavedSemaphore;

SemaphoreHandle_t xInfraredDataMutex;

QueueHandle_t xInfraredSignalToTxQueue;

rmt_symbol_word_t ir_signal[MAX_PULSES]; // The active signal itself

ir_remote_t remotes[MAX_REMOTES]; // Remotes array
size_t num_remotes = 1; // 1 default
size_t ir_current_remote = 0;

volatile bool restart_rx_pending = false; // Global restart flag

int menu_idx; // Index received from menu


static void infrared_task(void *pvParameters) {
    // Create semaphores
    xInfraredDisableSemaphore = xSemaphoreCreateBinary();
    configASSERT(xInfraredDisableSemaphore);
    xInfraredStartRxSemaphore = xSemaphoreCreateBinary();
    configASSERT(xInfraredStartRxSemaphore);
    xInfraredSignalSavedSemaphore = xSemaphoreCreateBinary();
    configASSERT(xInfraredSignalSavedSemaphore);
    xInfraredRxEventSemaphore = xSemaphoreCreateBinary();
    configASSERT(xInfraredRxEventSemaphore);
    
    // Other
    xInfraredDataMutex = xSemaphoreCreateMutex();
    configASSERT(xInfraredDataMutex);
    
    xInfraredSignalToTxQueue = xQueueCreate(1, sizeof(int));
    configASSERT(xInfraredSignalToTxQueue);
        
    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Initializing IR system...");
    #endif
    
    infrared_utils_init_rx();
    infrared_utils_init_tx();
    
    // Load remotes from NVS
    #ifdef POLYCAST5_IR_NVS_CLEAR
    infrared_utils_clear_nvs();
    #endif    
    
    // Load remotes and signals from NVS (includes names)
    infrared_utils_load_remotes_nvs();
    
    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Loaded %zu remotes from NVS", num_remotes);
    #endif
    
    while (1) {        
        // When user selects to add new signal
        if (xSemaphoreTake(xInfraredStartRxSemaphore, 0) == pdTRUE) {
            infrared_utils_restart_rx();
        }
        
        // When user canceled adding new signal
        if (xSemaphoreTake(xInfraredDisableSemaphore, 0) == pdTRUE) {
            infrared_utils_disable_rx();
        }
        
        // If received garbage in cb, restart
        if (restart_rx_pending) { // When len < MIN_VALID_PULSES
            #ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "Invalid IR signal, restarting RX");
            #endif
            infrared_utils_restart_rx();
            restart_rx_pending = false;
        }
        
        // Wait for valid IR signal
        if (xSemaphoreTake(xInfraredRxEventSemaphore, 0) == pdTRUE) {            
            xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR

            #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Received IR signal (%zu pulses)", ir_signal_length);
            #endif
            
            // Check if space available
            if (!infrared_utils_ensure_capacity()) {
                ESP_LOGW(TAG, "Max signals reached, dropping new signal");
                infrared_utils_restart_rx();
                
                xSemaphoreGive(xInfraredDataMutex); // Release IR
                continue;
            }
            
            // Pad final gap if less than FINAL_GAP_US
            if (ir_signal[ir_signal_length - 1].duration1 < FINAL_GAP_US) {
                ir_signal[ir_signal_length - 1].duration1 = FINAL_GAP_US;
                
                #ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Padded final gap to %dus", FINAL_GAP_US);
                #endif
            }
            
            // Compute exactly how many bytes we need for signal:
            // Header (length field) + ir_signal_length entries
            size_t alloc_size = sizeof(ir_signal_t) + (ir_signal_length * sizeof(rmt_symbol_word_t));
            
            // Allocate that full size
            ir_signal_t *sig = malloc(alloc_size);
            if (!sig) {
                ESP_LOGE(TAG, "Out of heap for new signal");
                infrared_utils_restart_rx();
                
                xSemaphoreGive(xInfraredDataMutex); // Release IR
                continue;
            }
            
            // Save signal to data structure
            memcpy(sig->pulses, ir_signal, ir_signal_length * sizeof(rmt_symbol_word_t));
            sig->length = ir_signal_length;
            
            // Append to the current remote
            size_t ns = remotes[ir_current_remote].num_signals; // Number of signals already in remote
            
            // Resize the dynamic array to hold another signal
            remotes[ir_current_remote].signals = realloc(remotes[ir_current_remote].signals, (ns + 1) * sizeof(ir_signal_t *));
            
            // Resize the dynamic array to hold another signal name
            remotes[ir_current_remote].signal_names = realloc(remotes[ir_current_remote].signal_names, (ns + 1) * sizeof(char *));
            
            // Save the signal to remote
            remotes[ir_current_remote].signals[ns] = sig;
            remotes[ir_current_remote].signal_names[ns] = strdup(""); // Temporary empty name
            remotes[ir_current_remote].num_signals++; // Now one more signal
            
            // Save the signal blob and update num_signals in NVS
            infrared_utils_save_signal_to_remote_nvs(ir_current_remote, ns, sig, ""); // Empty name for now
            infrared_utils_save_remote_nsig_nvs(ir_current_remote);
            
            #ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Saved signal index %zu for remote %zu (%zu pulses)", ns, ir_current_remote, sig->length);
            #endif
            
            xSemaphoreGive(xInfraredDataMutex); // Release IR
            
            xSemaphoreGive(xInfraredSignalSavedSemaphore); // Notify LCD we got and saved a valid signal

            // Disable until next signal
            infrared_utils_disable_rx();
        }
        
        // Transmit a specific signal (index menu_idx)
        if (xQueueReceive(xInfraredSignalToTxQueue, &menu_idx, 0) == pdTRUE) {
            xSemaphoreTake(xInfraredDataMutex, portMAX_DELAY); // Lock IR
            
            // Negative means delete index menu_idx
            if (menu_idx < 0) {
                menu_idx = -menu_idx; // Make positive
                size_t sig_idx = (size_t) menu_idx - 3; // Offset for 0-based
                
                infrared_utils_delete_signal_from_remote_nvs(ir_current_remote, sig_idx);
            } else { // Else send the signal at that index
                size_t sig_idx = (size_t) menu_idx - 3; // Offset for 0-based
            
                // Get signal from current remote
                ir_signal_t *sig = remotes[ir_current_remote].signals[sig_idx];
                
                #ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Replaying signal %zu for remote %zu (%zu pulses)", sig_idx, ir_current_remote, sig->length);
                #endif
                
                // Send
                infrared_utils_transmit_ir(sig->pulses, sig->length);
            }
            
            xSemaphoreGive(xInfraredDataMutex); // Release IR
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void infrared_task_create(void) {
    if (xTaskCreate(infrared_task, "infrared_task", 1024 * 3, NULL, POLYCAST5_PRIORITY_MEDIUM, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start infrared_task");
    }
}