#ifndef AI_FREQ_H
#define AI_FREQ_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/idf_additions.h"

// Spectrum strip columns spanning the analysis band (~9Hz per column)
#define AI_FREQ_BARS 29

// Sentinel for "no peak to highlight"
#define AI_FREQ_BAR_NONE 0xFF

/**
 * Measurement range. The mode picks the decimation factor, which sets the sample rate the
 * FFT runs at and therefore trades bin resolution against how high the band reaches -- the
 * two cannot both be improved with a fixed transform size.
 *
 *   BELT    1.5 Hz bins over 40-300 Hz    finest resolution, for belts and bass strings
 *   GUITAR  2.9 Hz bins over 60-600 Hz    all six guitar strings, violin, mid piano
 *   PIANO   5.9 Hz bins over 80-2000 Hz   E2 to B6, about 60 of a piano's 88 keys
 *
 * The bottom of a piano (A0 27.5 Hz to D#1) is out of reach in every mode: it sits at the
 * T5848's own 28 Hz low-frequency corner, so the microphone is the limit, not the analysis.
 */
typedef enum {
    AI_FREQ_MODE_BELT = 0,
    AI_FREQ_MODE_GUITAR,
    AI_FREQ_MODE_PIANO,
    AI_FREQ_MODE_COUNT,
} ai_freq_mode_t;

/**
 * @brief Short display name for a mode ("Belt", "Guitar", "Piano")
 *
 * @param mode ai_freq_mode_t value; out-of-range values fall back to BELT
 *
 * @returns Static string, never NULL
 */
const char *ai_freq_mode_name(uint8_t mode);

/**
 * @brief Lowest and highest frequency a mode can report, in whole Hz (for UI labels)
 *
 * @param mode ai_freq_mode_t value; out-of-range values fall back to BELT
 *
 * @returns Band edge in Hz
 */
uint16_t ai_freq_mode_lo_hz(uint8_t mode);
uint16_t ai_freq_mode_hi_hz(uint8_t mode);

// Capture/analysis state, driven by ai_freq_run()
typedef enum {
    AI_FREQ_ST_WARMUP = 0, // Mic settling, no readings yet
    AI_FREQ_ST_LISTENING,  // Idle, waiting for a pluck
    AI_FREQ_ST_MEASURING,  // Pluck detected, collecting the best frame of the burst
    AI_FREQ_ST_RESULT,     // A confident reading is available in f_mhz
    AI_FREQ_ST_OVERRUN,    // I2S dropped samples, window discarded
    AI_FREQ_ST_ERROR,      // Mic could not be brought up
    AI_FREQ_ST_NOISY,      // Room/machine is loud enough that a pluck cannot be picked out
    AI_FREQ_ST_RESEED,     // Range changed: decimator re-seeding, microphone still open
} ai_freq_state_t;

// Snapshot published to the LCD every analysis hop
// Plain values only: lcd_task owns every LVGL object and there is no LVGL mutex, so
// nothing here may be a pointer into either task's state
typedef struct {
    uint32_t seq;                 // Bumped on every publish, so the UI redraws only on change
    uint8_t  state;               // ai_freq_state_t
    uint32_t f_mhz;               // Latched fundamental in milli-hertz (0 = nothing yet)
    uint32_t results;             // Bumped each time a NEW confident reading is latched
    uint16_t conf;                // Peak/noise-floor power ratio, clamped
    uint8_t  level_pct;           // Input level 0..100, so the user can tell if they're close enough
    uint8_t  peak_bar;            // Strip index holding the fundamental, or AI_FREQ_BAR_NONE
    uint8_t  mode;                // Mode this frame was measured in (echoed back to the UI)
    uint8_t  bars[AI_FREQ_BARS];  // 0..100, log-scaled magnitudes
    uint32_t ovf_count;           // I2S overruns this session (diagnostics)
} ai_freq_result_t;

// Cleared by the LCD page to stop capture and hand the mic back
extern volatile bool ai_freq_running;

// Measurement range, an ai_freq_mode_t. The LCD writes it; ai_freq_run picks the change up
// on its next hop and re-seeds the decimator without dropping the microphone.
extern volatile uint8_t ai_freq_mode;

// Length 1: ai_freq.c overwrites, the LCD page peeks without blocking
extern QueueHandle_t xAiFreqResultQueue;

/**
 * @brief Capture from the mic and publish fundamental-frequency estimates until
 *        *keep_running goes false
 *
 * Owns the mic for its whole lifetime: brings I2S up via ai_voice_init_ex() and tears it
 * back down via ai_voice_deinit() on every exit path, including errors. Blocks until the
 * caller's flag clears, so it must only ever run on ai_task (the single I2S owner).
 *
 * Allocates its ~26KB of working buffers on entry and frees them on exit, so the feature
 * costs nothing while the page is closed. Nothing goes on the stack: ai_task's is only 4KB.
 *
 * @param keep_running Pointer to a volatile bool: runs while true
 *
 * @returns ESP error status
 */
esp_err_t ai_freq_run(volatile bool *keep_running);

#endif // AI_FREQ_H
