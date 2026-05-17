#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "nvs.h"
#include "cJSON.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h" // For IPSTR/IP2STR

#include "wifi_btc_portal.h"
#include "wifi_btc_portal_html.h"

#define TAG "WIFI_BTC_PORTAL"

#define BTC_PASS_NS "btc_wifi"
#define BTC_PASS_KEY "pass"

#define BTC_ADDR_NS "btc_recv"
#define BTC_ADDR_KEY "addr"

#define MAX_BODY 1024

extern char btc_wifi_portal_pass[];

// Default AP info (replace with your existing portal creds when desired)
static httpd_handle_t btc_server = NULL;
static esp_netif_t *btc_ap_netif = NULL;

static char btc_portal_ssid[32] = "PolyCast5-BTC-Portal"; // AP SSID
static char btc_portal_ip[16] = "192.168.4.1"; // AP IP cached

/* =============== HTTP handlers =============== */

static esp_err_t root_get(httpd_req_t *req)
{
    // Serve HTML
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WIFI_BTC_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t addr_get(httpd_req_t *req)
{
    // Read address from NVS
    char addr[128] = "";
    (void)wifi_btc_addr_get_nvs(addr, sizeof(addr));

    // Build JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); // OOM
    }
    cJSON_AddStringToObject(root, "address", addr);

    // Send JSON
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); // OOM
    }
    
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, txt);
    
    free(txt); // Free JSON string
    return e;
}

static void trim_ascii(char *s)
{
    // Trim in-place
    if (!s) {
        return;
    }

    // Skip leading
    char *p = s;
    while (*p && (unsigned char)*p <= ' ') {
        p++;
    }

    // Find trailing
    size_t n = strlen(p);
    while (n && (unsigned char)p[n - 1] <= ' ') {
        p[--n] = '\0'; // Chop tail
    }

    // Move to front
    if (p != s) {
        memmove(s, p, n + 1); // Include NUL
    }
}

static esp_err_t addr_post(httpd_req_t *req)
{
    // Bound content length
    if (req->content_len > MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big"); // Reject large
    }

    // Read body
    char *buf = (char*)calloc(1, (size_t)req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); // OOM
    }
    size_t r = 0;
    while (r < (size_t)req->content_len) {
        int g = httpd_req_recv(req, buf + r, req->content_len - (int)r);
        if (g <= 0) {
            free(buf); // Cleanup on recv error
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        r += (size_t)g;
    }

    // Parse JSON
    cJSON *j = cJSON_Parse(buf);
    free(buf); // Free raw buffer
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); // Parse error
    }

    // Get address field
    const cJSON *ja = cJSON_GetObjectItemCaseSensitive(j, "address");
    if (!cJSON_IsString(ja)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad address"); // Missing/invalid
    }

    // Copy and trim
    char addr[128] = {0};
    strncpy(addr, ja->valuestring, sizeof(addr) - 1);
    trim_ascii(addr); // Remove leading/trailing spaces

    // Simple bech32 mainnet sanity (bc1q... or bc1p..., 14..90 chars)
    size_t L = strlen(addr);
    if (!((strncmp(addr, "bc1q", 4) == 0 || strncmp(addr, "bc1p", 4) == 0) && L >= 14 && L <= 90)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid address"); // Basic check
    }

    // Save to NVS
    esp_err_t e = wifi_btc_addr_set_nvs(addr);

    // Cleanup
    cJSON_Delete(j);

    // Return result
    if (e != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs"); // NVS error
    }
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* =============== HTTPD bring-up =============== */

static httpd_handle_t btc_httpd_start(void)
{
    // Configure server
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192; // Increase if needed

    // Start server
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        return NULL; // Failed to start
    }

    // Register endpoints
    httpd_uri_t u_root = {
        .uri="/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL
    };
    httpd_uri_t u_get  = {
        .uri="/api/address", .method = HTTP_GET, .handler = addr_get, .user_ctx = NULL
    };
    httpd_uri_t u_post = {
        .uri="/api/address", .method = HTTP_POST, .handler = addr_post, .user_ctx = NULL
    };

    httpd_register_uri_handler(h, &u_root);
    httpd_register_uri_handler(h, &u_get);
    httpd_register_uri_handler(h, &u_post);

    return h;
}

/* =============== Wi-Fi AP bring-up =============== */

esp_err_t wifi_btc_portal_start(void)
{
    // If server already running, nothing to do
    if (btc_server) {
        return ESP_OK;
    }

    // Init netif (ignore already initialized)
    esp_err_t err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Create default loop (ignore already created)
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Create SoftAP netif if needed
    if (!btc_ap_netif) {
        btc_ap_netif = esp_netif_create_default_wifi_ap();
        if (!btc_ap_netif) {
            return ESP_FAIL;
        }
    }

    // Init Wi-Fi (tolerate already init)
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE) && (err != ESP_ERR_INVALID_STATE)) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Keep config in RAM only
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Configure SoftAP
    wifi_config_t ap = {0};
    strlcpy((char*)ap.ap.ssid, btc_portal_ssid, sizeof(ap.ap.ssid));
    strlcpy((char*)ap.ap.password, btc_wifi_portal_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
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
    if (esp_netif_get_ip_info(btc_ap_netif, &ip) == ESP_OK) {
        // Write "a.b.c.d"
        snprintf(btc_portal_ip, sizeof(btc_portal_ip), IPSTR, IP2STR(&ip.ip));
    }

    // Start HTTP server
    btc_server = btc_httpd_start(); // start_httpd helper
    if (!btc_server) {
        (void)wifi_btc_portal_stop();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t wifi_btc_portal_stop(void)
{
    // Stop httpd if running
    if (btc_server) {
        httpd_stop(btc_server);
        btc_server = NULL; // Mark stopped
    }
    
    esp_err_t err = esp_wifi_stop();

    // Fully detach Wi-Fi from any interface
    err = esp_wifi_set_mode(WIFI_MODE_NULL);
    
    if (btc_ap_netif) {
        esp_netif_destroy_default_wifi(btc_ap_netif); // Destroys handlers and netif
        btc_ap_netif = NULL;
    }

    // Everything else needs station mode
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    
    return err;
}

/* =============== Public variables =============== */

const char *wifi_btc_portal_get_ssid(void)
{
    // Return SSID
    return btc_portal_ssid;
}

const char *wifi_btc_portal_get_pass(void)
{
    // Return password
    return btc_wifi_portal_pass;
}

const char *wifi_btc_portal_get_ip(void)
{
    // Return IP
    return btc_portal_ip;
}

/* =============== NVS =============== */

esp_err_t wifi_btc_addr_set_nvs(const char *addr)
{
    // Fallback to empty string
    if (!addr) {
        addr = "";
    }

    // Open NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(BTC_ADDR_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err; // NVS open failed
    }

    // Save address
    err = nvs_set_str(h, BTC_ADDR_KEY, addr);

    // Commit if OK
    if (err == ESP_OK) {
        err = nvs_commit(h); // Persist changes
    }

    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t wifi_btc_addr_get_nvs(char *addr_out, size_t sz)
{
    // Validate args
    if (!addr_out || sz == 0) {
        return ESP_ERR_INVALID_ARG; // Bad buffer
    }

    // Open NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(BTC_ADDR_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        addr_out[0] = '\0'; // Default empty on first boot
        return err;
    }

    // Get address blob
    size_t need = sz;
    err = nvs_get_str(h, BTC_ADDR_KEY, addr_out, &need);

    // If not found, default empty
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        addr_out[0] = '\0'; // Zero for first-boot default
    }

    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t wifi_btc_pass_save_nvs(const char *val)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(BTC_PASS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Set the version string
    err = nvs_set_str(h, BTC_PASS_KEY, val);
    
    // Persist changes if success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    
    // Close and return
    nvs_close(h);
    return err;
}

esp_err_t wifi_btc_pass_load_nvs(char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(BTC_PASS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t len = out_sz; // Must include room for '\0'
    
    // Get the saved version string
    err = nvs_get_str(h, BTC_PASS_KEY, out, &len);
    
    // Close and return
    nvs_close(h);
    return err;
}
