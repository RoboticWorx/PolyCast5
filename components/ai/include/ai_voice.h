#ifndef AI_VOICE_H
#define AI_VOICE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

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
 * @brief Send PCM16 mono 16kHz to xAI realtime WebSocket STT and return the transcript
 * 
 * @param pcm16 Pointer to PCM16 samples
 * @param samples Number of samples in pcm16
 * @param out_text Output buffer for transcript text
 * @param out_text_sz Size of out_text buffer
 * 
 * @returns ESP error status
 */
esp_err_t ai_voice_stt_ws_transcribe_pcm16_xai(const int16_t *pcm16, size_t samples, char *out_text, size_t out_text_sz);

#endif // AI_VOICE_H
