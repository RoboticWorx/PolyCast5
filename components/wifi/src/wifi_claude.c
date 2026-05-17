#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "cJSON.h"
#include "nvs.h"

#include "wifi_task.h"
#include "wifi_claude.h"

#define TAG "WIFI_CLAUDE"

// NVS namespace + key for the companion host URL (e.g. http://192.168.1.188:8765/usage)
#define CLAUDE_NS      "anthropic"
#define CLAUDE_KEY_URL "host_url"

// 15s between background polls
#define CLAUDE_POLL_INTERVAL_US (15ULL * 1000000ULL)

// HTTP-level safety net
#define CLAUDE_HTTP_TIMEOUT_MS 10000       // Companion is on LAN; 10s is generous
#define CLAUDE_HTTP_BODY_MAX   (16 * 1024) // Hard ceiling so a bad response can't eat RAM

// Dedicated fetch task - TLS handshake + JSON parse can push past 4KB
#define CLAUDE_FETCH_TASK_STACK (6 * 1024)

// Single-entry queue: LCD task peeks, fetch task overwrites
QueueHandle_t xWifiClaudeUsageQueue = NULL;

// LCD task gives this to ask for a refresh outside the 15s window
SemaphoreHandle_t xWifiClaudeRefreshSemaphore = NULL;

static int64_t s_last_fetch_us = 0;             // Timestamp of last attempt (success or fail)
static TaskHandle_t s_fetch_task_handle = NULL; // Non-NULL while a fetch is in flight
static volatile bool s_page_active = false;     // Set by lcd_tools_claude_usage_page on enter/exit

/* =============== HTTP body accumulator =============== */

typedef struct {
    char *buf;      // Accumulated body bytes (always NUL-terminated)
    size_t len;     // Current length (excluding NUL)
    size_t cap;     // Allocated capacity (includes room for NUL)
    bool oom;       // Set if a malloc/realloc failed
    bool truncated; // Set if we hit the cap and refused to grow further
} acc_t;

// Initialize accumulator with a starting capacity (clamped to >= 512)
static bool acc_init(acc_t *a, size_t initial_cap)
{
    // Clear state
    memset(a, 0, sizeof(*a));

    // Floor the cap so trivial responses don't trigger an immediate realloc
    if (initial_cap < 512) {
        initial_cap = 512;
    }

    // Allocate from regular heap (internal RAM); body is small enough not to need PSRAM
    a->buf = (char *)malloc(initial_cap);
    if (!a->buf) {
        a->oom = true;
        return false;
    }

    // Initialize as empty C-string
    a->cap = initial_cap;
    a->buf[0] = '\0';
    return true;
}

// Free the backing buffer and reset all fields
static void acc_free(acc_t *a)
{
    if (a && a->buf) {
        free(a->buf);
    }
    if (a) {
        memset(a, 0, sizeof(*a));
    }
}

// Grow the buffer so it can hold at least `need_cap` bytes
static bool acc_reserve(acc_t *a, size_t need_cap)
{
    // Already big enough
    if (need_cap <= a->cap) {
        return true;
    }

    // Double-until-fits sizing
    size_t nc = a->cap ? a->cap : 1024;
    while (nc < need_cap) {
        nc *= 2;
    }

    // Refuse to grow past the hard ceiling
    if (nc > CLAUDE_HTTP_BODY_MAX) {
        a->truncated = true;
        return false;
    }

    // Realloc; on failure keep the old buffer intact
    char *nb = (char *)realloc(a->buf, nc);
    if (!nb) {
        a->oom = true;
        return false;
    }

    // Commit the grow
    a->buf = nb;
    a->cap = nc;
    return true;
}

// esp_http_client event handler: append response body chunks into the accumulator
static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    acc_t *a = (acc_t *)evt->user_data;
    if (!a) {
        return ESP_OK;
    }

    // Connected: reset length so we never accidentally splice in stale bytes from a previous request that reused this accumulator
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        a->len = 0;
        if (a->buf && a->cap > 0) {
            a->buf[0] = '\0';
        }
        return ESP_OK;
    }

    // Body chunk: append and keep the buffer NUL-terminated for cJSON
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        // If we already know we're OOM or truncated, drop remaining bytes silently
        if (a->oom || a->truncated) {
            return ESP_OK;
        }

        // Ensure room for incoming bytes + NUL
        size_t n = (size_t)evt->data_len;
        if (!acc_reserve(a, a->len + n + 1)) {
            return ESP_OK;
        }

        // Copy chunk in and re-terminate
        memcpy(a->buf + a->len, evt->data, n);
        a->len += n;
        a->buf[a->len] = '\0';
    }

    return ESP_OK;
}

/* =============== NVS save/load =============== */

// Persist the companion-host URL to NVS
esp_err_t wifi_claude_save_config_nvs(const char *url)
{
    // Validate input
    if (!url) {
        return ESP_ERR_INVALID_ARG;
    }

    // Open the dedicated anthropic namespace for read/write
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLAUDE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Write the URL string and only commit on success
    err = nvs_set_str(h, CLAUDE_KEY_URL, url);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    // Close handle and return final status
    nvs_close(h);
    return err;
}

// Read the companion-host URL out of NVS into the caller's buffer
esp_err_t wifi_claude_load_config_nvs(char *url_out, size_t url_sz)
{
    // Validate buffer
    if (!url_out || url_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Open namespace read-only
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLAUDE_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    // nvs_get_str takes size as in/out, so seed it with the buffer capacity
    size_t sz = url_sz;
    err = nvs_get_str(h, CLAUDE_KEY_URL, url_out, &sz);

    // Close handle and return whatever nvs_get_str reported
    nvs_close(h);
    return err;
}

/* =============== Configuration check =============== */

// Decide whether the user has finished setup yet
bool wifi_claude_is_configured(void)
{
    // Local buffer for the URL read
    char url[WIFI_CLAUDE_URL_MAX_LEN] = {0};

    // Missing key in NVS
    if (wifi_claude_load_config_nvs(url, sizeof(url)) != ESP_OK) {
        return false;
    }

    // Present-but-empty also counts
    return url[0] != '\0';
}

/* =============== JSON parsing =============== */

// Extract a single metric sub-object {"pct":N,"reset_secs":N[,"unused":true]} into (pct_out, reset_out)
// When "unused":true is set or the node is missing pct_out is the WIFI_CLAUDE_PCT_UNUSED sentinel
static void parse_metric(cJSON *obj, uint8_t *pct_out, uint32_t *reset_out)
{
    // Default to unused so callers can short-circuit on missing fields
    *pct_out = WIFI_CLAUDE_PCT_UNUSED;
    *reset_out = 0;

    // Anything other than an object means absent/null - keep the defaults
    if (!cJSON_IsObject(obj)) {
        return;
    }

    // Honor the "unused": true flag from the companion script
    cJSON *unused = cJSON_GetObjectItem(obj, "unused");
    bool is_unused = cJSON_IsBool(unused) && cJSON_IsTrue(unused);

    // Pull "pct" (percentage) only if it's both a number and not flagged unused + clamp to [0,100].
    cJSON *pct = cJSON_GetObjectItem(obj, "pct");
    if (cJSON_IsNumber(pct) && !is_unused) {
        int v = (int)pct->valuedouble;

        if (v < 0) v = 0;
        if (v > 100) v = 100;

        *pct_out = (uint8_t)v;
    }

    // Reset time in seconds (time until limit resets)
    cJSON *rs = cJSON_GetObjectItem(obj, "reset_secs");
    if (cJSON_IsNumber(rs)) {
        double v = rs->valuedouble;

        if (v < 0) v = 0;
        if (v > (double)UINT32_MAX) v = (double)UINT32_MAX; // Clamp to uint32_t

        *reset_out = (uint32_t)v;
    }
}

// Parse the full companion-server response into a claude_usage_t
static bool parse_usage_json(const char *body, size_t len, claude_usage_t *out)
{
    // cJSON_ParseWithLength handles non-NUL-terminated buffers, but current already is terminated so either works
    cJSON *json = cJSON_ParseWithLength(body, len);
    if (!json) {
        ESP_LOGE(TAG, "JSON parse failed (len=%u)", (unsigned)len);
        return false;
    }

    // Map each of the four top-level keys produced by the companion script
    parse_metric(cJSON_GetObjectItem(json, "session"), &out->session_pct, &out->session_reset_secs);
    parse_metric(cJSON_GetObjectItem(json, "weekly"), &out->weekly_all_pct, &out->weekly_all_reset_secs);
    parse_metric(cJSON_GetObjectItem(json, "sonnet"), &out->weekly_sonnet_pct, &out->weekly_sonnet_reset_secs);
    parse_metric(cJSON_GetObjectItem(json, "design"), &out->claude_design_pct, &out->claude_design_reset_secs);

    // Free the cJSON tree (out fields are scalar copies, no dangling pointers)
    cJSON_Delete(json);
    return true;
}

/* =============== Public init =============== */

// Create the queue + manual-refresh semaphore
void wifi_claude_init(void)
{
    // Single-slot queue, populated via xQueueOverwrite so a slow LCD task never causes a backlog of stale snapshots
    if (!xWifiClaudeUsageQueue) {
        xWifiClaudeUsageQueue = xQueueCreate(1, sizeof(claude_usage_t));
        configASSERT(xWifiClaudeUsageQueue);
    }

    // Binary semaphore so multiple presses while a fetch is in flight collapse to a single re-fetch when that fetch finishes
    if (!xWifiClaudeRefreshSemaphore) {
        xWifiClaudeRefreshSemaphore = xSemaphoreCreateBinary();
        configASSERT(xWifiClaudeRefreshSemaphore);
    }
}

/* =============== Fetch path =============== */

// Issue the HTTP GET, accumulate the body, and parse it into `out`
static bool do_fetch(claude_usage_t *out)
{
    // Load the companion-host URL from NVS; bail out if user hasn't set it yet
    char url[WIFI_CLAUDE_URL_MAX_LEN] = {0};
    if (wifi_claude_load_config_nvs(url, sizeof(url)) != ESP_OK || url[0] == '\0') {
        return false;
    }

    // Initialize the body accumulator with a small starting capacity
    acc_t acc;
    if (!acc_init(&acc, 1024)) {
        return false;
    }

    // Configure esp_http_client: URL can be either http://... (LAN companion) or https://... (reverse-proxied)
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CLAUDE_HTTP_TIMEOUT_MS,
        .event_handler = http_evt,
        .user_data = &acc,
    };

    // Spin up the client; bail (and free accumulator) on failure
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        acc_free(&acc);
        return false;
    }

    // Hint to the companion that we want JSON
    esp_http_client_set_header(client, "Accept", "application/json");

    // Perform the request (blocks until done or timeout). Body chunks land in
    // `acc` via the http_evt callback.
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    // Release HTTP resources before any branching so we don't leak on failures
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Network/TLS failure - DNS, refused, timeout, etc.
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
        acc_free(&acc);
        return false;
    }

    // Companion responded but with non-200 (e.g. 503 "no data yet")
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d from companion host", status);
        acc_free(&acc);
        return false;
    }

    // Sanity-check the body before we feed it to cJSON
    if (acc.oom || acc.truncated || acc.len == 0 || !acc.buf) {
        ESP_LOGW(TAG, "Bad body: len=%u oom=%d trunc=%d", (unsigned)acc.len, acc.oom, acc.truncated);
        acc_free(&acc);
        return false;
    }

    // Parse the JSON and write into the caller-provided struct
    bool parsed = parse_usage_json(acc.buf, acc.len, out);
    acc_free(&acc);
    return parsed;
}

// Runs in its own task so TLS handshake + JSON parsing don't blow the wifi_task's 4KB stack
static void claude_fetch_task(void *param)
{
    (void)param;

    // Build the snapshot, then stamp it with the result + timestamp
    claude_usage_t usage = {0};
    bool ok = do_fetch(&usage);

    usage.ok = ok;
    usage.fetched_at_us = esp_timer_get_time();

    // Advance the throttle window for both success and failure so a broken companion host doesn't get retried every 10ms
    s_last_fetch_us = usage.fetched_at_us;

    // Publish via overwrite so the LCD always sees the freshest snapshot
    if (xWifiClaudeUsageQueue) {
        xQueueOverwrite(xWifiClaudeUsageQueue, &usage);
    }

    // Mark the slot free so wifi_claude_tick() can spawn the next fetch
    s_fetch_task_handle = NULL;
    vTaskDelete(NULL);
}

/* =============== Tick =============== */

// Called from wifi_task's main loop (~every 10 ms)
// Decides whether to spawn a fresh claude_fetch_task based on throttle window, Wi-Fi state, and manual-refresh semaphore
void wifi_claude_tick(void)
{
    // Guard against being called before wifi_claude_init()
    if (!xWifiClaudeUsageQueue || !xWifiClaudeRefreshSemaphore) {
        return;
    }

    // Only poll while the user is viewing the Claude Usage page
    if (!s_page_active) {
        return;
    }

    // Don't stack fetches
    if (s_fetch_task_handle != NULL) {
        return;
    }

    // Drain a pending manual refresh, if any
    bool manual = (xSemaphoreTake(xWifiClaudeRefreshSemaphore, 0) == pdTRUE);

    // Throttle background polls to CLAUDE_POLL_INTERVAL_US; manual refresh bypasses it
    int64_t now_us = esp_timer_get_time();
    if (!manual && s_last_fetch_us != 0 && ((now_us - s_last_fetch_us) < (int64_t)CLAUDE_POLL_INTERVAL_US)) {
        return;
    }

    // Don't try if Wi-Fi is down
    if (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
        return;
    }

    // Skip if user hasn't completed setup yet
    if (!wifi_claude_is_configured()) {
        s_last_fetch_us = now_us;
        return;
    }

    // Spawn the fetch
    if (xTaskCreate(claude_fetch_task, "claude_fetch", CLAUDE_FETCH_TASK_STACK,
            NULL, POLYCAST5_PRIORITY_MEDIUM, &s_fetch_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn claude_fetch_task");
        s_fetch_task_handle = NULL;
        s_last_fetch_us = now_us;
    }
}

void wifi_claude_set_page_active(bool active)
{
    s_page_active = active;
}
