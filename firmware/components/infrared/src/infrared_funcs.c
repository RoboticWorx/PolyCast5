#include "infrared_funcs.h"
#include "esp_log.h"
#include "string.h"
#include "nvs_flash.h"

static const char *TAG = "IR_FUNCS";

// Global variables
ir_signal_t **stored_signals = NULL;
size_t num_stored_signals = 0;
size_t stored_signals_capacity = 0;
rmt_symbol_word_t ir_signal[MAX_PULSES];
size_t ir_signal_length = 0;
SemaphoreHandle_t ir_rx_sem = NULL;

rmt_channel_handle_t rx_channel = NULL;
rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t tx_encoder = NULL;

volatile bool is_transmitting = false;

static bool IRAM_ATTR rmt_rx_callback(rmt_channel_handle_t channel,
                                      const rmt_rx_done_event_data_t *edata,
                                      void *user_data) {
    if (is_transmitting) {
        ESP_LOGD(TAG, "RX ISR ignored: transmission in progress");
        return false;
    }

    size_t len = edata->num_symbols;
    if (len > MAX_PULSES) {
        len = MAX_PULSES;
    }
    memcpy(ir_signal, edata->received_symbols, len * sizeof(rmt_symbol_word_t));
    ir_signal_length = len;

    // Log last symbol
    if (len > 0) {
        ESP_LOGD(TAG, "RX last symbol: level0=%d, duration0=%d, level1=%d, duration1=%d",
                 ir_signal[len - 1].level0, ir_signal[len - 1].duration0,
                 ir_signal[len - 1].level1, ir_signal[len - 1].duration1);
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(ir_rx_sem, &xHigherPriorityTaskWoken);
    return (xHigherPriorityTaskWoken == pdTRUE);
}

void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
}

void load_stored_signals(void) {
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
        ESP_LOGW(TAG, "Truncating stored signals to %d", MAX_STORED_SIGNALS);
    }

    // Load signals
    for (size_t i = 0; i < stored_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "signal_%d", i);
        size_t length = sizeof(ir_signal_t);
        
        ir_signal_t *signal = malloc(sizeof(ir_signal_t));
        if (signal == NULL) {
            ESP_LOGE(TAG, "Failed to allocate signal %d", i);
            break;
        }
        
        ret = nvs_get_blob(nvs_handle, key, signal, &length);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read signal %d: %s", i, esp_err_to_name(ret));
            free(signal);
            break;
        }
        
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

    nvs_close(nvs_handle);
}

void save_stored_signal(size_t index) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("ir_storage", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    // Save signal
    char key[16];
    snprintf(key, sizeof(key), "signal_%d", index);
    ret = nvs_set_blob(nvs_handle, key, stored_signals[index], sizeof(ir_signal_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save signal %d: %s", index, esp_err_to_name(ret));
    }

    // Save num_stored_signals
    ret = nvs_set_u32(nvs_handle, "num_signals", num_stored_signals);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save num_signals: %s", esp_err_to_name(ret));
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

void delete_stored_signal(size_t index) {
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

    // Update NVS
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
    nvs_erase_key(nvs_handle, key); // Ignore errors (key may not exist)

    // Update num_signals
    ret = nvs_set_u32(nvs_handle, "num_signals", num_stored_signals);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save num_signals: %s", esp_err_to_name(ret));
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Updated NVS, %d signals remain", num_stored_signals);
}

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
    esp_err_t ret = rmt_new_rx_channel(&rx_config, &rx_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RX channel: %s", esp_err_to_name(ret));
        rx_channel = NULL;
        return;
    }

    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = rmt_rx_callback,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &callbacks, NULL));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));

    rmt_receive_config_t rx_receive_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 15000000, // 15ms timeout
    };
    ESP_LOGI(TAG, "Starting RX with mem_block=%d, resolution=%dHz, max_timeout=%dns",
             PULSE_BLOCK, RMT_RESOLUTION_HZ, rx_receive_config.signal_range_max_ns);
    ESP_ERROR_CHECK(rmt_receive(rx_channel, ir_signal,
                                sizeof(rmt_symbol_word_t) * MAX_PULSES,
                                &rx_receive_config));
}

void init_tx(void) {
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

    rmt_carrier_config_t carrier_config = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33,
        .flags.polarity_active_low = true,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_config));

    rmt_copy_encoder_config_t encoder_config = {0};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &tx_encoder));
    ESP_ERROR_CHECK(rmt_enable(tx_channel));
}

void transmit_ir_signal(rmt_symbol_word_t *signal, size_t length) {
    if (signal == NULL || length == 0 || length > MAX_PULSES) {
        ESP_LOGE(TAG, "Invalid signal: null=%d, length=%d", signal == NULL, length);
        return;
    }
    if (tx_channel == NULL || tx_encoder == NULL) {
        ESP_LOGE(TAG, "TX channel or encoder not initialized");
        return;
    }

    // Log last symbol
    ESP_LOGI(TAG, "TX last symbol: level0=%d, duration0=%d, level1=%d, duration1=%d",
             signal[length - 1].level0, signal[length - 1].duration0,
             signal[length - 1].level1, signal[length - 1].duration1);

    is_transmitting = true;

    rmt_transmit_config_t tx_config = {.loop_count = 1};
    esp_err_t ret = rmt_transmit(tx_channel, tx_encoder, signal,
                                 length * sizeof(rmt_symbol_word_t), &tx_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Transmit failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Transmission complete (%d pulses)", length);
    }

    is_transmitting = false;
}

void restart_rx(void) {
    if (rx_channel == NULL) {
        ESP_LOGE(TAG, "Cannot restart RX: channel not initialized");
        return;
    }
    rmt_receive_config_t rx_config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 15000000, // 15ms timeout
    };
    ESP_LOGD(TAG, "Restarting RX with max_timeout=%dns", rx_config.signal_range_max_ns);
    ESP_ERROR_CHECK(rmt_receive(rx_channel, ir_signal,
                                sizeof(rmt_symbol_word_t) * MAX_PULSES, &rx_config));
}

void clear_stored_signals(void) {
    // Free all signals in SRAM
    if (stored_signals != NULL) {
        for (size_t i = 0; i < num_stored_signals; i++) {
            if (stored_signals[i] != NULL) {
                free(stored_signals[i]);
                stored_signals[i] = NULL;
            }
        }
        num_stored_signals = 0;
        ESP_LOGI(TAG, "Cleared all signals from SRAM");
    }

    // Clear NVS
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("ir_storage", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_erase_all(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
    }

    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    }

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Cleared all signals from NVS");
}