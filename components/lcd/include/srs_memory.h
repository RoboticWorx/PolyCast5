/*

This is a tool to help humans remember things! NOT digital memory.

This file is used for the SRS (spaced repetition system) memory option on PolyCast5.

This allows PolyCast5 to serve as a memory assistant (neurologically) when the user is
trying to memorize new things. This is based on the Ebbinghaus forgetting curve, in which
learned materials is better remembered by reviewing them at increasing intervals for ideal
LTP of synapses between neurons in the brain.

*/

#ifndef SRS_MEMORY_H
#define SRS_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

#define SRS_NS "srs"

#define SRS_MAX_ENTRIES 2048 // Max number of entries
#define SRS_NUM_STEPS 8

typedef struct {
    uint16_t page; // Notebook page number
    uint16_t step; // 0...SRS_NUM_STEPS - 1: e.g. 1d > 3d > 7d ...
    uint32_t start_day; // Day of creation relative to epoch
} srs_entry_t;

extern srs_entry_t srs_tbl[SRS_MAX_ENTRIES];

extern const uint16_t srs_days[];

#ifdef POLYCAST5_SRS_CALIBRATING
// Calibration entry structure for batch loading
typedef struct {
    uint16_t page;
    const char *date; // Format: "MM/DD/YYYY"
} srs_calibration_entry_t;
#endif

/** 
 * @brief Saves SRS struct to NVS
 */
void srs_nvs_save(void);

/** 
 * @brief Loads SRS struct from NVS
 */
void srs_nvs_load(void);

/** 
 * @brief Gets days since local time epoch
 *
 * @param [in] calibrate Offset to add days in the case that the user isn't starting from scratch
 *
 * @returns Days since epoch
 */
uint32_t srs_days_since_epoch_local(void);

/** 
 * @brief Build list of due SRS pages
 *
 * @param [out] out_idx Due entries
 * @param [in] today Current day
 *
 * @returns Total number of due entries
 */
int srs_build_due_list(int *out_idx, int max_out, uint32_t today);

/** 
 * @brief Mark index as reviewed
 *
 * @param [in] idx Index to mark
 * @param [in] today Current day
 */
void srs_mark_reviewed_index(int idx, uint32_t today);

/** 
 * @brief Add or reset page to review
 *
 * @param [in] page Page to adjust
 * @param [in] today Current day
 */
void srs_add_or_reset(uint16_t page, uint32_t today);

/** 
 * @brief Auto-increments the next default page
 *
 * @returns Next default page
 */
uint16_t srs_next_default_page(void);

/** 
 * @brief Gets the current time and data over Wi-Fi to sync to RTC
 *
 * @returns True on success
 */
bool srs_sync_time_over_wifi(void);

#ifdef POLYCAST5_SRS_CALIBRATING
/** 
 * @brief Batch load calibration entries from date strings
 *
 * @param [in] entries Array of calibration entries
 * @param [in] count Number of entries in the array
 */
void srs_batch_load_from_dates(const srs_calibration_entry_t *entries, int count);
#endif

#endif // SRS_MEMORY_H