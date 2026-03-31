#include "esp_err.h"
#include "polycast5_macros.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "nvs.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"

#include "portmacro.h"
#include "wifi_utils.h"
#include "wifi_mqtt.h"
#include "wifi_task.h"
#include "wifi_ota_update.h"

#define TAG "WIFI_OTA_UPDATE"

#define NVS_OTA_VERSION_NS "ota"
#define NVS_OTA_VERSION_KEY "version"

//#define OTA_CHECK_PROJ_DESC 1 // Enables project_description.json version check (redundant)

POLYCAST5_USE_PSRAM_BSS char ota_update_url[512] = {0};
POLYCAST5_USE_PSRAM_BSS char ota_update_info[512] = {0};

POLYCAST5_USE_PSRAM_BSS static char url_buf[1024]; // URL buffer
static TaskHandle_t ota_task_handle = NULL;

static TaskHandle_t ota_check_task_handle = NULL; // Only one check at a time
POLYCAST5_USE_PSRAM_BSS static char manifest_url_buf[512]; // Manifest URL buffer

static char pending_manifest_ver[64];
static int manifest_size_bytes = -1;

static void ota_task(void *_)
{
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Pre-OTA heap: free=%u, internal=%u, min=%u",
            esp_get_free_heap_size(),
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            esp_get_minimum_free_heap_size());
#endif

    // Free some internal heap
    wifi_mqtt_client_destroy();

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Pre-OTA heap after MQTT deinit: free=%u, internal=%u, min=%u",
            esp_get_free_heap_size(),
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            esp_get_minimum_free_heap_size());
#endif
    
    // Configure the HTTP(S) client
    esp_http_client_config_t http_cfg = {
        .url = url_buf,
        .timeout_ms = 30000, // Timeout
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .buffer_size = 2048, // Header RX buffer
        .buffer_size_tx = 2048, // Request TX buffer
        .keep_alive_enable = false,
        //.user_agent = "esp-idf-ota/1.0",
    };
    
    // Pass those HTTP settings into the higher-level OTA engine
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg
    };

    esp_https_ota_handle_t h = NULL;
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "OTA total size: %d", manifest_size_bytes);
#endif
    
    // Opens the URL, validates TLS, selects the inactive OTA partition, and prepares to stream
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin error: %s", esp_err_to_name(err));
        pending_manifest_ver[0] = '\0'; // Clear pending version
        goto out;
    }

#ifdef OTA_CHECK_PROJ_DESC
    /* Reject if same version */
    // Already checked above in ota_check_task
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
#endif

    /* Download and write loop */
    size_t last = 0;
    int last_pct = -1;
    
    // While updating:
    // Stream chunks from the server and write them directly into flash at the inactive OTA slot
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        // Log data read so far
        size_t read = esp_https_ota_get_image_len_read(h);
        
        // If was able to get size of new OTA bin, log %s
        if (manifest_size_bytes > 0) {
            // Show smooth %
            int pct = (int)((read * 100ULL) / (unsigned)manifest_size_bytes);
            
            // Only when it changes
            if (pct != last_pct) {
                last_pct = pct;

                if (pct > 100) {
                    pct = 100;
                }
                if (pct < 0) {
                    pct = 0;
                }
                
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Update %d%% (%u/%d)", pct, (unsigned)read, manifest_size_bytes);
#endif
            }

            // Send percentage to LCD
            xQueueSend(xWifiOtaPctQueue, &pct, 0);
        } else { // Fallback when content length is unknown: every ~64KB
            if (read - last >= 64 * 1024) {
                last = read;
                
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Downloaded %u B", (unsigned)read);
#endif
            }
        }
        
        // Yield CPU time for UI
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // If error
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_perform error: %s", esp_err_to_name(err));
        
        // Abort
        pending_manifest_ver[0] = '\0'; // Clear pending version
        esp_https_ota_abort(h);
        goto out;
    }

    // If OTA completed successfully
    err = esp_https_ota_finish(h);
    if (err == ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "OTA OK, rebooting...");
#endif

        // Save the new version to NVS if exists
        if (pending_manifest_ver[0]) {
            err = wifi_ota_update_set_nvs_version(pending_manifest_ver);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_ota_update_set_nvs_version failed: %s", esp_err_to_name(err));
            } else {
#ifdef POLYCAST5_DEBUG
                ESP_LOGI(TAG, "Saved new FW version to NVS: %s", pending_manifest_ver);
#endif
            }
        }
        
        int done = -1;
        // Send OTA success to LCD
        xQueueSend(xWifiOtaPctQueue, &done, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart(); // Restart
    } else {
        ESP_LOGE(TAG, "ota_finish error: %s", esp_err_to_name(err));
        pending_manifest_ver[0] = '\0'; // Clear pending version
    }
    
    // Abort process
    out:
    ESP_LOGE(TAG, "OTA error! Restarting: %s", esp_err_to_name(err));

    int fail = -2;
    // Send OTA failed to LCD
    xQueueSend(xWifiOtaPctQueue, &fail, pdMS_TO_TICKS(100));

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart(); // Restart

    // Fallback (should not reach here after restart)
    ota_task_handle = NULL;
    vTaskDelete(NULL);
}

static bool http_get_small(const char *url, char *out, size_t out_sz)
{
    // Build HTTP client config
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false, // One fetch
    };

    // Create the client
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { // Abort on failure
        return false;
    }
    
    // Opens the connection and sends a plain GET request
    if (esp_http_client_open(h, 0) != ESP_OK) { // Abort on failure
        esp_http_client_cleanup(h);
        return false;
    }

    // Reads response headers and returns content length if present
    int cl = esp_http_client_fetch_headers(h);

    // Reject non-200 responses
    int status = esp_http_client_get_status_code(h);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "http_get_small: HTTP %d", status);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return false;
    }

    // If the server does report a length and it won't fit, abort
    if (cl > 0 && cl >= (int)out_sz) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return false;
    }

    // Read the response body in a loop
    int n = 0, r;
    while ((r = esp_http_client_read(h, out + n, out_sz - 1 - n)) > 0) { // Returns bytes read while there is data
        n += r;
    }
    out[n] = '\0'; // NUL-terminator

    // Clean up and return
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return n > 0; // True if bytes read
}

static void ota_check_task(void *_)
{
    // Free heap before HTTPS manifest fetch
    wifi_mqtt_client_stop();

    POLYCAST5_USE_PSRAM_BSS static char buf[2048];
    memset(buf, 0, sizeof(buf));
    
    // Get the full, safe string for parsing
    // (download manifest.json into buf)
    if (!http_get_small(manifest_url_buf, buf, sizeof buf)) {
        ESP_LOGE(TAG, "Manifest fetch failed");
        goto done;
    }
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "http_get_small string='%s'", buf);
#endif

    // Parse the JSON text into a DOM
    cJSON *root = cJSON_Parse(buf);
    if (!root) { // Abort on fail
        ESP_LOGE(TAG, "Manifest JSON parse failed");
        goto done;
    }

    // Extract version and url fields
    const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *jsize = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON *jinfo = cJSON_GetObjectItemCaseSensitive(root, "info");

    // Get new OTA .bin size
    manifest_size_bytes = (cJSON_IsNumber(jsize) && jsize->valuedouble > 0) ? (int)jsize->valuedouble : -1;
    
    // Validate strings are JSON strings
    if (!cJSON_IsString(jver) || !cJSON_IsString(jurl) || !cJSON_IsString(jinfo)) { // Abort on fail
        ESP_LOGE(TAG, "Manifest missing version/url");
        cJSON_Delete(root);
        goto done;
    }

    // Read the current app's version
    char current_ver[64] = {0};
    esp_err_t err = wifi_ota_update_get_nvs_version(current_ver, sizeof(current_ver));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_ota_update_get_nvs_version failed: %s", esp_err_to_name(err));
    }
    const char *new_ver = jver->valuestring;
    strlcpy(ota_update_url, jurl->valuestring, sizeof(ota_update_url));

    // Copy update info to global buffer
    strlcpy(ota_update_info, jinfo->valuestring, sizeof(ota_update_info));
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "ota_update_info: %s", ota_update_info);
    ESP_LOGI(TAG, "Current ver: %s | Available: %s", current_ver, new_ver);
#endif

    // If different version -> update from extracted URL
    if (strcmp(current_ver, new_ver) != 0) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "New version found -> Considering OTA from %s", ota_update_url);
#endif

        // Save the pending version to global buffer
        strlcpy(pending_manifest_ver, new_ver, sizeof(pending_manifest_ver));
        
        // OTA update is available
        xEventGroupSetBits(xWifiEventGroup, WIFI_OTA_AVAILABLE_BIT);
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Already up-to-date");
#endif
        // No OTA available
        xEventGroupClearBits(xWifiEventGroup, WIFI_OTA_AVAILABLE_BIT);

        pending_manifest_ver[0] = '\0'; // Clear pending version
        ota_update_url[0] = '\0';
        ota_update_info[0] = '\0';
    }

    // Clean up and exit
    cJSON_Delete(root);

    done:
    wifi_mqtt_client_start(); // Bring back MQTT for normal operation
    ota_check_task_handle = NULL;
    vTaskDelete(NULL);
}

bool wifi_ota_update_check_start(const char *manifest_url)
{    
    // If already checked or in progress, exit
    if (ota_check_task_handle || wifi_ota_update_in_progress()) {
        return false;
    }
    
    // Check manifest_url is valid
    if (!manifest_url || !manifest_url[0]) {
        return false;
    }
    
    // Copy into global buffer
    strlcpy(manifest_url_buf, manifest_url, sizeof(manifest_url_buf));
    
    // Create OTA check task
    return xTaskCreate(ota_check_task, "ota_check_task", 5 * 1024, NULL, POLYCAST5_PRIORITY_HIGH, &ota_check_task_handle) == pdPASS;
}

bool wifi_ota_update_start(const char *url)
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
    if (xTaskCreate(ota_task, "ota_task", 8 * 1024, NULL, POLYCAST5_PRIORITY_HIGH, &ota_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Create ota_task failed");
        ota_task_handle = NULL;
        return false;
    }
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Starting OTA: %s", url);
#endif
    return true;
}

#ifdef OTA_CHECK_PROJ_DESC
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
#endif

bool wifi_ota_update_in_progress(void)
{
    return ota_task_handle != NULL;
}

void wifi_ota_update_mark_app_valid(void)
{
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Marked app valid");
    }
#else
    ESP_LOGE(TAG, "APP ROLLBACK NOT ENABLED: Please enable in menuconfig");
#endif
}

esp_err_t wifi_ota_update_set_nvs_version(const char *val)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(NVS_OTA_VERSION_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Set the version string
    err = nvs_set_str(h, NVS_OTA_VERSION_KEY, val);
    
    // Persist changes if success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    
    // Close and return
    nvs_close(h);
    return err;
}

esp_err_t wifi_ota_update_get_nvs_version(char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(NVS_OTA_VERSION_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t len = out_sz; // Must include room for '\0'
    
    // Get the saved version string
    err = nvs_get_str(h, NVS_OTA_VERSION_KEY, out, &len);
    
    // Close and return
    nvs_close(h);
    return err;
}

