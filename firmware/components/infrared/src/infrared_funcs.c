#include "string.h"

#include "nvs_flash.h"

#include "esp_log.h"

#include "infrared_funcs.h"
#include "infrared_task.h"

#define SIGNAL_MIN_NS 1000
#define SIGNAL_MAX_NS 15000000 // 15ms

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
    ESP_LOGI(TAG, "NVS initialized");
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
        ESP_LOGD(TAG, "RX last symbol: level0=%d, duration0=%d, level1=%d, duration1=%d",
                 ir_signal[len - 1].level0, ir_signal[len - 1].duration0,
                 ir_signal[len - 1].level1, ir_signal[len - 1].duration1);
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

	ESP_LOGI(TAG, "TX symbol: level0=%d, duration0=%d, level1=%d, duration1=%d",
			 signal[length - 1].level0, signal[length - 1].duration0,
			 signal[length - 1].level1, signal[length - 1].duration1);

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
        ESP_LOGI(TAG, "Transmission complete (%d pulses)", length);
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
    
    ESP_LOGD(TAG, "Resized stored_signals to %zu", new_cap);
    return true;
}

void infrared_save_stored_signal(void) {
	// Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("ir_storage", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    // Save signal
    char key[16];
    snprintf(key, sizeof(key), "signal_%d", num_stored_signals);
    ret = nvs_set_blob(nvs_handle, key, stored_signals[num_stored_signals], sizeof(ir_signal_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save signal %d: %s", num_stored_signals, esp_err_to_name(ret));
    }

    // Save num_stored_signals
    ret = nvs_set_u32(nvs_handle, "num_signals", num_stored_signals + 1);
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
    esp_err_t ret = nvs_open("ir_storage", NVS_READONLY, &nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored signals found in NVS");
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    // Load num_stored_signals
    uint32_t stored_count = 0;
    ret = nvs_get_u32(nvs_handle, "num_signals", &stored_count);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to read num_signals: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return;
    }

    // Limit to MAX_STORED_SIGNALS
    if (stored_count > MAX_STORED_SIGNALS) {
        stored_count = MAX_STORED_SIGNALS;
        ESP_LOGW(TAG, "Capping stored signals at %d", MAX_STORED_SIGNALS);
    }

    // Load signals
    for (size_t i = 0; i < stored_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "signal_%d", i);
        size_t length = sizeof(ir_signal_t);
        
        // Allocate space for signal
        ir_signal_t *signal = malloc(sizeof(ir_signal_t));
        if (signal == NULL) {
            ESP_LOGE(TAG, "Failed to allocate signal %d", i);
            break;
        }
        
        // Retrieve signal
        ret = nvs_get_blob(nvs_handle, key, signal, &length);
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
    ESP_LOGI(TAG, "Deleted signal %d from SRAM", index + 1);

    // Shift remaining signals
    for (size_t i = index; i < num_stored_signals - 1; i++) {
        stored_signals[i] = stored_signals[i + 1];
    }
    num_stored_signals--;

    // Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("ir_storage", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    // Rewrite all signals with new indices
    for (size_t i = 0; i < num_stored_signals; i++) {
        char key[16];
        snprintf(key, sizeof(key), "signal_%d", i);
        ret = nvs_set_blob(nvs_handle, key, stored_signals[i], sizeof(ir_signal_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save signal %d: %s", i, esp_err_to_name(ret));
        }
    }

    // Clear any leftover signal keys
    char key[16];
    snprintf(key, sizeof(key), "signal_%d", num_stored_signals);
    ret = nvs_erase_key(nvs_handle, key);
	if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
	  ESP_LOGE(TAG, "Erase key %s failed: %s", key, esp_err_to_name(ret));
	}

    // Update num_signals
    ret = nvs_set_u32(nvs_handle, "num_signals", num_stored_signals);
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

void infrared_clear_stored_signals(void) {
	
    // Free all signals
    if (stored_signals != NULL) {
        for (size_t i = 0; i < num_stored_signals; i++) {
            if (stored_signals[i] != NULL) {
                free(stored_signals[i]);
                stored_signals[i] = NULL;
            }
        }
        free(stored_signals);
        stored_signals = calloc(stored_signals_capacity, sizeof(ir_signal_t *));;
        stored_signals_capacity = INITIAL_CAPACITY;
        num_stored_signals = 0;
    }

    // Open NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("ir_storage", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }
	
	// Clear NVS
    ret = nvs_erase_all(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
    }
	
	// Write changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

	// Close NVS
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Cleared all signals from NVS");
}