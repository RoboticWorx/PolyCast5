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
 * @brief Initialize mic capture with an explicit DMA ring size
 *
 * ai_voice_init() forwards the historical 4 x 200 frames (~16.7ms), which suits the STT
 * path. Spectral analysis needs a deeper ring: lcd_task runs at a higher priority on a
 * single core, and one LVGL flush can stall the reader for longer than 16.7ms, which
 * drops samples and corrupts the analysis window.
 *
 * A DMA descriptor caps at 4092 bytes and a stereo 32-bit frame is 8 bytes, so
 * dma_frame_num must stay <= 480 -- grow the ring with dma_desc_num instead.
 *
 * @param dma_desc_num Number of DMA descriptors (>= 2)
 * @param dma_frame_num Stereo frames per descriptor (8..480)
 *
 * @returns ESP error status
 */
esp_err_t ai_voice_init_ex(int dma_desc_num, int dma_frame_num);

/**
 * @brief Read raw 32-bit I2S slots from the mic's RX channel
 *
 * Each stereo frame is two 32-bit slots; the T5848 drives the left slot (index 0) and the
 * 24-bit payload sits in bits [31:8]. Lets a caller run its own capture/conversion loop
 * without exposing the channel handle.
 *
 * @param dst Destination buffer for raw slot words
 * @param dst_bytes Size of dst in bytes
 * @param out_bytes Set to the number of bytes actually read
 * @param timeout_ms Read timeout in milliseconds
 *
 * @returns ESP error status, or ESP_ERR_INVALID_STATE if the mic is not initialized
 */
esp_err_t ai_voice_read_raw(int32_t *dst, size_t dst_bytes, size_t *out_bytes, uint32_t timeout_ms);

/**
 * @brief Number of I2S receive-queue overflows since the last ai_voice_init_ex()
 *
 * A non-zero delta across a capture window means samples were dropped and the window is
 * discontinuous, so any spectral result derived from it should be discarded.
 *
 * @returns Overflow count
 */
uint32_t ai_voice_get_ovf_count(void);

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
