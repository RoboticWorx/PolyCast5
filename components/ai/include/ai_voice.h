#ifndef AI_VOICE_H
#define AI_VOICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// Returned when the API rejects the request with HTTP 429 (out of credits / rate limited)
#define AI_VOICE_ERR_RATE_LIMITED 0x2001

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

/**
 * @brief Boot hardware self-test: verify the T5848 mic is driving the I2S data line.
 *        Brings the I2S channel up, samples ~110 ms of data with a pull-down on SD
 *        (so an absent mic reads all-zero, and a stuck/shorted line reads one
 *        constant value), then tears the channel back down.
 *        Boot-time only: must not run concurrently with dictation.
 *
 * @param alive Set true if the data line shows a live, varying signal;
 *              false if silent or stuck at a constant value
 *
 * @returns ESP_OK if the probe ran (see *alive), or the I2S/memory error that stopped it
 */
esp_err_t ai_voice_mic_selftest(bool *alive);

/**
 * @brief Records audio from the I2S microphone and downsamples it to 16kHz mono 16-bit PCM
 * 
 * @param keep_recording Pointer to a volatile bool: recording continues while true, stops when false
 * @param out Pointer to output struct to fill
 * 
 * @returns ESP error status
 */
esp_err_t ai_voice_record_pcm16_16k(volatile bool *keep_recording, ai_voice_pcm_t *out);

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
 * memory use stays bounded even for ~30s recordings. Returns AI_VOICE_ERR_RATE_LIMITED on HTTP 429.
 *
 * @param pcm16 Pointer to PCM16 samples
 * @param samples Number of samples in pcm16
 * @param out_text Output buffer for transcript text
 * @param out_text_sz Size of out_text buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_voice_stt_transcribe_pcm16_xai(const int16_t *pcm16, size_t samples, char *out_text, size_t out_text_sz);

#endif // AI_VOICE_H
