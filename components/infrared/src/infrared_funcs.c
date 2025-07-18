#include "polycast5_macros.h"

#include <string.h>

#include "nvs_flash.h"

#include "esp_log.h"

#include "infrared_funcs.h"
#include "infrared_task.h"

#define SIGNAL_MIN_NS 1000
#define SIGNAL_MAX_NS 15000000 // 15ms


// Globals
extern rmt_symbol_word_t ir_signal[MAX_PULSES]; // The signal

extern volatile bool restart_rx_pending;

size_t ir_signal_length;

// Statics
static const char *TAG = "IR_FUNCS";

static rmt_channel_handle_t rx_channel = NULL;
static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t tx_encoder = NULL;

// When IR signal received
static bool IRAM_ATTR infrared_rx_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
	// Get len
	size_t len = edata->num_symbols;
	
	// Cap signal
	if (len > MAX_PULSES) {
		len = MAX_PULSES;
	}
	
	// Make sure an actual signal (not random IR)
	if (len < MIN_VALID_PULSES) {
		restart_rx_pending = true;
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		return (xHigherPriorityTaskWoken == pdTRUE);
	}
	
	// Copy received signal into ir_signal
	memcpy(ir_signal, edata->received_symbols, len * sizeof(rmt_symbol_word_t));
	
	// Copy length
	ir_signal_length = len;

	/*#ifdef POLYCAST5_DEBUG
		ESP_LOGD(TAG, "RX last symbol: level0=%d, duration0=%d, level1=%d, duration1=%d",
			 ir_signal[len - 1].level0, ir_signal[len - 1].duration0,
			 ir_signal[len - 1].level1, ir_signal[len - 1].duration1);
	#endif*/

	// Notify semaphore that a signal was received
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(xInfraredRxEventSemaphore, &xHigherPriorityTaskWoken);
	return (xHigherPriorityTaskWoken == pdTRUE);
}

void infrared_init_rx(void)
{
	// Configure rmt RX channel
	rmt_rx_channel_config_t rx_config = {
		.gpio_num = RMT_RX_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RMT_RESOLUTION_HZ,
		.mem_block_symbols = PULSE_BLOCK,
	};
	
	// Create new
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

void infrared_init_tx(void)
{
	// Configure rmt TX channel
	rmt_tx_channel_config_t tx_config = {
		.gpio_num = RMT_TX_GPIO,
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = RMT_RESOLUTION_HZ,
		.mem_block_symbols = PULSE_BLOCK,
		.trans_queue_depth = 1,
	};
	
	// Create new
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

void infrared_restart_rx(void)
{
	// Ensure initialized
	if (rx_channel == NULL) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGE(TAG, "Cannot restart RX: channel not initialized");
		#endif
		
		return;
	}
	
	// Disable channel
	rmt_disable(rx_channel);
	
	// Re-enable
	ESP_ERROR_CHECK(rmt_enable(rx_channel));
	
	// Re-apply configuration
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
		#ifdef POLYCAST5_DEBUG
			ESP_LOGE(TAG, "Cannot disable RX: channel not initialized");
		#endif
		
		return;
	}
	
	// Disable channel
	rmt_disable(rx_channel);
}

void infrared_transmit_ir(rmt_symbol_word_t *signal, size_t length)
{
	// Error checks
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
}

bool infrared_ensure_capacity(void)
{
	size_t total = 0;
	
	// Add up signals for all remotes
	for (size_t r = 0; r < num_remotes; r++) {
		total += remotes[r].num_signals;
	}
	
	// Check if at max
	if (total < MAX_STORED_SIGNALS) {
		return true;
	}
	
	#ifdef POLYCAST5_DEBUG
		ESP_LOGW(TAG, "Storage full (%zu signals)", total);
	#endif
	
	return false;
}

void infrared_nvs_load_remotes(void)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t ret = nvs_open(IR_NS, NVS_READONLY, &h);
	
	// Error check
	if (ret == ESP_ERR_NVS_NOT_FOUND) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "No stored remotes found in NVS");
		#endif
		
		// Default
		num_remotes = 1;
		current_remote = 0;
		remotes[0].name = strdup("REMOTE");
		remotes[0].num_signals = 0;
		remotes[0].signals = NULL;
		remotes[0].signal_names = NULL;
		return;
	}
	
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
		return;
	}

	// Load num_remotes
	uint8_t stored_num = 1; // 1 default
	ret = nvs_get_u8(h, IR_NUM_REMOTES_KEY, &stored_num);
	
	// Check
	if (ret != ESP_OK) {
		if (ret != ESP_ERR_NVS_NOT_FOUND) {
			ESP_LOGE(TAG, "Failed to read num_remotes: %s", esp_err_to_name(ret));
		}
		
		nvs_close(h);
		return;
	}
	
	// Assign extracted to global
	num_remotes = stored_num;
	
	// Cap at max
	if (num_remotes > MAX_REMOTES) {
		num_remotes = MAX_REMOTES;
		
		#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Capping remotes at %d", MAX_REMOTES);
		#endif
	}
	
	// Make sure at least one default
	if (num_remotes == 0) {
		num_remotes = 1;
	}

	// Load each remote
	for (size_t r = 0; r < num_remotes; r++) {
		char key[32];

		// Load name
		size_t len = 0;
		sprintf(key, IR_REMOTE_NAME_FMT, (int)r); // Format key
		
		// Get length
		ret = nvs_get_str(h, key, NULL, &len);
		
		// Check
		if (ret != ESP_OK) {
			remotes[r].name = strdup("REMOTE");
		}
		else {
			// If found allocate length
			remotes[r].name = malloc(len);
			
			// Get the actual remote name and save
			nvs_get_str(h, key, remotes[r].name, &len);
		}

		// Load num_signals
		uint32_t nsig = 0;
		
		// Format key
		sprintf(key, IR_REMOTE_NSIG_FMT, (int)r);
		ret = nvs_get_u32(h, key, &nsig); // Get num_signals
		
		// Assign default if DNE 
		if (ret != ESP_OK) {
			nsig = 0;
		}
		
		// Save to struct
		remotes[r].num_signals = nsig;
		remotes[r].signals = malloc(nsig * sizeof(ir_signal_t *)); // Allocate signal length
		remotes[r].signal_names = malloc(nsig * sizeof(char *)); // Allocate signal name

		// Load signals for remote
		for (size_t s = 0; s < nsig; s++) {
			// Format name key
			sprintf(key, IR_SIGNAL_NAME_FMT, (int)r, (int)s);
			
			len = 0;
			ret = nvs_get_str(h, key, NULL, &len); // Get name length
			
			// Save name if exists
			if (ret == ESP_OK) {
				remotes[r].signal_names[s] = malloc(len); // Allocate for name
				nvs_get_str(h, key, remotes[r].signal_names[s], &len); // Save
			}
			else {
				remotes[r].signal_names[s] = strdup("");
			}

			// Get actual signal
			// Format key
			sprintf(key, IR_SIGNAL_BLOB_FMT, (int)r, (int)s);
			
			size_t blob_size = 0;
			ret = nvs_get_blob(h, key, NULL, &blob_size); // Get signal size
			
			// If bad blob, skip
			if (ret != ESP_OK || blob_size < sizeof(ir_signal_t)) {
				continue;
			}
			
			// Allocate space for signal
			ir_signal_t *sig = malloc(blob_size);
			
			// Get the signal
			ret = nvs_get_blob(h, key, sig, &blob_size);
			
			// Save to struct if good
			if (ret == ESP_OK) {
				remotes[r].signals[s] = sig;
			}
			// Else bad
			else {
				free(sig);
			}
		}
	}

	// Close NVS
	nvs_close(h);
}

void infrared_nvs_save_signal_to_remote(size_t remote_idx, size_t sig_idx, ir_signal_t *sig, const char *name)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t ret = nvs_open(IR_NS, NVS_READWRITE, &h);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
		return;
	}

	// Format name key
	char key[32];
	sprintf(key, IR_SIGNAL_NAME_FMT, (int)remote_idx, (int)sig_idx);
	
	// Save name
	ret = nvs_set_str(h, key, name);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to save signal name %zu for remote %zu: %s", sig_idx, remote_idx, esp_err_to_name(ret));
	}

	// Format signal key
	sprintf(key, IR_SIGNAL_BLOB_FMT, (int)remote_idx, (int)sig_idx);
	size_t blob_size = sizeof(ir_signal_t) + (sig->length * sizeof(rmt_symbol_word_t));
	
	// Save signal blob
	ret = nvs_set_blob(h, key, sig, blob_size);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to save signal blob %zu for remote %zu: %s", sig_idx, remote_idx, esp_err_to_name(ret));
	}

	// Commit changes
	nvs_commit(h);
	
	// Close NVS
	nvs_close(h);
}

void infrared_nvs_save_remote_nsig(size_t remote_idx) {
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t ret = nvs_open(IR_NS, NVS_READWRITE, &h);
	if (ret != ESP_OK) {
		return;
	}
	
	// Format num_signals key
	char key[32];
	sprintf(key, IR_REMOTE_NSIG_FMT, (int)remote_idx);
	
	// Save num_signals
	ret = nvs_set_u32(h, key, (uint32_t)remotes[remote_idx].num_signals);
	
	// Commit changes
	nvs_commit(h);
	
	// Close NVS
	nvs_close(h);
}

void infrared_nvs_save_remote_name(size_t remote_idx)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t ret = nvs_open(IR_NS, NVS_READWRITE, &h);
	if (ret != ESP_OK) {
		return;
	}
	
	// Format remote name key
	char key[32];
	sprintf(key, IR_REMOTE_NAME_FMT, (int)remote_idx);
	
	// Save name to NVS
	ret = nvs_set_str(h, key, remotes[remote_idx].name);
	
	// Commit changes
	nvs_commit(h);
	
	// Close NVS
	nvs_close(h);
}

void infrared_nvs_save_all_remotes(void)
{
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t ret = nvs_open(IR_NS, NVS_READWRITE, &h);
	if (ret != ESP_OK) {
		return;
	}

	// Erase everything
	nvs_erase_all(h);
	nvs_commit(h); // Save

	// Save num_remotes
	nvs_set_u8(h, IR_NUM_REMOTES_KEY, (uint8_t)num_remotes);

	char key[32];
	for (size_t r = 0; r < num_remotes; r++) {
		// Format remote name key
		sprintf(key, IR_REMOTE_NAME_FMT, (int)r);
		nvs_set_str(h, key, remotes[r].name); // Save

		// Format num_signals key
		sprintf(key, IR_REMOTE_NSIG_FMT, (int)r);
		nvs_set_u32(h, key, (uint32_t)remotes[r].num_signals); // Save

		// Save all signals
		for (size_t s = 0; s < remotes[r].num_signals; s++) {
			// Format signals name key
			sprintf(key, IR_SIGNAL_NAME_FMT, (int)r, (int)s);
			nvs_set_str(h, key, remotes[r].signal_names[s]); // Save

			// Format signals key
			sprintf(key, IR_SIGNAL_BLOB_FMT, (int)r, (int)s);
			
			// Get signal size
			ir_signal_t *sig = remotes[r].signals[s];
			size_t blob_size = sizeof(ir_signal_t) + (sig->length * sizeof(rmt_symbol_word_t));
			nvs_set_blob(h, key, sig, blob_size); // Save
		}
	}

	// Commit changes
	nvs_commit(h);
	
	// Close NVS
	nvs_close(h);
}

void infrared_nvs_delete_signal_from_remote(size_t remote_idx, size_t sig_idx)
{
	// Ensure valid signal
	if (remote_idx >= num_remotes || sig_idx >= remotes[remote_idx].num_signals) {
		ESP_LOGE(TAG, "Invalid delete: remote %zu, sig %zu", remote_idx, sig_idx);
		return;
	}

	// Free the signal and name from heap
	free(remotes[remote_idx].signals[sig_idx]);
	free(remotes[remote_idx].signal_names[sig_idx]);

	size_t ns = remotes[remote_idx].num_signals; // Number of signals for remote
	
	// Shift arrays down one
	for (size_t i = sig_idx; i < ns - 1; i++) {
		// Shift signal and name
		remotes[remote_idx].signals[i] = remotes[remote_idx].signals[i + 1];
		remotes[remote_idx].signal_names[i] = remotes[remote_idx].signal_names[i + 1];
	}
	
	// One less
	remotes[remote_idx].num_signals--;

	// Realloc signals and names
	remotes[remote_idx].signals = realloc(remotes[remote_idx].signals, remotes[remote_idx].num_signals * sizeof(ir_signal_t *));
	remotes[remote_idx].signal_names = realloc(remotes[remote_idx].signal_names, remotes[remote_idx].num_signals * sizeof(char *));

	/* Update NVS with new indexes */
	nvs_handle_t h;
	
	// Open NVS
	esp_err_t ret = nvs_open(IR_NS, NVS_READWRITE, &h);
	if (ret != ESP_OK) {
		return;
	}

	char key[32];
	size_t old_ns = ns; // Previous num_signals
	
	// Erase all signals and names
	sprintf(key, IR_SIGNAL_NAME_FMT, (int)remote_idx, (int)(old_ns - 1));
	nvs_erase_key(h, key);
	sprintf(key, IR_SIGNAL_BLOB_FMT, (int)remote_idx, (int)(old_ns - 1));
	nvs_erase_key(h, key);

	// Rewrite current signals and names
	for (size_t i = 0; i < remotes[remote_idx].num_signals; i++) {
		// Format and save name
		sprintf(key, IR_SIGNAL_NAME_FMT, (int)remote_idx, (int)i);
		nvs_set_str(h, key, remotes[remote_idx].signal_names[i]);

		// Format and save signal
		sprintf(key, IR_SIGNAL_BLOB_FMT, (int)remote_idx, (int)i);
		ir_signal_t *sig = remotes[remote_idx].signals[i];
		size_t blob_size = sizeof(ir_signal_t) + (sig->length * sizeof(rmt_symbol_word_t));
		nvs_set_blob(h, key, sig, blob_size);
	}

	// Update num_signals
	sprintf(key, IR_REMOTE_NSIG_FMT, (int)remote_idx); // Format key
	nvs_set_u32(h, key, (uint32_t)remotes[remote_idx].num_signals); // Save

	// Commit changes
	nvs_commit(h);
	
	// Close NVS
	nvs_close(h);

	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Deleted signal %zu from remote %zu", sig_idx, remote_idx);
	#endif
}

void infrared_nvs_delete_remote(size_t remote_idx)
{
	// Ensure valid
	if (remote_idx >= num_remotes) {
		ESP_LOGE(TAG, "Invalid remote delete: %zu", remote_idx);
		return;
	}
	
	// Free resources
	for (size_t s = 0; s < remotes[remote_idx].num_signals; s++) {
		free(remotes[remote_idx].signals[s]);
		free(remotes[remote_idx].signal_names[s]);
	}
	free(remotes[remote_idx].signals);
	free(remotes[remote_idx].signal_names);
	free(remotes[remote_idx].name);

	// Shift remotes over one
	for (size_t k = remote_idx; k < num_remotes - 1; k++) {
		remotes[k] = remotes[k + 1];
	}
	num_remotes--;

	// Resave to NVS
	infrared_nvs_save_all_remotes();
}

void infrared_clear_nvs(void)
{
	nvs_handle_t h;
	
	// Clear all NVS
	if (nvs_open(IR_NS, NVS_READWRITE, &h) == ESP_OK) {
		nvs_erase_all(h);
		nvs_commit(h);
		nvs_close(h);
	}
}