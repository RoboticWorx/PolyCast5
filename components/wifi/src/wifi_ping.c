#include "polycast5_macros.h"

#include <string.h>
#include <string.h>
#include <stdlib.h>

#include <ping/ping_sock.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_ping.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "wifi_utils.h"
#include "wifi_task.h"

#define TAG "WIFI_PING"

esp_ip4_addr_t sta_gw = {0};
bool sta_gw_valid = false;

static volatile int32_t ping_avg_ms = -1;

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t total_time = 0;

    // Get ping profile stats
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_time, sizeof(total_time));

    // Calculate average RTT in ms
    if (received > 0) {
        ping_avg_ms = (int32_t)(total_time / received);
    } else {
        ping_avg_ms = -1;
    }

    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "ping end: tx=%"PRIu32" rx=%"PRIu32" avg=%"PRIu32" ms",
            transmitted, received, (uint32_t)(ping_avg_ms < 0 ? 0 : ping_avg_ms));
    #endif
}

// Generic ping IPv4 helper
static esp_err_t wifi_ping_ip4(const esp_ip4_addr_t *ip4, int32_t *rtt_ms)
{
    // Error check
    if (!ip4 || !rtt_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    // Prepare target
    ip_addr_t target = {0};
    target.type = IPADDR_TYPE_V4;

    // Copy raw IPv4
    ip4_addr_set_u32(ip_2_ip4(&target), ip4->addr);

    // Configure ping settings
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = 4;
    cfg.interval_ms = 1000;
    cfg.timeout_ms = 1000;

    // Set callbacks
    esp_ping_callbacks_t cbs = {
        .on_ping_success = NULL,
        .on_ping_timeout = NULL,
        .on_ping_end = ping_on_end,
    };

    // Create a new ping session
    esp_ping_handle_t ping;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &ping);
    if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_ping_new_session failed: %s", esp_err_to_name(err));
        #endif
        return err;
    }

    ping_avg_ms = -1;

    // Start pinging
    err = esp_ping_start(ping);
    if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_ping_start failed: %s", esp_err_to_name(err));
        #endif
        esp_ping_delete_session(ping);
        return err;
    }

    // Wait until done or timeout
    const uint32_t max_wait_ms = cfg.count * cfg.interval_ms + 2000;
    uint32_t waited = 0;

    while (ping_avg_ms == -1 && waited < max_wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }

    // Stop and delete the ping session
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);

    if (ping_avg_ms < 0) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "No replies from target");
        #endif
        return ESP_ERR_TIMEOUT;
    }

    *rtt_ms = ping_avg_ms;
    return ESP_OK;
}

esp_err_t wifi_ping_gateway(int32_t *rtt_ms)
{
    // Error check
    if (!sta_gw_valid) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Gateway unknown; not connected yet?");
        #endif
        return ESP_FAIL;
    }

    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Pinging gateway: " IPSTR, IP2STR(&sta_gw));
    #endif

    return wifi_ping_ip4(&sta_gw, rtt_ms);
}

esp_err_t wifi_ping_dns(const char *host, int32_t *rtt_ms)
{
    // Error check
    if (!host || !rtt_ms) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_ip4_addr_t ip4 = {0};

    // Try numeric IPv4
    in_addr_t addr = inet_addr(host); // lwIP's inet_addr()
    if (addr != IPADDR_NONE) {
        // Note: IPADDR_NONE is 0xFFFFFFFF -> this fails only for 255.255.255.255
        ip4.addr = addr;

        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Pinging numeric host %s", host);
        #endif

        return wifi_ping_ip4(&ip4, rtt_ms);
    }

    // Fallback: DNS resolve the hostname
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET; // IPv4 only
    hints.ai_socktype = SOCK_STREAM; // TCP-based host, any port

    // Resolve the hostname
    struct addrinfo *res = NULL;
    int err = getaddrinfo(host, NULL, &hints, &res);
    if (err != 0 || res == NULL) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "getaddrinfo failed for '%s': %d", host, err);
        #endif

        if (res) {
            freeaddrinfo(res);
        }
        return ESP_FAIL;
    }

    // Extract IPv4 address
    struct sockaddr_in *addr4 = (struct sockaddr_in *)res->ai_addr;
    ip4.addr = addr4->sin_addr.s_addr;

    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Pinging host %s -> %s", host, inet_ntoa(addr4->sin_addr));
    #endif

    freeaddrinfo(res);

    return wifi_ping_ip4(&ip4, rtt_ms);
}