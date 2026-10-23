#ifndef AI_VOICE_H
#define AI_VOICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define AI_VOICE_SAMPLE_RATE_HZ 16000 // PCM rate from ai_voice_record_pcm16_16k()
#define AI_VOICE_MIN_SAMPLES (AI_VOICE_SAMPLE_RATE_HZ / 4) // 250 ms: anything shorter is a tap, not speech
#define AI_VOICE_MAX_SAMPLES (AI_VOICE_SAMPLE_RATE_HZ * 60) // Recording stops itself at 60 s

// Audio buffer returned by ai_voice_record_pcm16_16k()
// Must call ai_voice_free_pcm() when done
typedef struct {
    int16_t *pcm16; // Pointer to samples
    size_t samples; // Number of samples in pcm16
} ai_voice_pcm_t;

/**
 * @brief Initialize mic capture: Creates an I2S RX channel, configures and enables it
 * 
 * @returns ESP error status
 */
esp_err_t ai_voice_init(void);

/**
 * @brief Deinitialize mic capture and free resources
 * 
 * @returns ESP error status
 */
esp_err_t ai_voice_deinit(void);

/** Result of ai_voice_mic_selftest(). Slots are scored separately: the T5848's LR
 *  pin is tied to GND, so it must drive the LEFT slot and leave the right tristated.
 *  An open LR joint moves it to the right slot, which a combined count cannot see. */
typedef struct {
    bool alive;        // Live varying data on the expected (left) slot
    bool wrong_slot;   // Data on the right slot instead - LR (pin 2) not tied to GND
    uint8_t left_pct;  // Percent of left-slot words nonzero (expect near 100)
    uint8_t right_pct; // Percent of right-slot words nonzero (expect 0)
    int32_t spread;    // max-min of the active slot's 24-bit samples; 0 if stuck
} ai_voice_mic_probe_t;

/**
 * @brief Boot hardware self-test: verify the T5848 mic is driving the I2S data line.
 *        Brings the I2S channel up, samples ~110 ms of data with a pull-down on SD
 *        (so an absent mic reads all-zero, and a stuck/shorted line reads one
 *        constant value), then tears the channel back down.
 *        Boot-time only: must not run concurrently with dictation.
 *
 *        Proves the electrical chain: the 1V8 rail, all three FXLP34P5X translators,
 *        and a powered, clocked, converting die. It cannot see a blocked acoustic
 *        port, a punctured membrane or low sensitivity - the idle noise floor is the
 *        mic's own EIN, not the room, so it reads the same either way.
 *
 * @param out Probe result; see ai_voice_mic_probe_t
 *
 * @returns ESP_OK if the probe ran (see out->alive), or the I2S/memory error that stopped it
 */
esp_err_t ai_voice_mic_selftest(ai_voice_mic_probe_t *out);

/**
 * @brief Records audio from the I2S microphone and downsamples it to 16kHz mono 16-bit PCM
 * 
 * Stops on its own at AI_VOICE_MAX_SAMPLES, or early if PSRAM can't grow the buffer, keeping
 * what it has. Either way it clears *keep_recording so the caller can tell.
 *
 * @param keep_recording Pointer to a volatile bool: recording continues while true, stops when false
 * @param out Pointer to output struct to fill
 * 
 * @returns ESP error status (ESP_ERR_NO_MEM only if nothing could be stored)
 */
esp_err_t ai_voice_record_pcm16_16k(volatile bool *keep_recording, ai_voice_pcm_t *out);

/**
 * @brief Read and clear the loudest microphone block peak seen since the last call
 *
 * Fed by ai_voice_record_pcm16_16k() while a recording is running, which evaluates one
 * peak per ~5.33ms I2S block. The value is a peak-hold accumulator rather than the most
 * recent block, so a caller polling far more slowly (the voice orb renders every ~33ms)
 * still sees every syllable onset. Returns 0 when nothing above the silence floor has
 * arrived, including when no recording is in progress.
 *
 * @returns Peak sample magnitude, 0..32767
 */
uint16_t ai_voice_take_level_peak(void);

/**
 * @brief Free the PCM buffer used by ai_voice_record_pcm16_16k()
 * 
 * @param p Pointer to ai_voice_pcm_t struct to free
 */
void ai_voice_free_pcm(ai_voice_pcm_t *p);

/**
 * @brief Sets the microphone I2S pins low to put T5848 mic into sleep mode
 * 
 * @returns ESP error status
 */
esp_err_t ai_voice_force_sleep_pins_low(void);

/**
 * @brief Send PCM16 mono 16kHz to the selected STT provider and return the transcript
 *
 * Endpoint, model, and format field name come from the provider registry via
 * ai_provider_resolve_stt() (e.g. xAI grok-stt or OpenAI/Groq Whisper), using the
 * separate STT key when one is configured. Wraps the PCM in a 44-byte WAV (RIFF)
 * header and POSTs as multipart/form-data with language=en. Streams the body so
 * memory use stays bounded even for 60 s recordings.
 *
 * @param pcm16 Pointer to PCM16 samples
 * @param samples Number of samples in pcm16
 * @param out_text Output buffer for transcript text
 * @param out_text_sz Size of out_text buffer
 *
 * @returns ESP_OK, AI_ERR_NO_SPEECH for an empty transcript, another AI_ERR_* code (ai_utils.h), or ESP_ERR_NO_MEM
 */
esp_err_t ai_voice_stt_transcribe_pcm16_xai(const int16_t *pcm16, size_t samples, char *out_text, size_t out_text_sz);

#endif // AI_VOICE_H
