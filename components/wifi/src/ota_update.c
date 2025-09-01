#include "ota_update.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include <string.h>

static const char *TAG = "OTA";
static TaskHandle_t s_ota_task = NULL;
static char s_url[384];

static void log_versions(const esp_app_desc_t *running, const esp_app_desc_t *incoming) {
    if (!running || !incoming) return;
    ESP_LOGI(TAG, "Running  : %s (%s %s)", running->version, running->date, running->time);
    ESP_LOGI(TAG, "Incoming : %s (%s %s)", incoming->version, incoming->date, incoming->time);
}

static void ota_task(void *_) {
    esp_http_client_config_t http_cfg = {
        .url = s_url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err)); goto out; }

    // Optional: reject same version
    const esp_app_desc_t *running = esp_app_get_description();
    esp_app_desc_t incoming = {0};
    if (esp_https_ota_get_img_desc(h, &incoming) == ESP_OK) {
        log_versions(running, &incoming);
        if (running && strncmp(running->version, incoming.version, sizeof incoming.version) == 0) {
            ESP_LOGW(TAG, "Same version, aborting OTA");
            esp_https_ota_abort(h);
            goto out;
        }
    }

    size_t last = 0;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        size_t read = esp_https_ota_get_image_len_read(h);
        if (read - last >= 64 * 1024) { last = read; ESP_LOGI(TAG, "Downloaded %u B", (unsigned)read); }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "ota_perform: %s", esp_err_to_name(err)); esp_https_ota_abort(h); goto out; }

    err = esp_https_ota_finish(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA OK, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "ota_finish: %s", esp_err_to_name(err));
    }
out:
    s_ota_task = NULL;
    vTaskDelete(NULL);
}

bool ota_start(const char *url) {
    if (s_ota_task) { ESP_LOGW(TAG, "OTA already running"); return false; }
    if (!url || !url[0]) { ESP_LOGE(TAG, "Invalid OTA URL"); return false; }
    strlcpy(s_url, url, sizeof s_url);
    if (xTaskCreate(ota_task, "ota_task", 6 * 1024, NULL, tskIDLE_PRIORITY + 2, &s_ota_task) != pdPASS) {
        ESP_LOGE(TAG, "Create ota_task failed");
        s_ota_task = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Starting OTA: %s", s_url);
    return true;
}

bool ota_in_progress(void) { return s_ota_task != NULL; }

void ota_mark_app_valid(void) {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) ESP_LOGI(TAG, "Marked app valid");
#endif
}
