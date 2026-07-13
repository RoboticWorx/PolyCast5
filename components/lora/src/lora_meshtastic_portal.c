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
#include "lora_meshtastic.h" // node id, message log, TX enqueue
#include "lora_pcp.h" // LoRa region NVS load (US/EU banner frequency)

#define TAG "MESHTASTIC_WEB_PORTAL"

// Largest POST body accepted by /api/send
// (a single text message, plus slack for UTF-8 / trailing whitespace; lora_meshtastic_enqueue_text truncates to spec)
#define MESHTASTIC_SEND_MAX_BODY 512

// Scratch sizes for the /api/messages JSON builder
// A JSON-escaped char can grow to 6 bytes (\u00XX), so bound the worst case on the stored text length
#define MESHTASTIC_MSG_ESC_BUF (MESHTASTIC_RX_TEXT_MAX * 6 + 8)
#define MESHTASTIC_MSG_OBJ_BUF (MESHTASTIC_RX_TEXT_MAX * 6 + 256)

// Scratch sizes for the /api/nodes JSON builder (JSON-escaped names + fixed fields)
#define MESHTASTIC_NODE_LONG_ESC  (MESHTASTIC_NODE_LONG_MAX * 6 + 8)
#define MESHTASTIC_NODE_SHORT_ESC (MESHTASTIC_NODE_SHORT_MAX * 6 + 8)
#define MESHTASTIC_NODE_OBJ_BUF   (MESHTASTIC_NODE_LONG_ESC + MESHTASTIC_NODE_SHORT_ESC + 128)

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

    // Head blob (everything up to the RF-info banner)
    esp_err_t err = httpd_resp_send_chunk(req, LORA_MESHTASTIC_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }

    // RF-info banner reflects the active LoRa region so an EU user sees the real frequency
    const char *banner = (lora_pcp_load_region_nvs() == LORA_REGION_EU)
        ? "        <div class=\"netinfo\">LongFast &middot; EU 868 &middot; 869.525 MHz &middot; SF11/BW250</div>\n"
        : "        <div class=\"netinfo\">LongFast &middot; US 915 &middot; 906.875 MHz &middot; SF11/BW250</div>\n";
    err = httpd_resp_send_chunk(req, banner, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }

    // Tail blob (the PSK banner onward)
    err = httpd_resp_send_chunk(req, LORA_MESHTASTIC_PORTAL_HTML_TAIL, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }

    // Terminate the chunked response
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Escape a UTF-8 string into a JSON string body (without the surrounding quotes),
// writing at most out_sz-1 bytes plus a NUL. Returns bytes written (excl. NUL).
static size_t json_escape(const char *in, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        unsigned char c = *p;

        // Worst case is a 6-byte \u00XX escape; keep room for that plus the NUL
        if (o + 6 >= out_sz) {
            break;
        }

        switch (c) {
        case '\"': out[o++] = '\\'; out[o++] = '\"'; break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                out[o++] = hex[(c >> 4) & 0x0F];
                out[o++] = hex[c & 0x0F];
            } else {
                out[o++] = (char)c; // Printable ASCII and UTF-8 bytes pass through
            }
            break;
        }
    }

    out[o] = '\0';
    return o;
}

// POST /api/send - raw request body is the message text to broadcast.
// The portal is purely an input surface: it hands the text to the LoRa radio,
// which frames and transmits it per Meshtastic spec.
static esp_err_t send_post(httpd_req_t *req)
{
    // Reject empty or oversized bodies (copy to int first, as the other portals
    // do, to keep the length comparisons signed)
    int total = (int)req->content_len;
    if (total <= 0 || total > MESHTASTIC_SEND_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    // Allocate a buffer for the body (+1 for NUL)
    char *buf = (char *)malloc((size_t)total + 1);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Read the full body
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        off += r;
    }
    buf[off] = '\0';

    // Trim leading/trailing ASCII blanks/controls (UTF-8 bytes are >= 0x80, kept)
    char *start = buf;
    while (*start != '\0' && (unsigned char)*start <= ' ') {
        start++;
    }
    size_t len = strlen(start);
    while (len > 0 && (unsigned char)start[len - 1] <= ' ') {
        start[--len] = '\0';
    }

    // Queue for the mesh TX task (single-producer TX). Empty after trim -> reject
    bool ok = (len > 0) && lora_meshtastic_enqueue_text(start);
    free(buf);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// GET /api/messages?since=N  — text messages with seq > N, as JSON.
// Response: {"node":"!id","newest":N,"msgs":[{"seq","from","out","rssi","snr","text"},...]}
static esp_err_t messages_get(httpd_req_t *req)
{
    // Parse optional ?since=N (default 0 = all currently stored)
    uint32_t since = 0;
    char qstr[48];
    if (httpd_req_get_url_query_str(req, qstr, sizeof(qstr)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(qstr, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    // Scratch: message snapshot + per-message escape/object buffers
    lora_meshtastic_msg_t *msgs = (lora_meshtastic_msg_t *)malloc(sizeof(*msgs) * MESHTASTIC_MSG_LOG_CAP);
    char *esc = (char *)malloc(MESHTASTIC_MSG_ESC_BUF);
    char *obj = (char *)malloc(MESHTASTIC_MSG_OBJ_BUF);
    if (msgs == NULL || esc == NULL || obj == NULL) {
        free(msgs);
        free(esc);
        free(obj);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    uint32_t newest = 0;
    size_t n = lora_meshtastic_get_msgs_since(since, msgs, MESHTASTIC_MSG_LOG_CAP, &newest);

    httpd_resp_set_type(req, "application/json");

    // Header chunk (also carries the live node count for the roster badge)
    int hlen = snprintf(obj, MESHTASTIC_MSG_OBJ_BUF,
                        "{\"node\":\"%s\",\"newest\":%u,\"nodes\":%u,\"msgs\":[",
                        lora_meshtastic_node_id(), (unsigned)newest,
                        (unsigned)lora_meshtastic_node_count());
    if (hlen < 0) {
        hlen = 0;
    } else if (hlen >= MESHTASTIC_MSG_OBJ_BUF) {
        hlen = MESHTASTIC_MSG_OBJ_BUF - 1;
    }
    esp_err_t err = httpd_resp_send_chunk(req, obj, hlen);

    // One chunk per message object
    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        json_escape(msgs[i].text, esc, MESHTASTIC_MSG_ESC_BUF);

        int olen = snprintf(obj, MESHTASTIC_MSG_OBJ_BUF,
                            "%s{\"seq\":%u,\"id\":%u,\"from\":\"!%08x\",\"out\":%s,\"acked\":%s,\"failed\":%s,\"rssi\":%d,\"snr\":%d,\"hops\":%u,\"text\":\"%s\"}",
                            (i == 0) ? "" : ",",
                            (unsigned)msgs[i].seq, (unsigned)msgs[i].id, (unsigned)msgs[i].from_node,
                            msgs[i].outbound ? "true" : "false",
                            msgs[i].acked ? "true" : "false",
                            msgs[i].failed ? "true" : "false",
                            (int)msgs[i].rssi, (int)msgs[i].snr, (unsigned)msgs[i].hops, esc);
        if (olen < 0) {
            olen = 0;
        } else if (olen >= MESHTASTIC_MSG_OBJ_BUF) {
            olen = MESHTASTIC_MSG_OBJ_BUF - 1;
        }
        err = httpd_resp_send_chunk(req, obj, olen);
    }

    // Footer + terminate the chunked response (best-effort on prior error)
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    httpd_resp_send_chunk(req, NULL, 0);

    free(msgs);
    free(esc);
    free(obj);
    return err;
}

// GET /api/nodes  — roster of heard nodes (client sorts by most-recently-heard).
// Response: {"nodes":[{"id":"!id","long":"..","short":"..","rssi":r,"snr":s,"hops":h,"age":sec},...]}
static esp_err_t nodes_get(httpd_req_t *req)
{
    lora_meshtastic_node_t *nodes = (lora_meshtastic_node_t *)malloc(sizeof(*nodes) * MESHTASTIC_NODE_MAX);
    char *el  = (char *)malloc(MESHTASTIC_NODE_LONG_ESC);
    char *es  = (char *)malloc(MESHTASTIC_NODE_SHORT_ESC);
    char *obj = (char *)malloc(MESHTASTIC_NODE_OBJ_BUF);
    if (nodes == NULL || el == NULL || es == NULL || obj == NULL) {
        free(nodes);
        free(el);
        free(es);
        free(obj);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    size_t n = lora_meshtastic_get_nodes(nodes, MESHTASTIC_NODE_MAX);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, "{\"nodes\":[", 10);

    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        json_escape(nodes[i].long_name, el, MESHTASTIC_NODE_LONG_ESC);
        json_escape(nodes[i].short_name, es, MESHTASTIC_NODE_SHORT_ESC);

        int olen = snprintf(obj, MESHTASTIC_NODE_OBJ_BUF,
                            "%s{\"id\":\"!%08x\",\"long\":\"%s\",\"short\":\"%s\",\"rssi\":%d,\"snr\":%d,\"hops\":%u,\"age\":%u}",
                            (i == 0) ? "" : ",",
                            (unsigned)nodes[i].node_num, el, es,
                            (int)nodes[i].rssi, (int)nodes[i].snr,
                            (unsigned)nodes[i].hops, (unsigned)nodes[i].age_s);
        if (olen < 0) {
            olen = 0;
        } else if (olen >= MESHTASTIC_NODE_OBJ_BUF) {
            olen = MESHTASTIC_NODE_OBJ_BUF - 1;
        }
        err = httpd_resp_send_chunk(req, obj, olen);
    }

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    httpd_resp_send_chunk(req, NULL, 0);

    free(nodes);
    free(el);
    free(es);
    free(obj);
    return err;
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

    // Messaging API
    httpd_uri_t send = {.uri = "/api/send", .method = HTTP_POST, .handler = send_post};
    httpd_register_uri_handler(srv, &send);

    httpd_uri_t msgs = {.uri = "/api/messages", .method = HTTP_GET, .handler = messages_get};
    httpd_register_uri_handler(srv, &msgs);

    httpd_uri_t nodes = {.uri = "/api/nodes", .method = HTTP_GET, .handler = nodes_get};
    httpd_register_uri_handler(srv, &nodes);

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
