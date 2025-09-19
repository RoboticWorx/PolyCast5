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

#include <stdint.h>

#define SRS_MAX_ENTRIES 2048 // Max number of entries

typedef struct {
	uint16_t page; // Notebook page number
	uint16_t step; // 0...SRS_NUM_STEPS - 1
	uint32_t last_day; // Days since epoch of last review/creation
} srs_entry_t;

extern srs_entry_t srs_tbl[SRS_MAX_ENTRIES];

extern const uint16_t srs_days[];
#define SRS_NUM_STEPS (sizeof(srs_days)/sizeof(srs_days[0]))

/** 
 * @brief Saves SRS struct to NVS
 */
void srs_nvs_save(void);

/** 
 * @brief Loads SRS struct from NVS
 */
void srs_nvs_load(void);

/** 
 * @brief Saves SRS struct to NVS
 *
 * @returns Days since epoch
 */
uint32_t srs_days_since_epoch_local(void);

/** 
 * @brief Saves SRS struct to NVS
 *
 * @param [out] out_idx Due entries
 * @param [in] max_out Max number of entries
 * @param [in] today Current day
 *
 * @returns Total number due of entries
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
 */
void srs_sync_time_over_wifi(void);

#endif // SRS_MEMORY_H