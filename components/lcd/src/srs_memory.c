/*

This is a tool to help humans remember things! NOT digital memory.

This file is used for the SRS (spaced repetition system) memory option on PolyCast5.

This allows PolyCast5 to serve as a memory assistant (neurologically) when the user is
trying to memorize new things. This is based on the Ebbinghaus forgetting curve, in which
learned materials is better remembered by reviewing them at increasing intervals for ideal
LTP of synapses between neurons in the brain.

Please see https://polycast5.com/blogs/docs/srs-memory-planner

*/

#include "freertos/FreeRTOS.h"
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
POLYCAST5_USE_PSRAM_BSS srs_entry_t srs_tbl[SRS_MAX_ENTRIES];
POLYCAST5_USE_PSRAM_BSS static int srs_tmp_idx[SRS_MAX_ENTRIES];

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
    // Bounds check against corrupted NVS data
    if (e->step >= SRS_NUM_STEPS) {
        ESP_LOGE(TAG, "Invalid step %u for page %u", e->step, e->page);
        return false;
    }

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

        lcd_anim_loading_stop();
    
        return true;
    }

    /* If not synced */
    xEventGroupSetBits(xWifiEventGroup, WIFI_RECONNECT_BIT); // Reconnect to previous Wi-Fi network

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
            lcd_anim_loading_stop();

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
        lcd_anim_loading_stop();

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
    lcd_anim_loading_stop();

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
uint32_t srs_days_since_epoch_local(void)
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
    return (uint32_t)(local_midnight_epoch / 86400); // 0-based
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

#ifdef POLYCAST5_SRS_CALIBRATING
/* ========== Calibration Functions ========== */

// Hardcoded list of pages with their dates to autofill previous entries on a new PC5 notebook
const srs_calibration_entry_t srs_calibration_data[] = {
    // Example: {1, "09/12/2025"} means page 1 was added on September 12, 2025
    {1, "09/12/2025"},
    {2, "09/13/2025"},
    {3, "09/14/2025"},
    {4, "09/15/2025"},
    {5, "09/16/2025"},
    {6, "09/17/2025"},
    {7, "09/18/2025"},
    {8, "09/19/2025"},
    {9, "09/20/2025"},
    {10, "09/21/2025"},
    {11, "09/22/2025"},
    {12, "09/24/2025"},
    {13, "09/25/2025"},
    {14, "09/26/2025"},
    {15, "09/27/2025"},
    {16, "09/28/2025"},
    {17, "09/29/2025"},
    {18, "09/30/2025"},
    {19, "10/02/2025"},
    {20, "10/04/2025"},
    {21, "10/05/2025"},
    {22, "10/06/2025"},
    {23, "10/08/2025"},
    {24, "10/10/2025"},
    {25, "10/11/2025"},
    {26, "10/12/2025"},
    {27, "10/13/2025"},
    {28, "10/15/2025"},
    {29, "10/16/2025"},
    {30, "10/17/2025"},
    {31, "10/18/2025"},
    {32, "10/20/2025"},
    {33, "10/21/2025"},
    {34, "10/26/2025"},
    {35, "10/27/2025"},
    {36, "10/28/2025"},
    {37, "10/29/2025"},
    {38, "10/30/2025"},
    {39, "11/01/2025"},
    {40, "11/02/2025"},
    {41, "11/03/2025"},
    {42, "11/04/2025"},
    {43, "11/05/2025"},
    {44, "11/06/2025"},
    {45, "11/07/2025"},
    {46, "11/08/2025"},
    {47, "11/09/2025"},
    {48, "11/10/2025"},
    {49, "11/11/2025"},
    {50, "11/13/2025"},
    {51, "11/14/2025"},
    {52, "11/15/2025"},
    {53, "11/18/2025"},
    {54, "11/19/2025"},
    {55, "11/20/2025"},
    {56, "11/22/2025"},
    {57, "11/23/2025"},
    {58, "11/24/2025"},
    {59, "11/25/2025"},
    {60, "11/30/2025"},
    {61, "12/01/2025"},
    {62, "12/02/2025"},
    {63, "12/03/2025"},
    {64, "12/04/2025"},
    {65, "12/06/2025"},
    {66, "12/10/2025"},
    {67, "12/11/2025"},
    {68, "12/12/2025"},
    {69, "12/13/2025"},
    {70, "12/14/2025"},
    {71, "12/15/2025"},
    {72, "12/16/2025"},
    {73, "12/18/2025"},
    {74, "12/21/2025"},
    {75, "12/22/2025"},
    {76, "12/23/2025"},
    {77, "12/27/2025"},
    {78, "12/28/2025"},
    {79, "12/29/2025"},
    {80, "12/30/2025"},
    {81, "01/06/2026"},
    {82, "01/07/2026"},
    {83, "01/08/2026"},
    {84, "01/11/2026"},
    {85, "01/13/2026"},
    {86, "01/14/2026"},
    {87, "01/15/2026"},
    {88, "01/17/2026"},
    {89, "01/18/2026"},
    {90, "01/19/2026"},
    {91, "01/20/2026"},
    {92, "01/21/2026"},
    {93, "01/22/2026"},
    {94, "01/25/2026"},
    {95, "01/26/2026"},
    {96, "01/27/2026"},
    {97, "01/28/2026"},
    {98, "01/30/2026"},
    {99, "01/31/2026"},
    {100, "02/01/2026"},
    {101, "02/02/2026"},
    {102, "02/03/2026"},
    {103, "02/06/2026"},
};

const int srs_calibration_count = sizeof(srs_calibration_data) / sizeof(srs_calibration_data[0]);

// Helper to convert "MM/DD/YYYY" to days since epoch
static uint32_t parse_date_to_days(const char *date_str)
{
    int month, day, year;
    if (sscanf(date_str, "%d/%d/%d", &month, &day, &year) != 3) {
        ESP_LOGE(TAG, "Invalid date format: %s", date_str);
        return 0;
    }
    
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 0;
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    
    time_t epoch = mktime(&t);
    if (epoch <= 0) {
        ESP_LOGE(TAG, "Failed to convert date: %s", date_str);
        return 0;
    }
    
    return (uint32_t)(epoch / 86400); // Convert to days
}

void srs_batch_load_from_dates(const srs_calibration_entry_t *entries, int count)
{
    ESP_LOGI(TAG, "Processing %d calibration entries...", count);
    
    // Get today's date for step calculation
    uint32_t today = srs_days_since_epoch_local();
    
    for (int i = 0; i < count; ++i) {
        uint16_t page = entries[i].page;
        uint32_t start_day = parse_date_to_days(entries[i].date);
        
        if (start_day > 0) {
            // Calculate days since this page was added
            int32_t days_since = (int32_t)(today - start_day);
            
            // Determine step based on next scheduled interval
            uint8_t step = 0;
            if (days_since > 0) {
                while (step + 1 < SRS_NUM_STEPS && (uint32_t)days_since > srs_days[step]) {
                    step++;
                }
            }

            // Find if page already exists
            int idx = srs_find_by_page(page);
            if (idx >= 0) {
                // Update existing
                srs_tbl[idx].start_day = start_day;
                srs_tbl[idx].step = step;
            } else if (srs_cnt < SRS_MAX_ENTRIES) {
                // Add new
                srs_tbl[srs_cnt].page = page;
                srs_tbl[srs_cnt].step = step;
                srs_tbl[srs_cnt].start_day = start_day;
                srs_cnt++;
            }
            
            // Update last page tracker
            if (page > srs_last_page) {
                srs_last_page = page;
            }
            
            ESP_LOGI(TAG, "Added Pg. %u: start_day=%" PRIu32 " (%s), days_since=%d, step=%d", 
                    page, start_day, entries[i].date, days_since, step);
        }
    }
    
    // Save all at once
    srs_nvs_save();
    ESP_LOGI(TAG, "Calibration batch complete, %d entries loaded", srs_cnt);
}
#endif
