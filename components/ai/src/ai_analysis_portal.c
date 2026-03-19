#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "cJSON.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h" // IPSTR/IP2STR

#include "ai_utils.h"
#include "ai_analysis_portal.h"
#include "ai_analysis_portal_html.h"

#define TAG "AI_ANALYSIS_PORTAL"

// NOTE: This portal is intentionally an OPEN SoftAP (no password)
// This keeps UX simple for quickly viewing the AI packet analysis

// HTTP server instance
static httpd_handle_t http_handle = NULL;

// Default Wi-Fi AP netif created by esp_netif_create_default_wifi_ap()
static esp_netif_t *ap_netif = NULL;

// AP identity (SSID + cached IP)
static char s_ssid[32] = "PolyCast5-AI-Analysis";
static char s_ip[16] = "192.168.4.1";

// Latest analysis text (copied from the AI task output buffer).
// This avoids lifetime issues with the producer buffer being reused.
POLYCAST5_USE_PSRAM_BSS static char s_result[AI_RESPONSE_MAX_LEN] = {0};

/* =============== HTTP handlers =============== */

static esp_err_t root_get(httpd_req_t *req)
{
    // Serve the single-page UI
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, AI_ANALYSIS_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t result_get(httpd_req_t *req)
{
    // Return JSON:
    //  - has_result: bool
    //  - md: raw markdown text (client renders safely)
    bool has_result = (s_result[0] != '\0');

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddBoolToObject(root, "has_result", has_result);

    if (has_result) {
        // Cap to keep response size reasonable (avoid huge JSON allocations)
        #define AI_ANALYSIS_MD_CAP (16 * 1024)

        // Determine copy length
        size_t src_len = strlen(s_result);
        size_t copy_len = (src_len < (AI_ANALYSIS_MD_CAP - 1)) ? src_len : (AI_ANALYSIS_MD_CAP - 1);

        if (src_len > copy_len) {
            ESP_LOGW(TAG, "Analysis portal result truncated for HTTP (src_len=%u cap=%u)",
                    (unsigned)src_len, (unsigned)AI_ANALYSIS_MD_CAP);
        }

        // Allocate buffer
        char *md = (char *)malloc(copy_len + 1);
        if (!md) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }

        // Copy into buffer and null-terminate
        memcpy(md, s_result, copy_len);
        md[copy_len] = '\0';

        cJSON_AddStringToObject(root, "md", md);
        free(md);
    }

    // Serialize JSON response
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, txt);
    free(txt);
    return ret;
}

/* =============== HTTPD bring-up =============== */

static httpd_handle_t httpd_start_local(void)
{
    // Configure server (match ai_web_portal.c pattern)
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192; // Increase if needed

    // Start server
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        return NULL;
    }

    // Register endpoints
    httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL
    };
    httpd_uri_t u_res = {
        .uri = "/api/result", .method = HTTP_GET, .handler = result_get, .user_ctx = NULL
    };

    // Register URI handlers
    httpd_register_uri_handler(h, &u_root);
    httpd_register_uri_handler(h, &u_res);

    return h;
}

/* =============== Public API =============== */

esp_err_t ai_analysis_portal_set_result(const char *result)
{
    // Copy the result into our internal buffer (safe lifetime for HTTP handlers)
    if (!result) {
        s_result[0] = '\0';
        return ESP_OK;
    }

    strncpy(s_result, result, sizeof(s_result) - 1);
    s_result[sizeof(s_result) - 1] = '\0';

    return ESP_OK;
}

/* =============== Wi-Fi AP bring-up =============== */

esp_err_t ai_analysis_portal_start(void)
{
    // If server already running, nothing to do
    if (http_handle) {
        return ESP_OK;
    }

    // Init netif (ignore already initialized)
    esp_err_t err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Create default event loop (ignore already created)
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Create SoftAP netif if needed
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
        if (!ap_netif) {
            return ESP_FAIL;
        }
    }

    // Init Wi-Fi driver (tolerate already init)
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE)) {
        return err;
    }

    // Keep config in RAM only (no persistence)
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Configure SoftAP as OPEN (no password)
    wifi_config_t ap = {0};
    strlcpy((char*)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid));

    // For WIFI_AUTH_OPEN, password must be empty
    ap.ap.password[0] = '\0';
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    // Set AP mode (idempotent)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Apply config
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    // Start driver (tolerate already started)
    err = esp_wifi_start();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Cache AP IP (usually 192.168.4.1)
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    }

    // Start HTTP server
    http_handle = httpd_start_local();
    if (!http_handle) {
        return ESP_FAIL;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "AI analysis portal up: SSID='%s' (open) IP=%s", s_ssid, s_ip);
#endif

    return ESP_OK;
}

esp_err_t ai_analysis_portal_stop(void)
{
    // Stop httpd if running
    if (http_handle) {
        httpd_stop(http_handle);
        http_handle = NULL;
    }

    // Stop Wi-Fi if running (ignore errors)
    esp_err_t err = esp_wifi_stop();

    // Fully detach Wi-Fi from any interface
    err = esp_wifi_set_mode(WIFI_MODE_NULL);

    // Destroy netif to free handlers/resources
    if (ap_netif) {
        esp_netif_destroy_default_wifi(ap_netif);
        ap_netif = NULL;
    }

    // Most of the rest of the system expects STA mode available
    err = esp_wifi_set_mode(WIFI_MODE_STA);

    return err;
}

/* =============== Public getters =============== */

const char *ai_analysis_portal_get_ssid(void)
{
    return s_ssid;
}

const char *ai_analysis_portal_get_pass(void)
{
    // Open network => no password
    return "";
}

const char *ai_analysis_portal_get_ip(void)
{
    return s_ip;
}