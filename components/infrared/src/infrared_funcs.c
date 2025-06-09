#include "polycast5_macros.h"

#include <string.h>

#include "nvs_flash.h"

#include "esp_log.h"

#include "infrared_funcs.h"
#include "infrared_task.h"

#define SIGNAL_MIN_NS 1000
#define SIGNAL_MAX_NS 15000000 // 15ms

#define IR_SIG_NS "is_ns"
#define IR_SIG_COUNT_KEY "isc_key"
#define IR_SIG_KEY "ir_sig%d"

static const char *TAG = "IR_FUNCS";

static rmt_channel_handle_t rx_channel = NULL;
static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t tx_encoder = NULL;

void init_nvs(void) {
	// Initialize flash
    esp_err_t ret = nvs_flash_init();
    
    // Error check
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    ESP_ERROR_CHECK(ret);
    
    #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "NVS initialized");
    #endif
    
}

// When IR signal received
static bool IRAM_ATTR infrared_rx_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data) {

	// Cap signal
    size_t len = edata->num_symbols;
    if (len > MAX_PULSES) {
        len = MAX_PULSES;
    }
    // Copy received signal into ir_signal
    memcpy(ir_signal, edata->received_symbols, len * sizeof(rmt_symbol_word_t));
    ir_signal_length = len;

    // Log last symbol
    if (len > 0) {
		#ifdef POLYCAST5_DEBUG
        	ESP_LOGD(TAG, "RX last symbol: level0=%d, duration0=%d, level1=%d, duration1=%d",
                 ir_signal[len - 1].level0, ir_signal[len - 1].duration0,
                 ir_signal[len - 1].level1, ir_signal[len - 1].duration1);
        #endif
        
    }

	// Notify semaphore that a signal was received
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xInfraredRXEventSemaphore, &xHigherPriorityTaskWoken);
    return (xHigherPriorityTaskWoken == pdTRUE);
}

void infrared_init_rx(void) {
	// IR semaphore
    xInfraredRXEventSemaphore = xSemaphoreCreateBinary();
    if (xInfraredRXEventSemaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create RX semaphore");
        return;
    }

	// Configure rmt channel
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = RMT_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = PULSE_BLOCK,
    };
    esp_err_t ret = rmt_new_rx_channel(&rx_config, &rx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RX channel: %s", esp_err_to_name(ret));
        rx_channel = NULL;
        return;
    }

	// Initialize RX callback
    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = infrared_rx_callback,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &callbacks, NULL));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));
}

void infrared_init_tx(void) {
	// Configure TX channel
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = RMT_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = PULSE_BLOCK,
        .trans_queue_depth = 1,
    };
    esp_err_t ret = rmt_new_tx_channel(&tx_config, &tx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TX channel: %s", esp_err_to_name(ret));
        tx_channel = NULL;
        return;
    }

	// TX signal configuration
    rmt_carrier_config_t carrier_config = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33,
        .flags.polarity_active_low = true,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_config));

	// Copy into rmt memory
    rmt_copy_encoder_config_t encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &tx_encoder));
    
    // Enable rmt
    ESP_ERROR_CHECK(rmt_enable(tx_channel));
}

void infrared_transmit_ir(rmt_symbol_word_t *signal, size_t length) {
	
	// Error check
    if (signal == NULL || length == 0 || length > MAX_PULSES) {
        ESP_LOGE(TAG, "Invalid signal: null=%d, length=%d", signal == NULL, length);
        return;
    }
    if (tx_channel == NULL || tx_encoder == NULL) {
        ESP_LOGE(TAG, "TX channel or encoder not initialized");
        return;
    }
	
	#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "TX symbol: level0=%d, duration0=%d, level1=%d, duration1=%d",
			 signal[length - 1].level0, signal[length - 1].duration0,
			 signal[length - 1].level1, signal[length - 1].duration1);
    #endif
	

	// Make sure not to pick up our own transmission
	rmt_disable(rx_channel);

	// TX once
    rmt_transmit_config_t tx_config = {.loop_count = 1};
    esp_err_t ret = rmt_transmit(tx_channel, tx_encoder, signal,
                                 length * sizeof(rmt_symbol_word_t), &tx_config);
    
    // Allow time for transmisison to finish
    rmt_tx_wait_all_done(tx_channel, portMAX_DELAY);
    
    // Error check
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Transmit failed: %s", esp_err_to_name(ret));
    } 
    else {
		#ifdef POLYCAST5_DEBUG
        	ESP_LOGI(TAG, "Transmission complete (%d pulses)", length);
        #endif
        
    }

	// Re-enable RX channel
    rmt_enable(rx_channel);
    //infrared_restart_rx(); // Start receiving again
}

void infrared_restart_rx(void) {
	// Ensure initialized
    if (rx_channel == NULL) {
        ESP_LOGE(TAG, "Cannot restart RX: channel not initialized");
        return;
    }
    
    // Disable channel
    rmt_disable(rx_channel);
    
    // Re-enable
    ESP_ERROR_CHECK(rmt_enable(rx_channel));
    
    // Re-apply
    rmt_receive_config_t rx_receive_config = {
        .signal_range_min_ns = SIGNAL_MIN_NS,
        .signal_range_max_ns = SIGNAL_MAX_NS,
    };
    ESP_ERROR_CHECK(rmt_receive(rx_channel, ir_signal, sizeof(rmt_symbol_word_t) * MAX_PULSES, &rx_receive_config));
}

void infrared_disable_rx(void)
{
	// Ensure initialized
    if (rx_channel == NULL) {
        ESP_LOGE(TAG, "Cannot disable RX: channel not initialized");
        return;
    }
    
    // Disable channel
    rmt_disable(rx_channel);
}

bool ensure_capacity(void)
{
	// If space available
    if (num_stored_signals < stored_signals_capacity) {
        return true;
    }

	// If maxed out
    if (stored_signals_capacity >= MAX_STORED_SIGNALS) {
        ESP_LOGW(TAG, "Storage full (%zu signals)", stored_signals_capacity);
        return false;
    }
	
	// Else make more space
    size_t new_cap = stored_signals_capacity + INITIAL_CAPACITY;
    if (new_cap > MAX_STORED_SIGNALS) {
        new_cap = MAX_STORED_SIGNALS; // Hard cap upper bound
    }

	// Allocate space for more signals
    ir_signal_t **tmp = realloc(stored_signals, new_cap * sizeof(ir_signal_t *));
    if (!tmp) {
        ESP_LOGE(TAG, "Out of heap enlarging stored_signals to %zu", new_cap);
        return false;
    }

    stored_signals = tmp;
    stored_signals_capacity = new_cap;
    
    #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Resized stored_signals to %zu", new_cap);
    #endif
    
    return true;
}

void infrared_save_stored_signal(void) {
	// Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(IR_SIG_NS, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

	// Pick the signal we're about to store
    ir_signal_t *sig = stored_signals[num_stored_signals];
    
    // Save signal
    char key[16];
    snprintf(key, sizeof(key), IR_SIG_KEY, num_stored_signals);
    
    // Calculate how many bytes to write: header + actual pulses
    size_t blob_size = sizeof(ir_signal_t) + sig->length * sizeof(rmt_symbol_word_t);
                     
    ret = nvs_set_blob(nvs_handle, key, sig, blob_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save signal %d: %s",
                 num_stored_signals,
                 esp_err_to_name(ret));
    }

    // Save num_stored_signals
    ret = nvs_set_u32(nvs_handle, IR_SIG_COUNT_KEY, num_stored_signals + 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save num_signals: %s", esp_err_to_name(ret));
    }
	
	// Write changes
    ret = nvs_commit(nvs_handle);
	if (ret != ESP_OK) {
	    ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
	}
	
	// Close NVS
    nvs_close(nvs_handle);
}

void infrared_load_stored_signals(void) {
	// Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(IR_SIG_NS, NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
		#ifdef POLYCAST5_DEBUG
        	ESP_LOGI(TAG, "No stored signals found in NVS");
        #endif
        
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    // Load num_stored_signals
    uint32_t stored_count = 0;
    ret = nvs_get_u32(nvs_handle, IR_SIG_COUNT_KEY, &stored_count);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to read num_signals: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return;
    }

    // Limit to MAX_STORED_SIGNALS
    if (stored_count > MAX_STORED_SIGNALS) {
        stored_count = MAX_STORED_SIGNALS;
        #ifdef POLYCAST5_DEBUG
        	ESP_LOGW(TAG, "Capping stored signals at %d", MAX_STORED_SIGNALS);
        #endif
    }

    // Load signals
    for (size_t i = 0; i < stored_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), IR_SIG_KEY, i);
        
        // Get blob size
        size_t blob_size = 0;
        ret = nvs_get_blob(nvs_handle, key, NULL, &blob_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Couldn't get size for %s: %s",
                     key, esp_err_to_name(ret));
            continue;
        }
        
        // Make sure blob is good
        if (blob_size < sizeof(ir_signal_t) || blob_size > sizeof(ir_signal_t) + MAX_PULSES * sizeof(rmt_symbol_word_t)) 
		{
			#ifdef POLYCAST5_DEBUG
	        	ESP_LOGW(TAG, "Bad blob_size %u for %s—erasing", (unsigned)blob_size, key);
	        #endif
		    
		    nvs_erase_key(nvs_handle, key);
		    nvs_commit(nvs_handle);
		    continue;
		}
        
        // Allocate space for signal
        ir_signal_t *signal = malloc(blob_size);
        if (signal == NULL) {
            ESP_LOGE(TAG, "Failed to allocate signal %d", i);
            break;
        }
        
        // Retrieve signal
        ret = nvs_get_blob(nvs_handle, key, signal, &blob_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read signal %d: %s", i, esp_err_to_name(ret));
            free(signal);
            break;
        }
        
        // If reached initial capacity, allocate more space
        if (num_stored_signals >= stored_signals_capacity) {
            size_t new_capacity = stored_signals_capacity + INITIAL_CAPACITY;
            ir_signal_t **new_signals = realloc(stored_signals, new_capacity * sizeof(ir_signal_t *));
            if (new_signals == NULL) {
                ESP_LOGE(TAG, "Failed to resize stored_signals");
                free(signal);
                break;
            }
            stored_signals = new_signals;
            stored_signals_capacity = new_capacity;
        }
        stored_signals[num_stored_signals] = signal;
        num_stored_signals++;
    }
	
	// Close NVS
    nvs_close(nvs_handle);
}

void infrared_delete_stored_signal(size_t index) {
	// Check if in bounds
    if (index >= num_stored_signals) {
        ESP_LOGE(TAG, "Invalid signal index %d, max %d", index, num_stored_signals - 1);
        return;
    }

    // Free the signal
    free(stored_signals[index]);
    
    #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Deleted signal %d from SRAM", index);
    #endif
    

    // Shift remaining signals
    for (size_t i = index; i < num_stored_signals - 1; i++) {
        stored_signals[i] = stored_signals[i + 1];
    }
    num_stored_signals--;

    // Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(IR_SIG_NS, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    // Rewrite all signals with new indices and true blob size
    for (size_t i = 0; i < num_stored_signals; i++) {
		ir_signal_t *sig = stored_signals[i];
		
        // Header + length * sizeof(pulse)
        size_t blob_size = sizeof(ir_signal_t) + sig->length * sizeof(rmt_symbol_word_t);
                         
        char key[16];
        snprintf(key, sizeof(key), IR_SIG_KEY, i);
        ret = nvs_set_blob(nvs_handle, key, stored_signals[i], blob_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save signal %d: %s", i, esp_err_to_name(ret));
        }
    }

    // Clear any leftover signal keys
    char key[16];
    snprintf(key, sizeof(key), IR_SIG_KEY, num_stored_signals);
    ret = nvs_erase_key(nvs_handle, key);
	if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
	  ESP_LOGE(TAG, "Erase key %s failed: %s", key, esp_err_to_name(ret));
	}

    // Update num_signals
    ret = nvs_set_u32(nvs_handle, IR_SIG_COUNT_KEY, num_stored_signals);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save num_signals: %s", esp_err_to_name(ret));
    }
	
	// Write changes
    ret = nvs_commit(nvs_handle);
	if (ret != ESP_OK) {
	  ESP_LOGE(TAG, "Commit failed: %s", esp_err_to_name(ret));
	}
    
    // Close NVS
    nvs_close(nvs_handle);
}
