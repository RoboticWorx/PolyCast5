#ifndef WIFI_CLAUDE_H
#define WIFI_CLAUDE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/idf_additions.h"

#define WIFI_CLAUDE_URL_MAX_LEN 128

// 'Not used yet' sentinel for the pct fields
#define WIFI_CLAUDE_PCT_UNUSED 255U

typedef struct {
    uint8_t  session_pct; // 0..100, or WIFI_CLAUDE_PCT_UNUSED
    uint32_t session_reset_secs; // Seconds until reset

    uint8_t  weekly_all_pct;
    uint32_t weekly_all_reset_secs;

    uint8_t  weekly_sonnet_pct;
    uint32_t weekly_sonnet_reset_secs;

    uint8_t  claude_design_pct;
    uint32_t claude_design_reset_secs;

    int64_t  fetched_at_us; // esp_timer_get_time() at fetch
    bool     ok; // false => last fetch failed
} claude_usage_t;

extern QueueHandle_t xWifiClaudeUsageQueue;           // Length 1, peek-friendly
extern SemaphoreHandle_t xWifiClaudeRefreshSemaphore; // Manual refresh trigger

/**
 * @brief Create the Claude usage queue/semaphore (call once at wifi_task startup).
 */
void wifi_claude_init(void);

/**
 * @brief Called from the wifi_task main loop. Throttles to ~60s, fires a
 *        blocking HTTPS GET against the companion host when Wi-Fi is up,
 *        parses JSON, overwrites xWifiClaudeUsageQueue.
 */
void wifi_claude_tick(void);

/**
 * @brief Check if Claude info is configured by verifying if a non-empty host URL is set.
 * 
 * @returns True if the user has completed setup (we have a URL to fetch from).
 */
bool wifi_claude_is_configured(void);

/**
 * @brief Save companion host URL to NVS namespace.
 * 
 * @returns ESP error status
 */
esp_err_t wifi_claude_save_config_nvs(const char *url);

/**
 * @brief Load companion host URL from NVS.
 * 
 * @returns ESP error status
 */
esp_err_t wifi_claude_load_config_nvs(char *url_out, size_t url_sz);

#endif // WIFI_CLAUDE_H
