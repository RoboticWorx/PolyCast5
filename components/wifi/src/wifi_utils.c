#include "polycast5_macros.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "wifi_utils.h"
#include "wifi_autoconnect.h"
#include "wifi_mqtt.h"
#include "wifi_ping.h"
#include "gpio_utils.h"
#include "espnow_utils.h"
#include "wifi_ota_update.h"
#include "ai_utils.h"

#include "wifi_task.h"
#include "gpio_task.h"

#define TAG "WIFI_FUNCS"

#define RAW_HEX_BUF_CAP (AI_CMD_MAX_LEN) // AI_CMD_MAX_LEN PSRAM for hex strings

// extern to wifi_ping.c
extern esp_ip4_addr_t sta_gw;
extern bool sta_gw_valid;

extern bool last_known_network_conn_failed; // wifi_autoconnect.c

bool wifi_reconnect_with_scan = false;

// extern to lcd_wifi.c
POLYCAST5_USE_PSRAM_BSS char raw_frames_hex_buf[RAW_HEX_BUF_CAP]; // Accumulated hex strings
size_t raw_frames_hex_len = 0; // Current length
uint32_t raw_frames_captured = 0; // Counter

static uint8_t target_bssid[6] = {0};

POLYCAST5_USE_PSRAM_BSS static wifi_data_t wifi_data;

esp_err_t wifi_utils_scan(wifi_scan_t *wifi_scan)
{
    esp_err_t err;

    // Scan all SSIDs and channels
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0, // 0 = scan all channels
        .show_hidden = true
    };

    // Start scan (true = block until scan done)
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // How many APs were found
    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
        return err;
    }

    // If no networks found
    if (ap_num == 0) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "esp_wifi_scan_get_ap_num: No access points found");
#endif

        wifi_scan_t sentinel = {0};

        // Use an impossible auth value as a sentinel marker
        sentinel.auth = 0xFF;

        // Signal LCD no APs found
        if (xQueueSend(xWifiScanQueue, &sentinel, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "xWifiScanQueue: Failed to enqueue 'no APs' sentinel");
        }

        // Exit without error
        return ESP_OK;
    }

    // Allocate array to hold results
    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_list) {
        ESP_LOGE(TAG, "malloc for ap_list failed");
        return ESP_ERR_NO_MEM;
    }

    // Pull the records
    err = esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        free(ap_list);
        return err;
    }
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Found %d access point(s):", ap_num);
    for (int i = 0; i < ap_num; ++i) {
        ESP_LOGI(TAG,
                "[%d] SSID: %-32s BSSID: %02x:%02x:%02x:%02x:%02x:%02x RSSI: %3d  CH:%2d  AUTH:%d",
                i,
                (char*)ap_list[i].ssid,
                ap_list[i].bssid[0], ap_list[i].bssid[1],
                ap_list[i].bssid[2], ap_list[i].bssid[3],
                ap_list[i].bssid[4], ap_list[i].bssid[5],
                ap_list[i].rssi,
                ap_list[i].primary,
                ap_list[i].authmode
        );
    }
#endif

    // Build a list of unique SSIDs, keeping the strongest RSSI for each
    size_t unique_count = 0;

    for (uint16_t i = 0; i < ap_num && unique_count < WIFI_MAX_NETWORKS; ++i) {
        const char *ssid = (char *)ap_list[i].ssid;

        // Skip blank SSIDs
        if (ssid[0] == '\0') {
            continue;
        }

        bool found = false;
        size_t existing_idx = 0;

        // See if we've already recorded this SSID
        for (size_t j = 0; j < unique_count; ++j) {
            if (strncmp((char *)wifi_scan[j].ssid, ssid, sizeof(wifi_scan[j].ssid)) == 0) {
                found = true;
                existing_idx = j;
                break;
            }
        }

        // New SSID: add a fresh entry
        if (!found) {
            // Copy the SSID
            strlcpy((char *)wifi_scan[unique_count].ssid, ssid, sizeof(wifi_scan[unique_count].ssid));

            // Copy the BSSID
            memcpy(wifi_scan[unique_count].bssid, ap_list[i].bssid, sizeof(ap_list[i].bssid));

            // Fill the rest
            wifi_scan[unique_count].rssi = ap_list[i].rssi;
            wifi_scan[unique_count].channel = ap_list[i].primary;
            wifi_scan[unique_count].auth = ap_list[i].authmode;

            unique_count++;
        } else { // Same SSID as an existing one
            // Keep the stronger AP
            if (ap_list[i].rssi > wifi_scan[existing_idx].rssi) {
                // Copy over since larger RSSI
                memcpy(wifi_scan[existing_idx].bssid, ap_list[i].bssid, sizeof(ap_list[i].bssid));

                wifi_scan[existing_idx].rssi = ap_list[i].rssi;
                wifi_scan[existing_idx].channel = ap_list[i].primary;
                wifi_scan[existing_idx].auth = ap_list[i].authmode;
            }
        }
    }

    // Push the unique SSIDs to the LCD queue
    for (size_t i = 0; i < unique_count; ++i) {
        if (xQueueSend(xWifiScanQueue, &wifi_scan[i], portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "xWifiScanQueue: Failed to enqueue #%u", (unsigned)i);
        }
    }

    free(ap_list);
    return ESP_OK;
}

static const struct { const char *iana; const char *posix; } TZ_MAP[] = {
    // Majors
    {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"America/Phoenix", "MST7"}, // No DST
    {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
    {"America/Adak", "HAST10HADT,M3.2.0/2,M11.1.0/2"}, // Aleutian (has DST)
    {"Pacific/Honolulu", "HST10"}, // No DST

    // Territories
    {"America/Puerto_Rico", "AST4"}, // No DST
    {"America/St_Thomas", "AST4"}, // USVI, no DST
    {"Pacific/Guam", "ChST-10"}, // UTC+10, no DST
    {"Pacific/Saipan", "ChST-10"}, // Same
    {"Pacific/Pago_Pago", "SST11"}, // UTC-11, no DST

    // Useful aliases (map to their major rules)
    {"America/Detroit", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Kentucky/Louisville", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Kentucky/Monticello", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Indianapolis", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Marengo", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Vevay", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Vincennes", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Winamac", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Petersburg", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Knox", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/Indiana/Tell_City", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/North_Dakota/Center", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/North_Dakota/New_Salem", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/North_Dakota/Beulah", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/Boise", "MST7MDT,M3.2.0/2,M11.1.0/2"},
};

// Find a POSIX rule for a given IANA ID (exact match); returns NULL if unmapped
static const char* iana_to_posix(const char *iana)
{
    // Iterate the table
    for (size_t i = 0; i < sizeof(TZ_MAP) / sizeof(TZ_MAP[0]); ++i) {
        // Compare IANA strings
        if (strcmp(iana, TZ_MAP[i].iana) == 0) {
            // Return mapped POSIX rule
            return TZ_MAP[i].posix;
        }
    }
    // Not found in our compact table
    return NULL;
}

// Apply a fixed-offset POSIX TZ built from API offsets (correct "now", no future DST rules)
static void tz_apply_fixed_posix_from_offsets(int raw_offset_s, int dst_offset_s, bool dst_now)
{
    // Combine base UTC offset and DST add-on (seconds east of UTC, negative for the Americas)
    int total = raw_offset_s + (dst_now ? dst_offset_s : 0);

    // POSIX sign is inverted relative to UTC (UTC-4 → "UTC+4")
    int sec = -total;

    // Capture sign and make value positive for formatting
    int sign = (sec < 0) ? -1 : 1;
    sec = (sec < 0) ? -sec : sec;

    // Split into hours and minutes
    int h = sec / 3600;
    int m = (sec % 3600) / 60;

    // Format buffer
    char tzbuf[32];

    // Render "UTC±H" or "UTC±H:MM"
    if (m == 0) {
        // Whole-hour offset
        snprintf(tzbuf, sizeof(tzbuf), "UTC%+d", sign * h);
    } else {
        // Sub-hour offset (e.g., :30, :45)
        snprintf(tzbuf, sizeof(tzbuf), "UTC%+d:%02d", sign * h, m);
    }

    // Set the TZ environment variable
    setenv("TZ", tzbuf, 1);

    // Apply the TZ immediately
    tzset();

    // Log the applied fixed-offset TZ
#ifdef POLYCAST5_DEBUG
    ESP_LOGI("AUTO_TZ", "Applied fixed-offset TZ: %s", tzbuf);
#endif
}

// Simple retrying HTTP GET (chunk-safe). Returns true with malloc'd body on HTTP 200
static bool http_get_body_retry(const char *url, char **out_body, size_t *out_len)
{
    // Attempt count
    const int attempts = 3;

    // Try multiple times with short backoff
    for (int i = 0; i < attempts; ++i) {
        // Configure HTTP client (HTTP, short timeout)
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 5000,
        };

        // Create client handle
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);

        // If init failed, retry
        if (!cli) {
            continue;
        }

        // Some middleboxes dislike keep-alive; request connection close
        esp_http_client_set_header(cli, "Connection", "close");

        // Identify ourselves
        esp_http_client_set_header(cli, "User-Agent", "esp32");

        // Open the connection (sends GET)
        esp_err_t err = esp_http_client_open(cli, 0);

        // If open failed, cleanup and retry
        if (err != ESP_OK) {
            esp_http_client_cleanup(cli);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        // Parse headers (content length may be unknown/chunked)
        (void)esp_http_client_fetch_headers(cli);

        // Initial buffer capacity and current length
        size_t cap = 1024;
        size_t len = 0;

        // Allocate body buffer
        char *body = (char*)malloc(cap);

        // If malloc failed, cleanup and abort
        if (!body) {
            esp_http_client_cleanup(cli);
            return false;
        }

        // Read until EOF or error
        while (1) {
            // Temporary read chunk
            char buf[512];

            // Read from socket
            int r = esp_http_client_read(cli, buf, sizeof(buf));

            // On read error, free and mark as failed
            if (r < 0) {
                free(body);
                body = NULL;
                break;
            }

            // r == 0 means EOF (server closed after sending body)
            if (r == 0) {
                break;
            }

            // Grow the buffer if needed (+1 for terminating NUL)
            if (len + (size_t)r + 1 > cap) {
                // Double the capacity to amortize reallocs
                size_t nc = (cap + (size_t)r + 1) * 2;

                // Attempt to grow the buffer
                char *nb = (char*)realloc(body, nc);

                // If realloc fails, free and mark as failed
                if (!nb) {
                    free(body);
                    body = NULL;
                    break;
                }

                // Accept the grown buffer
                body = nb;
                cap = nc;
            }

            // Append chunk into the body buffer
            memcpy(body + len, buf, (size_t)r);

            // Advance total written
            len += (size_t)r;
        }

        // Capture HTTP status
        int status = esp_http_client_get_status_code(cli);

        // Cleanup the client object
        esp_http_client_cleanup(cli);

        // If we have a body and HTTP 200 OK, return success
        if (body && status == 200) {
            // NUL-terminate the body
            body[len] = '\0';

            // Return body pointer to caller
            *out_body = body;

            // Optionally output length
            if (out_len) {
                *out_len = len;
            }

            // Indicate success
            return true;
        }

        // If body was allocated but status was not OK, free it
        if (body) {
            free(body);
        }

        // Small backoff before retrying
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // All attempts failed
    return false;
}

// Trim ASCII whitespace in-place (both ends) on a mutable C string
static void strtrim_inplace(char *s)
{
    // Null guard
    if (!s) {
        return;
    }

    // Find first non-space
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }

    // Move content to the front if needed
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    // Find new end
    char *end = s + strlen(s);

    // Walk back over trailing spaces
    while (end > s && isspace((unsigned char)*(end - 1))) {
        --end;
    }

    // Terminate after last non-space
    *end = '\0';
}

// Fetch IANA timezone over HTTP, map to POSIX or apply fixed-offset; returns ESP_OK on success
esp_err_t wifi_utils_apply_timezone_auto(void)
{
    // Response body buffer
    char *body = NULL;

    // Default result is failure (caller may set TZ=UTC0 on failure)
    esp_err_t ret = ESP_FAIL;

    // Try #1: worldtimeapi.org (JSON: timezone + raw_offset/dst_offset/dst)
    if (http_get_body_retry("http://worldtimeapi.org/api/ip", &body, NULL)) {
        // Parse JSON
        cJSON *root = cJSON_Parse(body);

        // Free body buffer now that it's parsed
        free(body);
        body = NULL;

        // If JSON parsed
        if (root) {
            // Extract fields
            const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");
            const cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "raw_offset");
            const cJSON *dst_off = cJSON_GetObjectItemCaseSensitive(root, "dst_offset");
            const cJSON *dst_now = cJSON_GetObjectItemCaseSensitive(root, "dst");

            // Read IANA string if present
            const char *iana = (cJSON_IsString(tz) && tz->valuestring) ? tz->valuestring : NULL;

            // If IANA available
            if (iana) {
                // Try mapping to a full POSIX rule
                const char *posix = iana_to_posix(iana);

                // If mapping found, apply it
                if (posix) {
                    // Set POSIX TZ
                    setenv("TZ", posix, 1);

                    // Apply immediately
                    tzset();

                    // Log success
#ifdef POLYCAST5_DEBUG
                    ESP_LOGI("AUTO_TZ", "Applied TZ: IANA='%s' -> POSIX='%s'", iana, posix);
#endif

                    // Mark success
                    ret = ESP_OK;
                }
                // If unmapped but we have offsets, apply fixed-offset POSIX
                else if (cJSON_IsNumber(raw) && cJSON_IsNumber(dst_off) && cJSON_IsBool(dst_now)) {
                    // Apply fixed-offset TZ that is correct "now"
                    tz_apply_fixed_posix_from_offsets(raw->valueint, dst_off->valueint, cJSON_IsTrue(dst_now));

                    // Mark success
                    ret = ESP_OK;
                }
            }

            // Free JSON object
            cJSON_Delete(root);

            // If success, return immediately
            if (ret == ESP_OK) {
                return ret;
            }
        }
    }

    // Try #2: ip-api.com (JSON with "timezone" only; no offsets)
    if (http_get_body_retry("http://ip-api.com/json", &body, NULL)) {
        // Parse JSON
        cJSON *root = cJSON_Parse(body);

        // Free body buffer
        free(body);
        body = NULL;

        // If JSON parsed
        if (root) {
            // Extract "timezone" (IANA)
            const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezone");

            // Pull C string if present
            const char *iana = (cJSON_IsString(tz) && tz->valuestring) ? tz->valuestring : NULL;

            // If IANA present
            if (iana) {
                // Map to POSIX (table only; no offsets on this endpoint)
                const char *posix = iana_to_posix(iana);

                // If mapped, apply and succeed
                if (posix) {
                    // Set POSIX TZ
                    setenv("TZ", posix, 1);

                    // Apply immediately
                    tzset();

                    // Log success (ip-api path)
#ifdef POLYCAST5_DEBUG
                    ESP_LOGI("AUTO_TZ", "Applied TZ: IANA='%s' -> POSIX='%s' (ip-api)", iana, posix);
#endif

                    // Mark success
                    ret = ESP_OK;
                }
            }

            // Free JSON object
            cJSON_Delete(root);

            // If success, return immediately
            if (ret == ESP_OK) {
                return ret;
            }
        }
    }

    // Try #3: ipapi.co/timezone (plain text IANA string)
    if (http_get_body_retry("http://ipapi.co/timezone", &body, NULL)) {
        // Trim whitespace/newlines
        strtrim_inplace(body);

        // If non-empty string
        if (body[0]) {
            // Map to POSIX
            const char *posix = iana_to_posix(body);

            // If mapped, apply and succeed
            if (posix) {
                // Set POSIX TZ
                setenv("TZ", posix, 1);

                // Apply immediately
                tzset();

                // Log success (ipapi path)
#ifdef POLYCAST5_DEBUG
                ESP_LOGI("AUTO_TZ", "Applied TZ: IANA='%s' -> POSIX='%s' (ipapi)", body, posix);
#endif

                // Mark success
                ret = ESP_OK;
            }
        }

        // Free body buffer
        free(body);
        body = NULL;

        // If success, return immediately
        if (ret == ESP_OK) {
            return ret;
        }
    }

    // All providers failed or we couldn't map; let caller fall back to UTC0
    return ESP_FAIL;
}

void wifi_utils_get_current_date_time(void)
{
    static bool initialized = false;
    
    if (!initialized) {
        // Tell SNTP client to poll for time
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    
        // Point SNTP client at a given server
        esp_sntp_setservername(0, "pool.ntp.org"); // or time.nist.gov
    
        // Init and start SNTP service
        esp_sntp_init();
        
        initialized = true;
    }
    
    time_t now = 0;
    struct tm timeinfo = {0};

    // Wait until the SNTP task clock has gone past 2025
    while (timeinfo.tm_year < (2025 - 1900)) {
        vTaskDelay(pdMS_TO_TICKS(1));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    // Get local time zone over http
    if (wifi_utils_apply_timezone_auto() != ESP_OK) {
        // Fallback
        //setenv("TZ", "UTC0", 1); // UTC
        setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1); // Fallback to EST
        tzset();
        
        ESP_LOGE(TAG, "wifi_utils_apply_timezone_auto FAILED: Falling back to EST (EST5EDT,M3.2.0/2,M11.1.0/2)");
    }
    
    char strftime_buf[64];

    // Get the epoch time
    time(&now);
    
    // Convert to a local broken-out form
    localtime_r(&now, &timeinfo);
    // 'time()' returns seconds since Jan 1 1970 UTC,
    // 'localtime_r()' applies your TZ rules into a 'struct tm'.

    // Render it as 'YYYY-MM-DD HH:MM:SS' into our buffer
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Current date/time 24h: %s", strftime_buf);
#endif
    
    // 12-hour format with AM/PM:
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %I:%M:%S %p", &timeinfo);
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Current date/time 12h: %s", strftime_buf);
#endif

    // Notify time acquired
    xEventGroupSetBits(xWifiEventGroup, WIFI_GOT_DATE_TIME_BIT); // Set got time
}

static const char* wifi_disconnect_reason_str(uint8_t r)
{
    switch (r) {
        case WIFI_REASON_AUTH_EXPIRE:            return "AUTH_EXPIRE";
        case WIFI_REASON_AUTH_FAIL:              return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:             return "ASSOC_FAIL";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:      return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND:            return "NO_AP_FOUND";
        default:                                 return "UNKNOWN";
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    // Disconnection event
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)data;

#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Disconnected, reason=%d (%s)", d->reason, wifi_disconnect_reason_str(d->reason));
#endif
        wifi_utils_radio_stop();

        // Notify we disconnected
        xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTED_BIT | WIFI_MQTT_CONNECTED_BIT | WIFI_CONNECTING_BIT);

        // Disconnected icon
        xEventGroupClearBits(xConnectionIconEventGroup, ICON_BIT_WIFI_CONNECTED);
        
        // RGB indicator
        uint8_t rgb_state = RGB_SET_OFF;
        xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { // Connected event
        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Got IP: " IPSTR ", GW: " IPSTR,
                IP2STR(&e->ip_info.ip),
                IP2STR(&e->ip_info.gw));
#endif
        // Save gateway so we can ping it later
        sta_gw = e->ip_info.gw;
        sta_gw_valid = true;

        // Notify we connected
        xEventGroupSetBits(xWifiEventGroup, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTING_BIT); // No longer trying to connect
        
        // Connected icon
        xEventGroupSetBits(xConnectionIconEventGroup, ICON_BIT_WIFI_CONNECTED);
        
        // RGB indicator
        uint8_t rgb_state = RGB_SET_GREEN;
        xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);

        wifi_autoconnect_remember_current_network();

        // If WIFI_CHECK_OTA_ON_CONN_BIT is set, check for OTA firmware update on this connection
        if (xEventGroupGetBits(xWifiEventGroup) & WIFI_CHECK_OTA_ON_CONN_BIT) {
            // Check for new firmware version and update if so
            wifi_ota_update_check_start("https://raw.githubusercontent.com/RoboticWorx/pc5-test/main/manifest.json");

            // Clear the bit now that we've acted on it
            xEventGroupClearBits(xWifiEventGroup, WIFI_CHECK_OTA_ON_CONN_BIT);
        } else {
            // No OTA check pending, start MQTT client now
            wifi_mqtt_client_start();
        }
    }
}

void wifi_utils_wifi_event_init(void)
{
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
                 
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
}

esp_err_t wifi_utils_connect(void)
{
    // Fresh start
    xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTED_BIT | WIFI_MQTT_CONNECTED_BIT);
    
    esp_err_t err = esp_wifi_connect();
    
    // Wait for connection or timeout
    if (xEventGroupWaitBits(xWifiEventGroup, WIFI_CONNECTED_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONN_SCAN_TIMEOUT_MS)) & WIFI_CONNECTED_BIT) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Wi-Fi connected and got IP!");
#endif
        last_known_network_conn_failed = false;
        wifi_reconnect_with_scan = false;
        // wifi_mqtt_client_start() called after checking for OTA update
    } else {
        ESP_LOGE(TAG, "wifi_utils_connect: Timeout. Failed to connect.");

        last_known_network_conn_failed = true;
        wifi_utils_radio_stop();

        // Try again once with scan
        if (!wifi_reconnect_with_scan) {
            wifi_reconnect_with_scan = true;

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Connection failed. Will retry connection with a scan.");
#endif
        } else {
            // Both attempts failed
            wifi_reconnect_with_scan = false;

            // Notify LCD we failed to connect
            xEventGroupSetBits(xWifiEventGroup, WIFI_CONNECTING_FAILED_BIT);
            // No longer trying to connect
            xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTING_BIT);
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "Connection failed. Exiting...");
#endif
        }
    }
    
    return err;
}

esp_err_t wifi_utils_radio_start(const char *ssid, const uint8_t* bssid, const char *password)
{
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};

    // Copy in SSID and password
    strlcpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password));
    
    // Copy BSSID
    // cfg.sta.bssid_set = true;
    // memcpy(cfg.sta.bssid, bssid, sizeof(cfg.sta.bssid));

    cfg.sta.channel = 0; // Don't lock to a specific channel
    cfg.sta.scan_method = WIFI_FAST_SCAN; // First matching SSID (vs WIFI_ALL_CHANNEL_SCAN)
    
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // Weakest auth mode to accept in the fast scan mode 

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Setting Wi-Fi config SSID='%s'", ssid);
    // ESP_LOGI(TAG, "Setting Wi-Fi config BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
    //         bssid[0], bssid[1], bssid[2],
    //         bssid[3], bssid[4], bssid[5]);
    ESP_LOGI(TAG, "Setting Wi-Fi config password='%s'", password);
#endif
    
    // Set mode
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    // Set config
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    
    // Start the driver
    err = esp_wifi_start();
    
    return err;
}

esp_err_t wifi_utils_radio_stop(void)
{
    // Stop promiscuous sniffing if enabled
    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_set_promiscuous_rx_cb(NULL);

    // Stop client if started
    if (xEventGroupGetBits(xWifiEventGroup) & WIFI_MQTT_CONNECTED_BIT) {
        wifi_mqtt_client_stop();
    }

    esp_err_t err = esp_wifi_disconnect(); // Disconnect if connected
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "wifi_utils_radio_stop esp_wifi_disconnect error: %s", esp_err_to_name(err));
    }
    
    // Stop Wi-Fi
    err = esp_wifi_stop();
    if (err == ESP_OK) {
        xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(xWifiEventGroup, WIFI_CONNECTING_BIT); // No longer trying to connect
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "wifi_utils_radio_stop success");
#endif
    } else {
        ESP_LOGE(TAG, "wifi_utils_radio_stop esp_wifi_stop error: %s", esp_err_to_name(err));
    }
    
    xSemaphoreGive(xWifiCanSleepSemaphore);

    memset(&wifi_data, 0, sizeof(wifi_data)); // Zero out wifi_data if initialized
    
    return err;
}

esp_err_t wifi_utils_radio_cycle(void)
{
    wifi_mode_t mode;
    esp_err_t err;

    // If Wi-Fi has never been initialized, bail out quickly
    err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "wifi_utils_radio_cycle: esp_wifi_get_mode failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // If Wi-Fi was never configured, there is nothing to nudge
    if (mode == WIFI_MODE_NULL) {
        return ESP_OK;
    }

    // Do a minimal start/stop cycle - no need to actually connect to an AP

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "wifi_utils_radio_cycle: nudging Wi-Fi driver after sleep");
#endif

    // Start Wi-Fi
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "wifi_utils_radio_cycle: esp_wifi_start: %s", esp_err_to_name(err));
#endif
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay to let driver settle

    // Stop Wi-Fi
    err = esp_wifi_stop();
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "wifi_utils_radio_cycle: esp_wifi_stop: %s", esp_err_to_name(err));
#endif
        return err;
    }

    return ESP_OK;
}

wifi_login_t wifi_utils_get_prev(void)
{
    wifi_config_t current = {0};
    wifi_login_t prev = {0};

    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &current);
	if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get Wi-Fi config: %s", esp_err_to_name(err));
    } else {
        strlcpy(prev.ssid, (char *)current.sta.ssid, sizeof(prev.ssid));
        strlcpy(prev.password, (char *)current.sta.password, sizeof(prev.password));
    }
    
    return prev;
}

// Read a uint16 from a byte array in little-endian order (LSB first)
static inline uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Check suite selector OUI bytes (e.g., 00:0F:AC for RSN)
static inline bool is_oui(const uint8_t *p, uint8_t a, uint8_t b, uint8_t c)
{
    // Checks if the first 3 bytes at p match an OUI (Organizationally Unique Identifier)
    return (p[0] == a && p[1] == b && p[2] == c);
}

// Parses the body of the RSN IE
// (not including the id and len bytes of the IE wrapper)
void wifi_utils_parse_rsn_ie(const uint8_t *rsn, size_t rsn_len, wifi_beacon_t *out)
{
    // RSN IE simplified layout:
    //     version (2) +
    //     group_cipher_suite (4) +
    //     pairwise_cipher_count (2) +
    //     pairwise_cipher_list (4 * N) +
    //     akm_count (2) +
    //     akm_list (4 * M) +
    //     rsn_capabilities (2) +
    //     optional extras (PMKID list, group mgmt cipher, etc.)

    // Validate inputs
    if (!rsn || !out || rsn_len < 2) {
        return;
    }

    // Read the RSN version
    size_t off = 0;
    uint16_t ver = rd_le16(&rsn[off]);
    off += 2;
    if (ver != 1) {
        // Still try to parse, but version 1 is expected.
    }

    // Ensure there are 4 bytes to read
    if (off + 4 > rsn_len) {
        return;
    }

    // grp points to the 4-byte "suite selector"
    const uint8_t *grp = &rsn[off];
    off += 4;

    // If OUI matches RSN (00:0F:AC), store the suite type
    if (is_oui(grp, 0x00, 0x0F, 0xAC)) {
        out->rsn_group_cipher = grp[3];
    }

    // Common types:
    //  2 = TKIP (legacy)
    //  4 = CCMP-128 (AES/CCMP, typical WPA2)
    //  8 = GCMP-128 (newer)

    // Read pairwise cipher suites
    if (off + 2 > rsn_len) {
        return;
    }
    uint16_t pairwise_cnt = rd_le16(&rsn[off]);
    off += 2;

    // For each cipher
    for (uint16_t i = 0; i < pairwise_cnt; ++i) {
        // Bounds check
        if (off + 4 > rsn_len) {
            return;
        }

        const uint8_t *pcs = &rsn[off];
        off += 4;

        // Verify OUI
        if (is_oui(pcs, 0x00, 0x0F, 0xAC)) {
            // Read suite type
            uint8_t ctype = pcs[3];

            // Store it into a bitmask: 1u << ctype

            // 1u << ctype on a 32-bit int becomes undefined /
            // wrong if ctype is 32 or more. This is a safety guard
            if (ctype < 32) {
                out->rsn_pairwise_ciphers |= (1u << ctype);
            }
        }
    }

    // AKM suites
    if (off + 2 > rsn_len) {
        return;
    }

    // Read AKM Suites (auth/key-management)
    uint16_t akm_cnt = rd_le16(&rsn[off]);
    off += 2;

    // Grabs how the keys are negotiated / what auth scheme is used
    // Common suite types:
    //  2 = PSK (WPA2-Personal)
    //  1 = 802.1X (WPA2-Enterprise)
    //  8 = SAE (WPA3-Personal)
    //  18 = OWE (Enhanced Open)

    for (uint16_t i = 0; i < akm_cnt; ++i) {
        if (off + 4 > rsn_len) {
            return;
        }

        const uint8_t *akm = &rsn[off];
        off += 4;

        // Check if OUI bytes
        if (is_oui(akm, 0x00, 0x0F, 0xAC)) {
            uint8_t atype = akm[3];
            if (atype < 32) {
                out->rsn_akm_suites |= (1u << atype);
            }
        }
    }

    // Reads the 2-byte RSN Capabilities field and grabs the
    // two bits relevant to Protected Management Frames:
    //     bit 6: MFPC (capable)
    //     bit 7: MFPR (required)

    // RSN capabilities (PMF bits live here)
    if (off + 2 > rsn_len) {
        return;
    }
    uint16_t caps = rd_le16(&rsn[off]);

    // 802.11w / PMF bits:
    // bit 6 = MFPC (capable), bit 7 = MFPR (required)
    out->pmf_capable  = (caps & (1u << 6)) != 0;
    out->pmf_required = (caps & (1u << 7)) != 0;
}

static void wifi_sniffer_beacon_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    static volatile uint32_t frames_seen = 0;
    frames_seen++;
    
#ifdef POLYCAST5_DEBUG
    if (frames_seen % 100 == 0) { // Every 100 frames
        ESP_LOGI(TAG, "sniffer: %u frames so far\r\n", frames_seen);
    }
#endif

    // Make sure only management frames
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    
    // Points to the raw 802.11 frame bytes
    uint8_t *frame = pkt->payload;
    
    // frame_ctrl == first two bytes
    uint16_t frame_ctrl = (frame[1] << 8) | frame[0];
    
    // bits [3:2] give Type (00=Mgmt, 01=Control, 10=Data)
    // bits [7:4] give Subtype (1000=Beacon)
    // Check if management beacon frame
    if (FC_TYPE(frame_ctrl) != TYPE_MGMT || FC_SUBTYPE(frame_ctrl) != SUBTYPE_BEACON) {
        return;
    }
    
    // Addresses: bytes 4–9 = DA, 10–15 = SA, 16–21 = BSSID (for beacon)
    uint8_t *bssid = &frame[16];
    
    // Filter for target BSSID
    if (memcmp(bssid, target_bssid, 6) != 0) {
        return;
    }

    // After the 24-byte MAC header comes:
    // 8 bytes timestamp | 2 bytes interval | 2 bytes capability info
    uint8_t *fixed = frame + 24;
    uint64_t timestamp;
    uint16_t interval, cap_info;
    memcpy(&timestamp, fixed + 0, sizeof(timestamp));
    memcpy(&interval, fixed + 8, sizeof(interval));
    memcpy(&cap_info, fixed + 10, sizeof(cap_info));
    
    // Convert timestamp from µs to days (time since last reboot)
    uint64_t timestamp_seconds = timestamp / 1000000; 
    uint64_t timestamp_days = timestamp_seconds / (24 * 60 * 60); 
    
    uint8_t *ie = fixed + 12; // Information element (IE) starts 8 + 2 + 2 from 24
    int rem = pkt->rx_ctrl.sig_len - (ie - frame); // Remaining length of packet
    
    int channel = pkt->rx_ctrl.channel; // Works on both 2.4GHz and 5GHz
    bool has_rsn = false, has_wpa = false; // Flags to see if they exist
    char ssid[33] = {0}; // SSID buffer

    wifi_beacon_t beacon = {0};

    // For Wi-Fi security
    beacon.rsn_group_cipher = 0;
    beacon.rsn_pairwise_ciphers = 0;
    beacon.rsn_akm_suites = 0;
    beacon.pmf_capable = false;
    beacon.pmf_required = false;
    beacon.wps = false;

    // For Wi-Fi PHY
    bool has_ht = false;
    bool has_vht = false;
    bool has_he = false;
    // For b vs g inference on 2.4 GHz
    bool has_ofdm_rates = false;
    bool has_cck_rates = false;
    
    // Walk through bytes
    while (rem >= 2) {
        uint8_t id = ie[0], len = ie[1]; // Tag ID and packet length
        
        // Make sure tag valid
        if (len + 2 > rem) {
            break;
        }
        
        uint8_t *data = ie + 2; // Data pointer
    
        switch (id) {
            case 0: // SSID
                if (len < sizeof(ssid)) {
                    memcpy(ssid, data, len);
                }
                break;
                
            case 3: // DS Parameter Set (2.4 GHz channel)
                if (channel == 0) {
                    channel = data[0];
                }
                break;
                
            case 48: // RSN (WPA2/WPA3) IE
                has_rsn = true;
                wifi_utils_parse_rsn_ie(data, len, &beacon);
                break;

            case 1: // case 1 or 50: Supported Rates
            case 50:
                // Extended Supported Rates
                for (int i = 0; i < len; i++) {
                    uint8_t r = data[i] & 0x7F; // 500 kbps units
                    // CCK rates (1, 2, 5.5, 11 Mbps) -> 2, 4, 11, 22
                    if (r == 2 || r == 4 || r == 11 || r == 22) {
                        has_cck_rates = true;
                    }
                    // OFDM rates (6..54 Mbps) -> 12, 18, 24, 36, 48, 72, 96, 108
                    if (r == 12 || r == 18 || r == 24 || r == 36 ||
                            r == 48 || r == 72 || r == 96 || r == 108) {
                        has_ofdm_rates = true;
                    }
                }
                break;

            case 45: // HT Capabilities (11n)
            case 61: // HT Operation (11n)
                has_ht = true;
                break;

            case 191: // VHT Capabilities (11ac)
            case 192: // VHT Operation (11ac)
                has_vht = true;
                break;

            case 221: // Vendor specific IEs
                if (len >= 4) {
                    // WPA: 00:50:F2:01
                    if (data[0] == 0x00 && data[1] == 0x50 && data[2] == 0xF2 && data[3] == 0x01) {
                        has_wpa = true;
                        beacon.wpa = true;
                    }
                    // WPS: 00:50:F2:04
                    if (data[0] == 0x00 && data[1] == 0x50 && data[2] == 0xF2 && data[3] == 0x04) {
                        beacon.wps = true;
                    }
                }
                break;

            case 255: // HE Capabilities (11ax)
                if (len >= 1) {
                    uint8_t ext_id = data[0];

                    // HE Capabilities / HE Operation
                    if (ext_id == 35 || ext_id == 36) {
                        has_he = true;
                    }
                }
                break;
        }
        
        // Iterate pointer and remainder is less
        ie += len + 2;
        rem -= len + 2;
    }
    
    // Calculate network frequency
    int freq_mhz = 0;
    if (channel >= 1  && channel < 14) {
        freq_mhz = 2412 + 5 * (channel - 1);
    } else if (channel == 14) {
        freq_mhz = 2484; // Special case
    } else if (channel >= 36 && channel <= 165) {
        freq_mhz = 5000 + (5 * channel);
    }

    // Decide Wi-Fi PHY type
    beacon.he = has_he;
    beacon.vht = has_vht;
    beacon.ht = has_ht;
    if (has_he) {
        beacon.phy = WIFI_PHY_11AX;
    } else if (has_vht) {
        beacon.phy = WIFI_PHY_11AC;
    } else if (has_ht) {
        beacon.phy = WIFI_PHY_11N;
    } else if (freq_mhz >= 4900) {
        beacon.phy = WIFI_PHY_11A; // Legacy 5 GHz
    } else if (has_ofdm_rates) {
        beacon.phy = WIFI_PHY_11G; // Legacy 2.4 GHz OFDM
    } else if (has_cck_rates) {
        beacon.phy = WIFI_PHY_11B; // Legacy 2.4 GHz CCK
    } else {
        beacon.phy = WIFI_PHY_UNKNOWN;
    }
    
    // Get SNR
    int snr_db = pkt->rx_ctrl.rssi - pkt->rx_ctrl.noise_floor;
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "SSID: %s | Channel: %d (%d MHz) | RSSI: %d dBm | SNR: %d dB | Encryption: %s | TS=%llu days | intvl=%u ms | cap=0x%04x",
            ssid, channel, freq_mhz, pkt->rx_ctrl.rssi, snr_db,
            has_rsn ? "WPA2/3" : has_wpa ? "WPA" : (cap_info & 0x10) ? "WEP" : "Open",
            timestamp_days, interval, cap_info);
#endif
    
    // Populate and send
    strlcpy((char*)beacon.ssid, ssid, sizeof(beacon.ssid));
    beacon.channel = channel;
    beacon.freq = freq_mhz;
    beacon.rssi = pkt->rx_ctrl.rssi;
    beacon.snr = snr_db;
    beacon.rsn = has_rsn;
    beacon.wpa = has_wpa;
    beacon.cap_info = cap_info;
    beacon.interval = interval;
    beacon.timestamp = timestamp_seconds;
    if (xQueueSend(xWifiBeaconQueue, &beacon, 0) != pdTRUE) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "xWifiBeaconQueue send failed");
#endif
    }
    
    /*
    Capability Information (cap_info): A 16-bit bitmask of the AP's capabilities, defined by IEEE 802.11
    Example: cap=0x1431 -> binary 0001 0100 0011 0001
    
    | Bit | Name                | Value  | Set? | Meaning                                 |
    | --- | ------------------- | ------ | ---- | --------------------------------------- |
    | 0   | ESS                 | 1      | 1    | Infrastructure network (not IBSS).      |
    | 1   | IBSS                | 2      | 0    | Ad hoc mode (not set).                  |
    | 2   | CF‐Pollable         | 4      | 0    | Contention‐free polling (not set).      |
    | 3   | CF‐PollRequest      | 8      | 0    | Contention‐free request (not set).      |
    | 4   | Privacy             | 0x10   | 1    | WEP/WPA/WPA2 encryption supported.      |
    | 5   | Short Preamble      | 0x20   | 1    | Supports 'short' preamble (faster RX).  |
    | 6   | PBCC                | 0x40   | 0    | Packet Binary Convolutional Code (no).  |
    | 7   | Channel Agility     | 0x80   | 0    | Dynamic channel switching (no).         |
    | 8   | Spectrum Management | 0x100  | 0    | 5 GHz regulatory features (no).         |
    | 9   | QoS AP              | 0x200  | 0    | Quality‐of‐Service AP (no).             |
    | 10  | Short Slot Time     | 0x400  | 1    | 9 µs slot instead of 20 µs.             |
    | 11  | APSD                | 0x800  | 0    | Automatic Power‐Save Delivery (no).     |
    | 12  | Radio Measurement   | 0x1000 | 1    | 802.11k measurement (survey) supported. |
    | 13  | DSSS‐OFDM           | 0x2000 | 0    | Mixed‐mode DSSS/OFDM (no).              |
    | 14  | Delayed Block Ack   | 0x4000 | 0    | (802.11e feature) (no).                 |
    | 15  | Immediate Block Ack | 0x8000 | 0    | (802.11e feature) (no).                 |
    
    So 0x1431 tells you your AP is:
        - ESS (infrastructure AP, not ad-hoc)
        - Privacy (it's encrypting traffic)
        - Short Preamble (can use faster preamble)
        - Short Slot Time (9 µs slots for faster contention)
        - Radio Measurement (it supports 802.11k measurement features)
    */
}

static void record_client(const uint8_t *mac, int8_t rssi)
{
    // Check if exists
    for (int i = 0; i < wifi_data.client_count; ++i) {
        // If found existing
        if (memcmp(wifi_data.clients[i].mac, mac, 6) == 0) {
            // Update to latest RSSI
            wifi_data.clients[i].rssi = rssi;

            // Increment packet count up to max
            if (wifi_data.clients[i].pkt_count != UINT32_MAX) {
                wifi_data.clients[i].pkt_count++;
            }
            return;
        }
    }

    // Else add new
    if (wifi_data.client_count < MAX_MAC_CLIENTS) {
        memcpy(wifi_data.clients[wifi_data.client_count].mac, mac, 6);

        wifi_data.clients[wifi_data.client_count].rssi = rssi;
        wifi_data.clients[wifi_data.client_count].pkt_count = 1;

        wifi_data.client_count++;

#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "New MAC found: %02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
#endif
    }
}

static void wifi_sniffer_data_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    // Make sure a data packet
    if (type != WIFI_PKT_DATA) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t *frame = pkt->payload;
    // Get frame type
    uint16_t fc = (frame[1] << 8) | frame[0];

    // Double check: only true 802.11 data
    if ((fc & 0x0C) != 0x08) {
        return; // Bits [3:2] == 10
    }

    bool toDS = fc & 0x0100;
    bool fromDS = fc & 0x0200;
    const uint8_t *sa = NULL; // Source MAC pointer

    // Address layout depends on ToDS/FromDS:
    // [0] Frame Ctrl (2)
    // [2] Duration (2)
    // [4] Addr1
    // [10] Addr2
    // [16] Addr3
    // [24] Addr4 (only when both ToDS & FromDS)

    // Find MAC
    if(!toDS && !fromDS) {
        // STA to STA
        sa = &frame[10]; // Addr2
    } else if (!toDS && fromDS) {
        // From DS: AP -> STA
        sa = &frame[16]; // Addr3 is transmitter (the AP's MAC)
    } else if (toDS && !fromDS) {
        // To DS: STA -> AP
        sa = &frame[10]; // Addr2 is station's MAC
    } else {
        // WDS: mesh or 4-addr; Addr4 is original source
        sa = &frame[24];
    }

    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // Check uniqueness
    record_client(sa, rssi);

    wifi_data.rate = pkt->rx_ctrl.rate;
    wifi_data.channel = pkt->rx_ctrl.channel;
    wifi_data_t *p = &wifi_data;
    if (xQueueSend(xWifiDataQueue, &p, 0) != pdTRUE) {
        //ESP_LOGE(TAG, "xWifiDataQueue send failed");
    }
}

// Append bytes as hex string to buf (e.g., "DE AD BE EF ")
static bool append_hex(const uint8_t *bytes, size_t len, char *buf, size_t *cur_len, size_t cap)
{
    // Ensure all valid
    if (!bytes || !buf || !cur_len) {
        return false;
    }

    // Loop through buf len
    for (size_t i = 0; i < len; ++i) {
        if (*cur_len + 3 >= cap) {
            return false; // Space for "XX "
        }

        // Append hex
        int n = snprintf(buf + *cur_len, cap - *cur_len, "%02X ", bytes[i]);

        // Check snprintf success
        if (n < 0) {
            return false;
        }

        // Move forward
        *cur_len += n;
    }
    return true;
}

static void wifi_sniffer_raw_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    // Promiscuous RX callback runs in the Wi-Fi RX path.
    // Do not block here (ever), or the driver can stall.
    if (xSemaphoreTake(xWifiRawFramesMutex, 0) != pdTRUE) {
        return;
    }

    if (raw_frames_captured >= WIFI_MAX_RAW_FRAMES) {
        xSemaphoreGive(xWifiRawFramesMutex); // Release raw sniffed frames
        return; // Stop early if limit hit
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *frame = pkt->payload;
    size_t frame_len = pkt->rx_ctrl.sig_len;

    // Optional: Filter by type (e.g., skip if not management/data)
    //if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    // Append to global hex buffer: "Frame N: [hex] (len=L, rssi=R, chan=C)\n\n"
    size_t start = raw_frames_hex_len;
    uint32_t next = raw_frames_captured + 1;
    bool ok = false;
    int n;

    do {
        n = snprintf(raw_frames_hex_buf + raw_frames_hex_len,
                RAW_HEX_BUF_CAP - raw_frames_hex_len,
                "Frame %u: ",
                (unsigned)next);

        if (n <= 0 || raw_frames_hex_len + (size_t)n >= RAW_HEX_BUF_CAP) {
            break;
        }
        raw_frames_hex_len += (size_t)n;

        // Append hex bytes
        if (!append_hex(frame, frame_len, raw_frames_hex_buf, &raw_frames_hex_len, RAW_HEX_BUF_CAP)) {
            break;
        }

        // Append metadata
        n = snprintf(raw_frames_hex_buf + raw_frames_hex_len,
                RAW_HEX_BUF_CAP - raw_frames_hex_len,
                "(len=%u, rssi=%d, chan=%d)\n\n",
                (unsigned)frame_len, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
        
        if (n <= 0 || raw_frames_hex_len + (size_t)n >= RAW_HEX_BUF_CAP) {
            break;
        }
        raw_frames_hex_len += (size_t)n;

        raw_frames_captured = next;
        ok = true;
    } while (0);

    if (!ok) {
        // Roll back partial writes so buffer stays consistent
        raw_frames_hex_len = start;
        if (start < RAW_HEX_BUF_CAP) {
            raw_frames_hex_buf[start] = '\0';
        }
        xSemaphoreGive(xWifiRawFramesMutex); // Release raw sniffed frames
        return;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Captured frame %u (%u bytes)", (unsigned)raw_frames_captured, (unsigned)frame_len);
#endif

    xSemaphoreGive(xWifiRawFramesMutex); // Release raw sniffed frames
    return;
}

void wifi_utils_init_promiscuous(wifi_sniff_t *network)
{
    // Start up the radio
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());

    // Fix the channel to the target AP: set channel (0 = auto/current)
    ESP_ERROR_CHECK(esp_wifi_set_channel(network->channel, WIFI_SECOND_CHAN_NONE));

    // Only selected frame(s)
    wifi_promiscuous_filter_t filter = {
        .filter_mask = network->mask
    };

    // target_bssid not checked unless beacon_cb
    memcpy(target_bssid, network->target_bssid, 6);
    
    // Filter for matching packets
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));

    // Register callback and enable promiscuous mode
    if (network->mask == WIFI_PROMIS_FILTER_MASK_MGMT) {
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_beacon_cb)); // Sniff beacon frames
    } else if (network->mask == WIFI_PROMIS_FILTER_MASK_DATA) {
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_data_cb)); // Sniff data frames
        memset(&wifi_data, 0, sizeof(wifi_data)); // Reset counts so chart reflects this session
    } else if (network->mask == WIFI_PROMIS_FILTER_MASK_RAW_USEFUL) {
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_raw_cb)); // Sniff everything
    } else {
        ESP_LOGE(TAG, "wifi_utils_init_promiscuous: unknown filter mask: %d", network->mask);
    }
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    // If sniffing a target network
    if (network->mask != WIFI_PROMIS_FILTER_MASK_RAW_USEFUL) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Sniffer initialized; filtering beacon frames from %02x:%02x:%02x:%02x:%02x:%02x",
                network->target_bssid[0], network->target_bssid[1],
                network->target_bssid[2], network->target_bssid[3],
                network->target_bssid[4], network->target_bssid[5]);
#endif
    } else { // Else the kitchen sink
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Sniffer initialized; sniffing the kitchen sink.");
#endif
    }
}