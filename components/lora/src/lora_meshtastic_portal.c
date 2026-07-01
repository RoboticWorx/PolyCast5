#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_log.h"

#include "lora_meshtastic_portal.h"
#include "lora_meshtastic_portal_html.h"

#define TAG "MESHTASTIC_WEB_PORTAL"

// NVS storage for the randomly generated portal password
#define MESHTASTIC_PORTAL_NS  "mesh_portal"
#define MESHTASTIC_PORTAL_KEY "pass"

// NVS key (same namespace) for the Meshtastic enabled flag (u8: 0/1)
#define MESHTASTIC_PORTAL_ENABLED_KEY "enabled"

#define MESHTASTIC_PORTAL_PASS_LEN 12

static char s_pass[64] = {0};
static bool s_pass_ready = false;

static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ip[16] = "192.168.4.1";

/* =============== NVS =============== */

// Persist the randomly generated portal password
static esp_err_t meshtastic_portal_pass_save_nvs(const char *val)
{
    nvs_handle_t h;

    esp_err_t err = nvs_open(MESHTASTIC_PORTAL_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, MESHTASTIC_PORTAL_KEY, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

// Load the previously generated portal password
static esp_err_t meshtastic_portal_pass_load_nvs(char *out, size_t out_sz)
{
    nvs_handle_t h;

    esp_err_t err = nvs_open(MESHTASTIC_PORTAL_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = out_sz; // Must include room for '\0'
    err = nvs_get_str(h, MESHTASTIC_PORTAL_KEY, out, &len);

    nvs_close(h);
    return err;
}

esp_err_t lora_meshtastic_portal_enabled_save_nvs(bool enabled)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(MESHTASTIC_PORTAL_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "meshtastic enabled save nvs_open failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Store the flag as a u8 (0/1)
    err = nvs_set_u8(h, MESHTASTIC_PORTAL_ENABLED_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

bool lora_meshtastic_portal_enabled_load_nvs(void)
{
    nvs_handle_t h;
    uint8_t val = 0; // Default: disabled if not yet stored

    // Open NVS read-only; if the namespace/key doesn't exist yet, fall back to disabled
    if (nvs_open(MESHTASTIC_PORTAL_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, MESHTASTIC_PORTAL_ENABLED_KEY, &val) != ESP_OK) {
            val = 0; // Key not present; default disabled
        }
        nvs_close(h);
    }

    return val != 0;
}

void lora_meshtastic_portal_pass_init(void)
{
    // Already loaded/generated
    if (s_pass_ready) {
        return;
    }

    // Reuse the saved password if one exists, otherwise generate and persist one
    if (meshtastic_portal_pass_load_nvs(s_pass, sizeof(s_pass)) == ESP_OK) {
        s_pass_ready = true;

#ifdef POLYCAST5_PASS_DEBUG
        ESP_LOGI(TAG, "Using pre-set Meshtastic Wi-Fi portal password: '%s'", s_pass);
#endif
        return;
    }

    // Random chars to pick from (omits look-alike characters)
    static const char alphabet[] =
            "ABCDEFGHJKLMNPQRSTUVWXYZ"
            "abcdefghijkmnopqrstuvwxyz"
            "0123456789";

    const size_t N = sizeof(alphabet) - 1;

    // Create random password
    for (size_t i = 0; i < MESHTASTIC_PORTAL_PASS_LEN; ++i) {
        uint32_t r = esp_random();
        s_pass[i] = alphabet[r % N];
    }
    s_pass[MESHTASTIC_PORTAL_PASS_LEN] = '\0';
    s_pass_ready = true;

    // Save that version to NVS
    meshtastic_portal_pass_save_nvs(s_pass);

#ifdef POLYCAST5_PASS_DEBUG
    ESP_LOGW(TAG, "Setting first time Meshtastic Wi-Fi portal password: %s", s_pass);
#endif
}

/* =============== HTTP handlers =============== */

// Serve the single-page HTML UI
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    return httpd_resp_send(req, LORA_MESHTASTIC_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ========== HTTP server bootstrap ========== */

// Start the embedded HTTP server and register endpoints
static httpd_handle_t start_http(void)
{
    // Configure default
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;

    // Start HTTP
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        return NULL;
    }

    // UI
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_register_uri_handler(srv, &root);

    return srv;
}

/* ========== Portal management ========== */

esp_err_t lora_meshtastic_portal_start(void)
{
    // If already running, do nothing
    if (s_server != NULL) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Portal already running at http://%s", s_ip);
#endif
        return ESP_OK;
    }

    // Make sure the password is available (no-op if already loaded)
    lora_meshtastic_portal_pass_init();

    // Create default AP netif if needed
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_FAIL;
        }
    }

    // Init Wi-Fi (tolerate "already init" state)
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE) && (err != ESP_ERR_INVALID_STATE)) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Keep config in RAM so nothing persists accidentally
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Configure SoftAP
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, MESHTASTIC_PORTAL_SSID);
    ap.ap.ssid_len = strlen(MESHTASTIC_PORTAL_SSID);
    strcpy((char *)ap.ap.password, s_pass);
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Start Wi-Fi
    err = esp_wifi_start();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Cache AP IP (usually 192.168.4.1)
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    }

    // Bring up HTTP server and register endpoints
    s_server = start_http();
    if (s_server == NULL) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "start_http failed");
#endif
        (void)lora_meshtastic_portal_stop(); // Tear down the SoftAP we just brought up
        return ESP_FAIL;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Portal running at http://%s (SSID: " MESHTASTIC_PORTAL_SSID ")", s_ip);
#endif

    return ESP_OK;
}

esp_err_t lora_meshtastic_portal_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    esp_err_t err = esp_wifi_stop();

    // Fully detach Wi-Fi from any interface
    err = esp_wifi_set_mode(WIFI_MODE_NULL);

    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif); // Destroys handlers and netif
        s_ap_netif = NULL;
    }

    // Everything else needs station mode
    err = esp_wifi_set_mode(WIFI_MODE_STA);

    return err;
}

const char *lora_meshtastic_portal_get_ip(void)
{
    return s_ip;
}

const char *lora_meshtastic_portal_get_ssid(void)
{
    return MESHTASTIC_PORTAL_SSID;
}

const char *lora_meshtastic_portal_get_pass(void)
{
    // Ensure the password is populated even if the getter is called before init
    if (!s_pass_ready) {
        lora_meshtastic_portal_pass_init();
    }

    return s_pass;
}
