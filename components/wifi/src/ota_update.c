#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"

#include "ota_update.h"

#define TAG "OTA"

static char url_buf[1024]; // URL buffer
static TaskHandle_t ota_task_handle = NULL;

static void log_versions(const esp_app_desc_t *running, const esp_app_desc_t *incoming)
{
	// Avoid NULL deref if metadata fetch fails
	if (!running || !incoming) {
		return;
	}
	
	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Running : %s (%s %s)", running->version, running->date, running->time);
	ESP_LOGI(TAG, "Incoming : %s (%s %s)", incoming->version, incoming->date, incoming->time);
	#endif
}

static void ota_task(void *_)
{
	// Configure the HTTP(S) client
	esp_http_client_config_t http_cfg = {
		.url = url_buf,
		.timeout_ms = 30000, // Timeout
		.crt_bundle_attach = esp_crt_bundle_attach,
		.skip_cert_common_name_check = false,
		.buffer_size = 4096, // Header RX buffer
		.buffer_size_tx = 4096, // Request TX buffer
		.keep_alive_enable = true,
		//.user_agent = "esp-idf-ota/1.0",
	};
	
	// Pass those HTTP settings into the higher-level OTA engine
	esp_https_ota_config_t ota_cfg = {
		.http_config = &http_cfg
	};

	esp_https_ota_handle_t h = NULL;
	
	// Opens the URL, validates TLS, selects the inactive OTA partition, and prepares to stream
	esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "ota_begin error: %s", esp_err_to_name(err));
		goto out;
	}

	/* Reject if same version */
	const esp_app_desc_t *running = esp_app_get_description();
	esp_app_desc_t incoming = {0};
	
	// Get destination
	if (esp_https_ota_get_img_desc(h, &incoming) == ESP_OK) {
		log_versions(running, &incoming);
		
		// If running matches incoming
		if (running && strncmp(running->version, incoming.version, sizeof incoming.version) == 0) {
			ESP_LOGE(TAG, "Same version, aborting OTA");
			
			// Abort
			esp_https_ota_abort(h);
			goto out;
		}
	}

	/* Download and write loop */
	size_t last = 0;
	
	// While updating:
	// Stream chunks from the server and write them directly into flash at the inactive OTA slot
	while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
		// Log data read so far
		size_t read = esp_https_ota_get_image_len_read(h);
		
		// Progress logging every ~64 KB: keeps logs readable
		if (read - last >= 64 * 1024) {
			last = read;
			
			#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "Downloaded %u B", (unsigned)read);
			#endif
		}
		
		// Yield CPU time
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	
	// If error
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "ota_perform error: %s", esp_err_to_name(err));
		
		// Abort
		esp_https_ota_abort(h);
		goto out;
	}

	// If OTA completed successfully
	err = esp_https_ota_finish(h);
	if (err == ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "OTA OK, rebooting...");
		#endif
		
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart(); // Restart
	}
	else {
		ESP_LOGE(TAG, "ota_finish error: %s", esp_err_to_name(err));
	}
	
	// Abort process
	out:
	ota_task_handle = NULL;
	vTaskDelete(NULL);
}

bool ota_update_start(const char *url)
{
	// Make sure not already running
	if (ota_task_handle) {
		ESP_LOGE(TAG, "OTA already running");
		return false;
	}
	
	// Validate URL pointer
	if (!url || !url[0]) {
		ESP_LOGE(TAG, "Invalid OTA URL");
		return false;
	}
	
	// Copy URL into global buffer
	strlcpy(url_buf, url, sizeof(url_buf));
	
	// Create OTA task
	if (xTaskCreate(ota_task, "ota_task", 6 * 1024, NULL, tskIDLE_PRIORITY + 2, &ota_task_handle) != pdPASS) {
		ESP_LOGE(TAG, "Create ota_task failed");
		ota_task_handle = NULL;
		return false;
	}
	
	#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Starting OTA: %s", url);
	#endif
	return true;
}

bool ota_update_in_progress(void)
{
	return ota_task_handle != NULL;
}

void ota_update_mark_app_valid(void) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
	esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
	if (err == ESP_OK) ESP_LOGI(TAG, "Marked app valid");
#endif
}
