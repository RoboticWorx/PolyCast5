/*

This is a tool to help humans remember things! NOT digital memory.

This file is used for the SRS (spaced repetition system) memory option on PolyCast5.

This allows PolyCast5 to serve as a memory assistant (neurologically) when the user is
trying to memorize new things. This is based on the Ebbinghaus forgetting curve, in which
learned materials is better remembered by reviewing them at increasing intervals for ideal
LTP of synapses between neurons in the brain.

Please see https://polycast5.com/blogs/docs/srs-memory-planner

*/

#include "freertos/projdefs.h"
#include "lcd_utils.h"
#include "misc/lv_types.h"
#include "polycast5_macros.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>

#include "freertos/idf_additions.h"

#include "nvs.h"
#include "esp_log.h"

#include "wifi_utils.h"
#include "wifi_task.h"
#include "gpio_task.h"
#include "srs_memory.h"

#define TAG "SRS_MEMORY"

// Namespace and keys
#define SRS_FORMAT "e%04u"
#define SRS_CNT_KEY "cnt" // Number of entries key
#define SRS_LAST_PAGE_KEY "last" // Last page used (for auto-increment)

// SRS intervals: 1d > 3d > 7d > 14d > 1m > 3m > 6m > 12m
const uint16_t srs_days[] = {1, 3, 7, 14, 30, 90, 180, 365};

// Use PSRAM instead
POLYCAST5_USE_PSRAM srs_entry_t srs_tbl[SRS_MAX_ENTRIES];
POLYCAST5_USE_PSRAM static int srs_tmp_idx[SRS_MAX_ENTRIES];

static uint16_t srs_cnt = 0;
static uint16_t srs_last_page = 0;

/* --------------- Helpers --------------- */

// Check if RTC synced
static bool rtc_synced(void)
{
    // Treat anything before 2025 as "unsynced"
    struct tm t; 
    time_t now = time(NULL);

    // If unsynced
    if (now <= 0) {
        return false;
    }
    
    // Else synced
    localtime_r(&now, &t);
    return (t.tm_year >= 125); // Years since 1900
}

static int srs_find_by_page(uint16_t page)
{
    // Check if page exists
    for (int i = 0; i < srs_cnt; ++i) {
        // If so return that page
        if (srs_tbl[i].page == page) {
            return i;
        }
    }
    
    // Else -1
    return -1;
}

// Check if a page is due
static bool srs_is_due(const srs_entry_t *e, uint32_t today)
{
    // Due if today is later than or on the day added + current step interval
    uint16_t interval = srs_days[e->step];
    return (today >= e->start_day + interval);
}

/* --------------- Core --------------- */

bool srs_sync_time_over_wifi(void)
{
    // Check if RTC synced
    if (rtc_synced()) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Time already synced");
        #endif
        return true; // No need for Wi-Fi if already synced
    }

    LCD_LOADING_ANIM_START_DEFAULT();

    // Confirmation text
    lv_obj_t *lbl_info = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_info, "Getting day via Wi-Fi...", user_secondary_color,
            &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 0);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow render

    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Getting day via Wi-Fi");
    #endif

    // If Wi-Fi already connected
    if (xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Wi-Fi already connected");
        #endif

        // Request to get date and time
        xEventGroupSetBits(xWifiEventGroup, WIFI_GET_DATE_TIME_BIT);

        // Wait for it to complete
        while (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_GOT_DATE_TIME_BIT)) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        // Reset for next time
        xEventGroupClearBits(xWifiEventGroup, WIFI_GOT_DATE_TIME_BIT);

        // Delete helper text
        lv_obj_delete(lbl_info);
        lbl_info = NULL;

        lcd_loading_anim_stop();
    
        return true;
    }

    /* If not synced */
    // Ask Wi-Fi to reconnect to the last used network
    wifi_login_t selected_network = wifi_utils_get_prev(); // Loads boot state saved network info
    selected_network.prev = true; // Connecting to previous
    if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(TAG, "Failed srs_sync_time_over_wifi: xWifiSelectedNetworkQueue");
    }

    // Wait up to WIFI_CONN_TIMEOUT_MS for connection
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(WIFI_CONN_TIMEOUT_MS);
    bool connected = false;

    while ((xTaskGetTickCount() - start) < timeout) {
        // Connected
        if ((xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT) != 0) {
            connected = true;
            break;
        }

        // User cancelled
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdPASS) {
            // Stop loading animation
            lcd_loading_anim_stop();

            // Delete helper text
            lv_obj_delete(lbl_info);
            lbl_info = NULL;

            lcd_clear_pending_inputs = true; // Clear user inputs from wait

            xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT); // Disconnect Wi-Fi

            return false;
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    lcd_clear_pending_inputs = true; // Clear user inputs from wait

    if (connected) {
        // Request to get date and time
        xEventGroupSetBits(xWifiEventGroup, WIFI_GET_DATE_TIME_BIT);
        
        // Wait for it to complete
        while (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_GOT_DATE_TIME_BIT)) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        // Reset for next time
        xEventGroupClearBits(xWifiEventGroup, WIFI_GOT_DATE_TIME_BIT);

        // Done with Wi-Fi -> disconnect to save power
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

        lcd_clear_pending_inputs = true; // Clear user inputs from wait
    } else {
        // Stop loading animation
        lcd_loading_anim_stop();

        // Notify user
        lcd_format_label(lbl_info, "Connection failed!\nPlease connect to your\nWi-Fi network at least\nonce in the 'Wi-Fi'\nmenu and make sure\nyou are in range.", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 0);

        // Show
        lv_timer_handler();
        
        // Wait for left button press
        while (xSemaphoreTake(xLeftButtonSemaphore, 0) != pdPASS) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        lcd_clear_pending_inputs = true; // Clear user inputs from wait

        // Delete helper text
        lv_obj_delete(lbl_info);
        lbl_info = NULL;

        return false;
    }

    // Stop loading animation
    lcd_loading_anim_stop();

    // Delete helper text
    lv_obj_delete(lbl_info);
    lbl_info = NULL;
    return true;
}

// Auto-increment page
uint16_t srs_next_default_page(void)
{
    // Prefer persisted rolling number; backstop by scanning table
    uint16_t best = srs_last_page;
    
    // If first boot, find highest
    if (best == 0) {
        for (int i = 0; i < srs_cnt; ++i) {
            if (srs_tbl[i].page > best) {
                best = srs_tbl[i].page;
            }
        }
    }
    
    return (best + 1);
}

void srs_add_or_reset(uint16_t page, uint32_t today)
{
    int idx = srs_find_by_page(page);
    
    // If the page exists, restart its schedule from today
    if (idx >= 0) {
        // Reset review cycle from today
        srs_tbl[idx].step = 0;
        srs_tbl[idx].start_day = today;
    } else if (srs_cnt < SRS_MAX_ENTRIES) { // Else if there's room, append a new entry
        srs_tbl[srs_cnt].page = page;
        srs_tbl[srs_cnt].step = 0;
        srs_tbl[srs_cnt].start_day = today;
        srs_cnt++;
    }
    
    // Update if needed
    if (page > srs_last_page) {
        srs_last_page = page;
    }
    
    // Persist to NVS
    srs_nvs_save();
}

void srs_mark_reviewed_index(int idx, uint32_t today)
{
    // Bounds check
    if (idx < 0 || idx >= srs_cnt) {
        return;
    }
    
    // Increment step
    if (srs_tbl[idx].step < (SRS_NUM_STEPS - 1)) {
        srs_tbl[idx].step++;
    }
    
    // Persist to NVS
    srs_nvs_save();
}

// Returns number of due entries; fills up to max_out indices of due order
int srs_build_due_list(int *out_idx, int max_out, uint32_t today)
{
    const int cap = (int)(sizeof(srs_tmp_idx) / sizeof(srs_tmp_idx[0]));

    int collected = 0; // How many we stored in srs_tmp_idx
    int total_due = 0; // True number due

    // Collect due indices up to temp buffer capacity
    for (int i = 0; i < srs_cnt; ++i) {
        // If due
        if (srs_is_due(&srs_tbl[i], today)) {
            // And less than limit
            if (collected < cap) {
                // Append
                srs_tmp_idx[collected++] = i;
            }
            total_due++; // Regardless
        }
    }

    // Sort collected by most overdue using absolute schedule
    // overdue = today - (start_day + interval(step))
    for (int i = 0; i + 1 < collected; ++i) {
        for (int j = i + 1; j < collected; ++j) {
            const srs_entry_t *Ei = &srs_tbl[srs_tmp_idx[i]];
            const srs_entry_t *Ej = &srs_tbl[srs_tmp_idx[j]];

            int need_i = (int)srs_days[Ei->step];
            int need_j = (int)srs_days[Ej->step];

            int overdue_i = (int)today - (int)(Ei->start_day + (uint32_t)need_i);
            int overdue_j = (int)today - (int)(Ej->start_day + (uint32_t)need_j);

            // Swap handler
            bool swap = false;
            if (overdue_j > overdue_i) {
                swap = true; // More overdue first
            } else if (overdue_j == overdue_i) { // Tie-breaker
                if (Ej->start_day < Ei->start_day) {
                    swap = true;
                } else if (Ej->start_day == Ei->start_day && Ej->page < Ei->page) {
                    swap = true;
                }
            }

            // Swap places
            if (swap) {
                int t = srs_tmp_idx[i];
                srs_tmp_idx[i] = srs_tmp_idx[j];
                srs_tmp_idx[j] = t;
            }
        }
    }

    // Emit up to max_out into caller's buffer
    int out = (collected < max_out) ? collected : max_out;
    for (int k = 0; k < out; ++k) {
        out_idx[k] = srs_tmp_idx[k];
    }

    return total_due;
}

// Converts Unix seconds to whole days
uint32_t srs_days_since_epoch_local(int calibrate)
{
    tzset(); // Redundant

    time_t now = time(NULL);

    if (now <= 0) {
        return 0;
    }

    struct tm lt;
    localtime_r(&now, &lt); // Uses TZ/DST that was set with tzset()
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    lt.tm_isdst = -1; // Let mktime() decide DST

    time_t local_midnight_epoch = mktime(&lt);

    #ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "0-based srs_days_since_epoch now = %" PRId64 "s", (int64_t)local_midnight_epoch); // Seconds
    ESP_LOGI(TAG, "0-based srs_days_since_epoch now = %" PRIu32 "d", (uint32_t)(local_midnight_epoch / 86400)); // Days
    #endif

    // Round down by 86400 -> today index
    return (uint32_t)((local_midnight_epoch / 86400) + calibrate); // 0-based
}

void srs_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    if (nvs_open(SRS_NS, NVS_READONLY, &h) != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "srs_nvs_load nvs_open failed");
        #endif

        // Set defaults
        srs_cnt = 0;
        srs_last_page = 0;
        return;
    }

    // Get number of entries
    uint16_t cnt = 0;
    esp_err_t err = nvs_get_u16(h, SRS_CNT_KEY, &cnt);
    if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "srs_nvs_load nvs_get_u16 CNT failed");
        #endif
    }

    srs_cnt = 0;

    // Get last used page (for +1 add)
    uint16_t lastp = 0;
    err = nvs_get_u16(h, SRS_LAST_PAGE_KEY, &lastp);
    if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "srs_nvs_load nvs_get_u16 PAGE failed");
        #endif
    }

    srs_last_page = lastp;

    // Get all entries
    for (uint16_t i = 0; i < cnt && i < SRS_MAX_ENTRIES; ++i) {
        // Format key
        char key[7];
        snprintf(key, sizeof(key), SRS_FORMAT, i); // e.g. "e0001"
        
        // Get srs_entry_t blob
        size_t len = sizeof(srs_entry_t);
        srs_entry_t tmp;
        if (nvs_get_blob(h, key, &tmp, &len) == ESP_OK && len == sizeof(srs_entry_t)) {
            // If good srs_cnt++ into srs_tbl
            srs_tbl[srs_cnt++] = tmp;
        } else {
            #ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "srs_nvs_load nvs_get_blob failed at i=%d", i);
            #endif
        }
    }
    
    // Close NVS
    nvs_close(h);
}

void srs_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    if (nvs_open(SRS_NS, NVS_READWRITE, &h) != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "srs_nvs_save nvs_open failed");
        #endif
        return;
    }

    // Set count and last used page
    esp_err_t err = nvs_set_u16(h, SRS_CNT_KEY, srs_cnt);
    if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "srs_nvs_load nvs_set_u16 CNT failed");
        #endif
    }

    err = nvs_set_u16(h, SRS_LAST_PAGE_KEY, srs_last_page);
    if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "srs_nvs_load nvs_set_u16 PAGE failed");
        #endif
    }

    // Save each entry
    for (uint16_t i = 0; i < srs_cnt; ++i) {
        // Format key
        char key[7];
        snprintf(key, sizeof(key), SRS_FORMAT, i);
        
        // Save srs_entry_t blob
        err = nvs_set_blob(h, key, &srs_tbl[i], sizeof(srs_entry_t));
        if (err != ESP_OK) {
            #ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "srs_nvs_load nvs_set_blob failed");
            #endif
        }
    }
    
    // Commit changes
    nvs_commit(h);
    
    // Close NVS
    nvs_close(h);
}
