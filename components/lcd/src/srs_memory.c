/*

This is a tool to help humans remember things! NOT digital memory.

This file is used for the SRS (spaced repetition system) memory option on PolyCast5.

This allows PolyCast5 to serve as a memory assistant (neurologically) when the user is
trying to memorize new things. This is based on the Ebbinghaus forgetting curve, in which
learned materials is better remembered by reviewing them at increasing intervals for ideal
LTP of synapses between neurons in the brain.

*/

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "freertos/idf_additions.h"

#include "nvs.h"
#include "esp_log.h"

#include "wifi_funcs.h"
#include "wifi_task.h"
#include "srs_memory.h"

#define TAG "SRS_MEMORY"

// Namespace and keys
#define SRS_NS "srs"
#define SRS_CNT_KEY "cnt" // Number of entries key
#define SRS_LAST_PAGE_KEY "last" // Last page used (for auto-increment)


// SRS intervals: 1d > 3d > 7d > 14d > 1m > 3m > 6m > 12m
const uint16_t srs_days[] = {1, 3, 7, 14, 30, 90, 180, 365};

// Use PSRAM instead
EXT_RAM_BSS_ATTR srs_entry_t srs_tbl[SRS_MAX_ENTRIES];
EXT_RAM_BSS_ATTR static int srs_tmp_idx[SRS_MAX_ENTRIES];

static uint16_t srs_cnt = 0;
static uint16_t srs_last_page = 0;

/* Helpers */

// Check if RTC synced
static bool rtc_likely_unsynced(void)
{
	// Treat anything before 2025 as "unsynced"
	struct tm t; 
	time_t now = time(NULL);
	if (now <= 0) {
		return true;
	}
	
	localtime_r(&now, &t);
	return (t.tm_year < 125); // Years since 1900
}

/* Core SRS logic */

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
	// Due when the number of days since last review (today - last_day) is >= the current step's interval
	uint16_t interval = srs_days[e->step];
	return (today >= e->last_day + interval);
}

void srs_sync_time_over_wifi(void)
{
	// Check if RTC synced
	if (!rtc_likely_unsynced()) {
		return;
	}

	// Ask Wi-Fi to reconnect to the last used network
	wifi_login_t selected_network = wifi_funcs_get_prev(); // Loads boot state saved network info
	selected_network.prev = true; // Connecting to previous
	if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) {
		ESP_LOGE(TAG, "Failed srs_sync_time_over_wifi: xWifiSelectedNetworkQueue previous_network");
	}

	// Wait up to 6s for connection
	if (xSemaphoreTake(xWifiNetworkConnectedSemaphore, pdMS_TO_TICKS(6000)) == pdTRUE) {
		// Get time
		wifi_funcs_get_current_date_time();
		
		// Done with Wi-Fi
		xSemaphoreGive(xWifiDisconnectSemaphore);
	}
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
		srs_tbl[idx].last_day = today;
	}
	// Else if there's room, append a new entry
	else if (srs_cnt < SRS_MAX_ENTRIES) {
		srs_tbl[srs_cnt].page = page;
		srs_tbl[srs_cnt].step = 0;
		srs_tbl[srs_cnt].last_day = today;
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
	
	// Update last_day
	srs_tbl[idx].last_day = today;
	
	// Persist to NVS
	srs_nvs_save();
}

// Returns number of due entries; fills up to max_out indices of due order
int srs_build_due_list(int *out_idx, int max_out, uint32_t today)
{
	// First collect
	int n = 0;
	
	// Build the set of indices that are due today
	for (int i = 0; i < srs_cnt; ++i) {
		if (srs_is_due(&srs_tbl[i], today)) {
			srs_tmp_idx[n++] = i;
		}
	}
	
	// Most overdue appears first
	for (int i = 0; i + 1 < n; ++i) {
		for (int j = i + 1; j < n; ++j) {
			int di = (int)(today - srs_tbl[srs_tmp_idx[i]].last_day) - (int)srs_days[srs_tbl[srs_tmp_idx[i]].step];
			int dj = (int)(today - srs_tbl[srs_tmp_idx[j]].last_day) - (int)srs_days[srs_tbl[srs_tmp_idx[j]].step];
			
			// Swap if greater
			if (dj > di) {
				int t = srs_tmp_idx[i];
				srs_tmp_idx[i] = srs_tmp_idx[j];
				srs_tmp_idx[j] = t;
			}
		}
	}
	
	// Emit
	int out = (n < max_out) ? n : max_out;
	for (int k = 0; k < out; ++k) {
		out_idx[k] = srs_tmp_idx[k];
	}
	
	// Return the total number due
	return n;
}

void srs_nvs_load(void)
{
	nvs_handle_t h;
	
	// Open NVS
	if (nvs_open(SRS_NS, NVS_READONLY, &h) != ESP_OK) {
		srs_cnt = 0;
		srs_last_page = 0;
		return;
	}

	// Get number of entries
	uint16_t cnt = 0;
	nvs_get_u16(h, SRS_CNT_KEY, &cnt);
	srs_cnt = 0;

	// Get last used page (for +1 add)
	uint16_t lastp = 0;
	nvs_get_u16(h, SRS_LAST_PAGE_KEY, &lastp);
	srs_last_page = lastp;

	// Get all entries
	for (uint16_t i = 0; i < cnt && i < SRS_MAX_ENTRIES; i++) {
		// Format key
		char key[7]; // "e00" -> "eu16"
		snprintf(key, sizeof(key), "e%04u", i); // e.g. "e0001"
		
		// Get srs_entry_t blob
		size_t len = sizeof(srs_entry_t);
		srs_entry_t tmp;
		if (nvs_get_blob(h, key, &tmp, &len) == ESP_OK && len == sizeof(srs_entry_t)) {
			// If good srs_cnt++
			srs_tbl[srs_cnt++] = tmp;
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
		return;
	}

	// Set count and last used page
	nvs_set_u16(h, SRS_CNT_KEY, srs_cnt);
	nvs_set_u16(h, SRS_LAST_PAGE_KEY, srs_last_page);

	// Save each entry
	for (uint16_t i = 0; i < srs_cnt; i++) {
		// Format key
		char key[7];
		snprintf(key, sizeof(key), "e%04u", i);
		
		// Save srs_entry_t blob
		nvs_set_blob(h, key, &srs_tbl[i], sizeof(srs_entry_t));
	}
	
	// Commit changes
	nvs_commit(h);
	
	// Close NVS
	nvs_close(h);
}

// Converts Unix seconds to whole days
uint32_t srs_days_since_epoch_local(void)
{
	time_t now = time(NULL);
	if (now <= 0) {
		return 0;
	}
	
	// Round down by 86400 -> today index
	return (uint32_t)(now / 86400);
}
