#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "nvs.h"
#include "cJSON.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h"

#include "wifi_claude.h"
#include "wifi_claude_portal.h"
#include "wifi_claude_portal_html.h"

#define TAG "WIFI_CLAUDE_PORTAL"

// Max bytes on the /api/host POST body
#define MAX_BODY 512

static httpd_handle_t claude_server  = NULL; // Non-NULL while the HTTP server is running
static esp_netif_t  *claude_ap_netif = NULL; // SoftAP netif handle (created lazily)

// SSID is fixed; IP is filled in once the AP comes up
static char claude_portal_ssid[32] = "PolyCast5-Claude-Portal";
static char claude_portal_ip[16]   = "192.168.4.1";

/* =============== Helpers =============== */

// Parse "http://<ip>:<port>/usage" back into ip + port for the GET handler so we can pre-fill the form
static void split_saved_url(const char *url, char *ip_out, size_t ip_sz, char *port_out, size_t port_sz)
{
    ip_out[0] = '\0';
    port_out[0] = '\0';

    if (!url || !url[0]) {
        return;
    }

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    // Read IP up to ':' or '/'
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && i + 1 < ip_sz) {
        ip_out[i++] = *p++;
    }
    ip_out[i] = '\0';

    if (*p == ':') {
        p++;
        size_t j = 0;
        while (*p && isdigit((unsigned char)*p) && j + 1 < port_sz) {
            port_out[j++] = *p++;
        }
        port_out[j] = '\0';
    }
}

// Minimal IPv4 dotted-quad validator - accepts "a.b.c.d" where each octet is 1-3 decimal digits in [0,255]
// Rejects empty octets, leading non-digits, and DNS names
static bool valid_ipv4(const char *s)
{
    if (!s || !*s) {
        return false;
    }

    int octets = 0; // How many dot-separated octets we've completed
    int digits = 0; // Digit count of the current octet
    int val = 0;    // Numeric value of the current octet

    // Walk one char past the end so the trailing-octet path runs on '\0'
    for (const char *p = s; ; p++) {
        if (*p == '.' || *p == '\0') {
            // Finalize the current octet; reject empties, overflows, or too-long octets
            if (digits == 0 || digits > 3 || val > 255) {
                return false;
            }
            octets++;

            // Last char (NUL) ends the loop
            if (*p == '\0') {
                break;
            }

            // Reset for the next octet
            digits = 0;
            val = 0;
        } else if (*p >= '0' && *p <= '9') {
            // Accumulate decimal digits
            val = val * 10 + (*p - '0');
            digits++;
        } else {
            // Anything else (letters, dashes, etc.) is a hard reject
            return false;
        }
    }

    // Exactly four octets is the only valid count
    return octets == 4;
}

// Validate a numeric port string and write the parsed value to *port_out
// Accepts 1..65535; rejects empty, non-digit, and out-of-range inputs
static bool valid_port(const char *s, int *port_out)
{
    if (!s || !*s) {
        return false;
    }

    // Parse digits into v, bailing if we ever overshoot the 16-bit range
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        v = v * 10 + (*p - '0');
        if (v > 65535) {
            return false;
        }
    }

    // Port 0 is invalid for our use
    if (v < 1) {
        return false;
    }

    *port_out = v;
    return true;
}

// Strip leading/trailing whitespace in-place. Used to clean up user input
// from the form before validating.
static void trim_ascii(char *s)
{
    if (!s) return;

    // Skip leading whitespace
    char *p = s;
    while (*p && (unsigned char)*p <= ' ') p++;

    // Trim trailing whitespace
    size_t n = strlen(p);
    while (n && (unsigned char)p[n - 1] <= ' ') p[--n] = '\0';

    // Shift the trimmed string back to the start if needed
    if (p != s) memmove(s, p, n + 1);
}

/* =============== HTTP handlers =============== */

// GET / -> serve the form HTML (the page itself loads /api/host and posts to it)
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WIFI_CLAUDE_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /api/host -> return the currently saved IP+port so the form can pre-fill.
// Returns empty strings rather than 404 when no URL has been saved yet, which
// keeps the JS client trivial.
static esp_err_t host_get(httpd_req_t *req)
{
    // Load current URL from NVS (ignore the read-error case — we'll just send blanks)
    char url[WIFI_CLAUDE_URL_MAX_LEN] = "";
    (void)wifi_claude_load_config_nvs(url, sizeof(url));

    // Split it back into the form fields
    char ip[32]   = "";
    char port[8]  = "";
    split_saved_url(url, ip, sizeof(ip), port, sizeof(port));

    // Build the response JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "port", port);

    // Serialize and free the tree before sending so we don't leak on failure paths
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Send the serialized JSON and free the buffer
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, txt);
    free(txt);
    return e;
}

// POST /api/host -> accept {"ip":"...","port":"..."}, validate, assemble the full "http://<ip>:<port>/usage" URL, and persist via NVS
static esp_err_t host_post(httpd_req_t *req)
{
    // Reject absurd bodies before allocating
    if (req->content_len > MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
    }

    // Allocate body buffer (+1 for NUL so cJSON_Parse has a terminator)
    char *buf = (char *)calloc(1, (size_t)req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Read the body in a loop because httpd_req_recv may return partial chunks
    size_t r = 0;
    while (r < (size_t)req->content_len) {
        int g = httpd_req_recv(req, buf + r, req->content_len - (int)r);
        if (g <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        r += (size_t)g;
    }

    // Parse the JSON body and free the raw buffer immediately
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    // Extract the two expected fields
    const cJSON *jip = cJSON_GetObjectItemCaseSensitive(j, "ip");
    const cJSON *jport = cJSON_GetObjectItemCaseSensitive(j, "port");
    if (!cJSON_IsString(jip)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ip");
    }

    // Copy IP into a sized local buffer and strip whitespace
    char ip[32] = {0};
    char port[8] = "8765"; // Default if the user left the port field blank
    strncpy(ip, jip->valuestring, sizeof(ip) - 1);
    trim_ascii(ip);

    // Port is optional in the request; only override default if non-empty
    if (cJSON_IsString(jport) && jport->valuestring && jport->valuestring[0]) {
        strncpy(port, jport->valuestring, sizeof(port) - 1);
        port[sizeof(port) - 1] = '\0';
        trim_ascii(port);
    }

    // Validate IP shape (dotted-quad)
    if (!valid_ipv4(ip)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ip");
    }

    // Validate port (1..65535)
    int port_num = 0;
    if (!valid_port(port, &port_num)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid port");
    }

    // Assemble "http://<ip>:<port>/usage"
    char url[WIFI_CLAUDE_URL_MAX_LEN];
    int n = snprintf(url, sizeof(url), "http://%s:%d/usage", ip, port_num);
    cJSON_Delete(j);
    if (n <= 0 || n >= (int)sizeof(url)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "url too long");
    }

    // Persist to NVS
    esp_err_t e = wifi_claude_save_config_nvs(url);
    if (e != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    // Acknowledge so the JS client can flip its "Saved!" indicator
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* =============== HTTPD bring-up =============== */

// Boot the HTTP server and register the three endpoints
static httpd_handle_t claude_httpd_start(void)
{
    // Use defaults; bump the stack so cJSON parsing inside handlers has room
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;

    // Start the server
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        return NULL;
    }

    // Register URI handlers: form HTML and the two JSON endpoints
    httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL
    };
    httpd_uri_t u_get = {
        .uri = "/api/host", .method = HTTP_GET, .handler = host_get, .user_ctx = NULL
    };
    httpd_uri_t u_post = {
        .uri = "/api/host", .method = HTTP_POST, .handler = host_post, .user_ctx = NULL
    };

    httpd_register_uri_handler(h, &u_root);
    httpd_register_uri_handler(h, &u_get);
    httpd_register_uri_handler(h, &u_post);
    return h;
}

/* =============== Wi-Fi AP bring-up =============== */

esp_err_t wifi_claude_portal_start(void)
{
    // Already running?
    if (claude_server) {
        return ESP_OK;
    }

    // netif may have been initialized earlier by station-mode bring-up
    esp_err_t err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Default event loop may also already exist
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Create the SoftAP netif on first use
    if (!claude_ap_netif) {
        claude_ap_netif = esp_netif_create_default_wifi_ap();
        if (!claude_ap_netif) {
            return ESP_FAIL;
        }
    }

    // Initialize Wi-Fi; tolerate either "already init" status code
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE) && (err != ESP_ERR_INVALID_STATE)) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Keep config in RAM; we don't want this AP persisted across reboots
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // SoftAP config: open auth (LAN-only, low-stakes), single channel, four max clients
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, claude_portal_ssid, sizeof(ap.ap.ssid));
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    // Switch into AP-only mode and apply config
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    // Start the radio
    err = esp_wifi_start();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Cache the AP-side IP (usually 192.168.4.1) for the LCD setup page to display
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(claude_ap_netif, &ip) == ESP_OK) {
        snprintf(claude_portal_ip, sizeof(claude_portal_ip), IPSTR, IP2STR(&ip.ip));
    }

    // Start the HTTP server
    claude_server = claude_httpd_start();
    if (!claude_server) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t wifi_claude_portal_stop(void)
{
    // Stop the HTTP server if it was running
    if (claude_server) {
        httpd_stop(claude_server);
        claude_server = NULL;
    }

    // Stop the radio, then fully detach from any Wi-Fi interface
    esp_err_t err = esp_wifi_stop();
    err = esp_wifi_set_mode(WIFI_MODE_NULL);

    // Free the AP netif so a future portal_start() builds a fresh one
    if (claude_ap_netif) {
        esp_netif_destroy_default_wifi(claude_ap_netif);
        claude_ap_netif = NULL;
    }

    // Everything else needs STA mode
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    return err;
}

/* =============== Public getters =============== */

// SSID shown on the LCD setup page so the user knows what to connect to
const char *wifi_claude_portal_get_ssid(void)
{
    return claude_portal_ssid;
}

// IP shown on the LCD setup page so the user knows where to point their browser
const char *wifi_claude_portal_get_ip(void)
{
    return claude_portal_ip;
}
