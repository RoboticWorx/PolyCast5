#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "polycast5_macros.h"
#include "polycast5_gpios.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "driver/i2s_std.h"

#include "cJSON.h"

#include "ai_task.h"
#include "ai_voice.h"
#include "ai_utils.h"

#define TAG "AI_VOICE"

#define MIC_FS_HZ 48000 // 48kHz input from mic
#define STT_FS_HZ 16000 // 16kHz output for STT

// Each read pulls this many stereo frames (larger reads reduce overhead, smaller reads reduce latency)
#define FRAMES_PER_READ 256

// Normalize audio to improve STT reliability
#define AI_VOICE_ENABLE_NORMALIZE      1
#define AI_VOICE_NORMALIZE_TARGET_PEAK 12000
#define AI_VOICE_NORMALIZE_MAX_GAIN    20.0f

#define SOUND_ANIM_THRESHOLD 100

// xAI REST STT
#define XAI_STT_URL            "https://api.x.ai/v1/stt"
#define STT_HTTP_TIMEOUT_MS    30000
#define STT_WRITE_CHUNK_BYTES  4096
#define STT_RESPONSE_MAX_BYTES (32 * 1024)
#define STT_MULTIPART_BOUNDARY "----PolyCast5SttBoundaryP8c0z"

static i2s_chan_handle_t i2s_rx_channel = NULL;
static bool voice_inited = false;

// Converts a 32-bit I2S slot value (from the microphone) into a standard 16-bit signed PCM audio sample
static inline int16_t slot32_to_pcm16(int32_t w)
{
    // Signed 24-bit in bits [31:8]
    // Shift while preserving the sign
    int32_t s24 = w >> 8; // Keep sign
    int32_t s16 = s24 >> 8; // 24 -> 16

    // Clamp to int16 range
    if (s16 > 32767) {
        s16 = 32767;
    }
    if (s16 < -32768) {
        s16 = -32768;
    }

    return (int16_t)s16;
}

// Takes three 32-bit input samples (a, b, c) and returns their average as a 16-bit PCM sample
static inline int16_t decim3_avg(int32_t a, int32_t b, int32_t c)
{
    // Average in 32-bit space
    int32_t avg = (a / 3) + (b / 3) + (c / 3);

    // Convert to PCM16
    return slot32_to_pcm16(avg);
}

// Applies a gain to an array of 16-bit PCM samples and
// ensures the result stays within the valid 16-bit signed range
static void pcm_apply_gain_clip(int16_t *x, size_t n, float gain)
{
    // Loop through each sample in the array x
    for (size_t i = 0; i < n; ++i) {
        // Multiplies each sample by gain
        int32_t v = (int32_t)((float)x[i] * gain);

        // Clamps the result to the int16_t range
        if (v >  32767) {
            v =  32767;
        }
        if (v < -32768) {
            v = -32768;
        }

        // Stores it back into the array
        x[i] = (int16_t)v;
    }
}

// Scans an array of 16-bit PCM samples to find the current peak,
// then applies a gain so that the new peak matches a specified target_peak
static void pcm_normalize_peak(int16_t *x, size_t n, int16_t target_peak)
{
    int16_t peak = 1;

    // Finds the largest absolute sample value
    for (size_t i = 0; i < n; ++i) {
        int16_t a = (x[i] < 0) ? (int16_t)-x[i] : x[i];
        if (a > peak) {
            peak = a;
        }
    }

    // Calculates the gain needed to scale this peak to target_peak
    float gain = (float)target_peak / (float)peak;

    // Clamps the gain to avoid excessive amplification
    if (gain > AI_VOICE_NORMALIZE_MAX_GAIN) {
        gain = AI_VOICE_NORMALIZE_MAX_GAIN;
    }
    if (gain < 1.0f) {
        gain = 1.0f; // Don't make it quieter
    }

    pcm_apply_gain_clip(x, n, gain);
}

// Initializes the I2S microphone input for the system
esp_err_t ai_voice_init(void)
{
    // If already initialized, do nothing
    if (voice_inited) {
        return ESP_OK;
    }

    esp_err_t err;

    // Release any pin holds
    err = gpio_hold_dis(I2S_T5848_SCK_PIN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_init: gpio_hold_dis(I2S_T5848_SCK_PIN) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_hold_dis(I2S_T5848_WS_PIN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_init: gpio_hold_dis(I2S_T5848_WS_PIN) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Reset pins to default state
    err = gpio_reset_pin(I2S_T5848_SCK_PIN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_init: gpio_reset_pin(I2S_T5848_SCK_PIN) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = gpio_reset_pin(I2S_T5848_WS_PIN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_init: gpio_reset_pin(I2S_T5848_WS_PIN) failed: %s", esp_err_to_name(err));
        return err;
    }

    // Log free internal DMA-capable heap (diagnose ESP_ERR_NO_MEM on I2S init)
#ifdef POLYCAST5_DEBUG
    ESP_LOGW(TAG, "ai_voice_init before alloc: Free internal: %u, largest DMA block: %u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
#endif

    // Create an RX channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 200;
    err = i2s_new_channel(&chan_cfg, NULL, &i2s_rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_init: i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    // Configures I2S in standard (Philips) mode
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_FS_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // No master clock
            .bclk = I2S_T5848_SCK_PIN, // Bit clock
            .ws   = I2S_T5848_WS_PIN,  // Word select (LR clock)
            .dout = I2S_GPIO_UNUSED,   // No data out
            .din  = I2S_T5848_SD_PIN,  // Data in
            // No inversion
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    // Initialize I2S in standard mode
    err = i2s_channel_init_std_mode(i2s_rx_channel, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_init: i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(i2s_rx_channel);
        i2s_rx_channel = NULL;
        return err;
    }

    // Enable I2S channel (RX)
    err = i2s_channel_enable(i2s_rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_init: i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(i2s_rx_channel);
        i2s_rx_channel = NULL;
        return err;
    }

    voice_inited = true;
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Mic init OK (48k stereo, 32-bit slots).");
#endif
    return ESP_OK;
}

// Deinitializes the I2S microphone input for the system
esp_err_t ai_voice_deinit(void)
{
    // If already not initialized, do nothing
    if (!voice_inited) {
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;

    // Delete I2S RX channel if exists
    if (i2s_rx_channel) {
        err = i2s_channel_disable(i2s_rx_channel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ai_voice_deinit: i2s_channel_disable failed: %s", esp_err_to_name(err));
        }
        err = i2s_del_channel(i2s_rx_channel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ai_voice_deinit: i2s_del_channel failed: %s", esp_err_to_name(err));
        }
        i2s_rx_channel = NULL;
    }

    voice_inited = false;

    esp_err_t sleep_err = ai_voice_force_sleep_pins_low();
    if (sleep_err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_deinit: ai_voice_force_sleep_pins_low failed: %s", esp_err_to_name(sleep_err));
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Mic deinit successful.");
#endif

    return err;
}

esp_err_t ai_voice_force_sleep_pins_low(void)
{
    esp_err_t err = ESP_OK;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << I2S_T5848_SCK_PIN) | (1ULL << I2S_T5848_WS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&io);

    err = gpio_set_level(I2S_T5848_SCK_PIN, 0);
    err = gpio_set_level(I2S_T5848_WS_PIN, 0);

    // For light/deep sleep
    err = gpio_hold_en(I2S_T5848_SCK_PIN);
    err = gpio_hold_en(I2S_T5848_WS_PIN);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "ai_voice_force_sleep_pins_low successful.");
#endif

    return err;
}

// Records audio from the I2S microphone, downsamples it to 16kHz mono 16-bit PCM
#define INITIAL_BUFFER_SAMPLES 16000  // Start with 1 second @16kHz, then double as needed

esp_err_t ai_voice_record_pcm16_16k(volatile bool *keep_recording, ai_voice_pcm_t *out)
{
    // Validate arguments
    if (!out || !keep_recording) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out)); // Clear output struct

    // Ensure mic is initialized
    if (!voice_inited) {
        esp_err_t err = ai_voice_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ai_voice_record_pcm16_16k: ai_voice_init failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    // Dynamic buffer (prefer PSRAM)
    size_t capacity = 0;
    int16_t *pcm16 = NULL;
    size_t out_idx = 0;

    // Temp read buffer:
    //  - Read stereo frames
    //  - Each frame has 2 x 32-bit words: [L][R]
    POLYCAST5_USE_PSRAM_BSS static int32_t i2s_words[FRAMES_PER_READ * 2];
    memset(i2s_words, 0, sizeof(i2s_words));

    while (*keep_recording) {
        size_t bytes_read = 0;

        // Read a block of I2S data
        esp_err_t err = i2s_channel_read(
            i2s_rx_channel,
            i2s_words,
            sizeof(i2s_words),
            &bytes_read,
            portMAX_DELAY
        );
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ai_voice_record_pcm16_16k: i2s_channel_read failed: %s", esp_err_to_name(err));
            if (pcm16) {
                heap_caps_free(pcm16);
            }
            return err;
        }

        size_t words_read  = bytes_read / sizeof(int32_t);
        size_t frames_read = words_read / 2; // L,R per frame

        // Ensure enough capacity for new samples (each 3 frames -> 1 sample)
        size_t new_samples = frames_read / 3;
        while (out_idx + new_samples > capacity) {
            size_t new_capacity = capacity ? (capacity * 2) : INITIAL_BUFFER_SAMPLES;
            int16_t *new_pcm = (int16_t *)heap_caps_realloc(
                    pcm16,
                    new_capacity * sizeof(int16_t),
                    MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM
            );
            if (!new_pcm) {
                ESP_LOGE(TAG, "ai_voice_record_pcm16_16k: No mem to resize PCM buffer (%u samples)", (unsigned)new_capacity);
                if (pcm16) {
                    heap_caps_free(pcm16);
                }
                return ESP_ERR_NO_MEM;
            }
            pcm16 = new_pcm;
            capacity = new_capacity;
        }

        int16_t block_peak = 0; // Peak tracker for this block
        
        // Convert frames in groups of 3:
        // - Use left slot only (index 0)
        // - Average 3 consecutive samples
        for (size_t f = 0; (f + 2) < frames_read; f += 3) {
            int32_t l0 = i2s_words[(f + 0) * 2 + 0];
            int32_t l1 = i2s_words[(f + 1) * 2 + 0];
            int32_t l2 = i2s_words[(f + 2) * 2 + 0];

            // Decimate by 3 with averaging
            int16_t sample = decim3_avg(l0, l1, l2);
            pcm16[out_idx++] = sample;

            // -32768 can't be negated in int16 safely, so clamp it
            int16_t abs_mag = (sample == (int16_t)-32768) ? 32767 : (sample < 0 ? (int16_t)-sample : sample);

            // Track the max magnitude seen in this block
            if (abs_mag > block_peak) {
                block_peak = abs_mag;
            }
        }
        // If sound detected, notify LCD
        if (block_peak > SOUND_ANIM_THRESHOLD) {
            xSemaphoreGive(xAiSoundHeardSemaphore);
        }

        // Note: Any remaining frames (<3) are discarded (~1-2 frames max, <1.25ms @48kHz)
    }

#if AI_VOICE_ENABLE_NORMALIZE
    // Normalize to a reasonable peak so STT hears consistently
    if (out_idx > 0) {
        pcm_normalize_peak(pcm16, out_idx, (int16_t)AI_VOICE_NORMALIZE_TARGET_PEAK);
    }
#endif

    // Trim buffer to exact size (saves memory if needed)
    if (out_idx < capacity && out_idx > 0) {
        int16_t *trimmed = (int16_t *)heap_caps_realloc(
            pcm16,
            out_idx * sizeof(int16_t),
            MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM
        );
        if (trimmed) {
            pcm16 = trimmed;
        } // If realloc fails, keep original (oversized is fine)
    }

    // Fill out output struct
    out->pcm16 = pcm16;
    out->samples = out_idx;
    return ESP_OK;
}

// Frees the PCM buffer allocated by ai_voice_record_pcm16_16k()
void ai_voice_free_pcm(ai_voice_pcm_t *p)
{
    // If null, do nothing
    if (!p) {
        return;
    }

    // Free pcm16 buffer if exists
    if (p->pcm16) {
        heap_caps_free(p->pcm16);
        p->pcm16 = NULL;
    }

    // Reset sample count
    p->samples = 0;
}

// Writes a 32-bit unsigned value to p[0..3] in little-endian order.
// WAV is little-endian regardless of host endianness, so lay the bytes out explicitly.
static inline void wav_put_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v       & 0xFF);
    p[1] = (uint8_t)(v >>  8 & 0xFF);
    p[2] = (uint8_t)(v >> 16 & 0xFF);
    p[3] = (uint8_t)(v >> 24 & 0xFF);
}

// Writes a 16-bit unsigned value to p[0..1] in little-endian order
static inline void wav_put_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v      & 0xFF);
    p[1] = (uint8_t)(v >> 8 & 0xFF);
}

// Builds a canonical 44-byte WAV (RIFF/WAVE/fmt/data) header for PCM16 mono at STT_FS_HZ.
// The xAI /v1/stt endpoint expects a container (wav/mp3/mp4/m4a), not raw PCM, so we
// prepend this 44-byte header in front of our PCM buffer to form a valid WAV file.
static void wav_build_header_pcm16_mono(uint8_t hdr[44], uint32_t pcm_bytes)
{
    // Audio format constants for PCM16 mono 16kHz
    const uint32_t sample_rate   = STT_FS_HZ;
    const uint16_t channels      = 1;
    const uint16_t bits_per_samp = 16;
    const uint16_t block_align   = channels * (bits_per_samp / 8); // 2 bytes per sample
    const uint32_t byte_rate     = sample_rate * block_align; // bytes/sec
    const uint32_t riff_size     = 36 + pcm_bytes; // full file size - 8

    // RIFF chunk header: "RIFF" + size + "WAVE"
    memcpy(&hdr[0], "RIFF", 4);
    wav_put_u32_le(&hdr[4], riff_size);
    memcpy(&hdr[8], "WAVE", 4);

    // "fmt " subchunk: describes the audio format
    memcpy(&hdr[12], "fmt ", 4);
    wav_put_u32_le(&hdr[16], 16); // fmt chunk size (16 for PCM)
    wav_put_u16_le(&hdr[20], 1); // format code 1 = PCM
    wav_put_u16_le(&hdr[22], channels);
    wav_put_u32_le(&hdr[24], sample_rate);
    wav_put_u32_le(&hdr[28], byte_rate);
    wav_put_u16_le(&hdr[32], block_align);
    wav_put_u16_le(&hdr[34], bits_per_samp);

    // "data" subchunk header: the PCM samples follow this in the stream
    memcpy(&hdr[36], "data", 4);
    wav_put_u32_le(&hdr[40], pcm_bytes);
}

// Writes the full buffer to the HTTP client, looping on partial writes.
// esp_http_client_write() may accept fewer bytes than requested under back-pressure,
// so keep calling it until every byte is sent or we hit a hard error.
static esp_err_t stt_http_write_all(esp_http_client_handle_t client, const uint8_t *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        // Cap each write so large PCM buffers don't monopolize the TLS layer
        size_t chunk = len - written;
        if (chunk > STT_WRITE_CHUNK_BYTES) {
            chunk = STT_WRITE_CHUNK_BYTES;
        }

        // Push one chunk into the HTTP client
        int n = esp_http_client_write(client, (const char *)(buf + written), chunk);
        if (n <= 0) {
            ESP_LOGE(TAG, "stt_http_write_all: esp_http_client_write failed (n=%d)", n);
            return ESP_FAIL;
        }

        // Advance by however many bytes actually landed
        written += (size_t)n;
    }

    return ESP_OK;
}

esp_err_t ai_voice_stt_transcribe_pcm16_xai(const int16_t *pcm16, size_t samples, char *out_text, size_t out_text_sz)
{
    // Validate arguments
    if (!pcm16 || samples == 0 || !out_text || out_text_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    // Shared state used by cleanup: NULL/0-initialized so cleanup is safe on early failures
    esp_err_t err = ESP_OK;
    esp_http_client_handle_t client = NULL;
    char *resp_buf = NULL;
    size_t resp_len = 0;

    // Load xAI API key from NVS (memset first to clear any stale bytes from a prior call)
    POLYCAST5_USE_PSRAM_BSS static char api_key[AI_API_KEY_MAX_LEN] = {0};
    memset(api_key, 0, sizeof(api_key));
    err = ai_utils_load_api_key_nvs(api_key, sizeof(api_key));
    if (err != ESP_OK || api_key[0] == '\0') {
        ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: ai_utils_load_api_key_nvs failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Precompute sizes that drive Content-Length.
    // The file part payload is: [44-byte WAV header][raw PCM16 bytes], with no trailing CRLF
    // inside the part itself (the CRLF lives at the start of the closing boundary suffix).
    const uint32_t pcm_bytes = (uint32_t)(samples * sizeof(int16_t));
    const char *boundary = STT_MULTIPART_BOUNDARY;

    // Static PSRAM-BSS scratch buffers: not re-entrant, but ai_task calls this serially
    POLYCAST5_USE_PSRAM_BSS static char mp_prefix[1024];
    POLYCAST5_USE_PSRAM_BSS static char mp_suffix[128];

    // Build the multipart prefix: four form-data parts (model, format, language, file headers).
    // The file part's *body* (WAV + PCM) is appended separately so we don't embed binary in snprintf.
    int prefix_len = snprintf(mp_prefix, sizeof(mp_prefix),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "grok-stt\r\n" // Want STT model
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"format\"\r\n\r\n"
        "json\r\n" // Response as JSON
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
        "en\r\n" // English language
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary, boundary, boundary, boundary);

    // Closing delimiter: CRLF terminates the file part body, then "--boundary--" closes the envelope
    int suffix_len = snprintf(mp_suffix, sizeof(mp_suffix), "\r\n--%s--\r\n", boundary);

    // Guard against snprintf truncation (both lengths must be positive and fit within their buffers)
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(mp_prefix) ||
            suffix_len <= 0 || suffix_len >= (int)sizeof(mp_suffix)) {
        ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: multipart buffer overflow");
        return ESP_FAIL;
    }

    // Build the 44-byte WAV header so the server sees a proper RIFF container, not raw PCM
    uint8_t wav_hdr[44];
    wav_build_header_pcm16_mono(wav_hdr, pcm_bytes);

    // Total Content-Length must exactly equal the bytes streamed over write().
    // Any mismatch leaves the server expecting more data or closing the connection early.
    const size_t content_length = (size_t)prefix_len + sizeof(wav_hdr) + pcm_bytes + (size_t)suffix_len;

    // Authorization: "Bearer <key>"; Content-Type carries the boundary so the server can parse the body
    POLYCAST5_USE_PSRAM_BSS static char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    POLYCAST5_USE_PSRAM_BSS static char ctype_header[128];
    snprintf(ctype_header, sizeof(ctype_header), "multipart/form-data; boundary=%s", boundary);

    // Configure HTTPS request for xAI /v1/stt (TLS via ESP-IDF CA bundle, 30s overall timeout)
    esp_http_client_config_t cfg = {
        .url = XAI_STT_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = STT_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
    };

    // Create the HTTP client (one-shot: we destroy it at cleanup)
    client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: esp_http_client_init failed");
        return ESP_FAIL;
    }

    // Attach the three custom headers (Authorization + the multipart Content-Type + Accept)
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", ctype_header);
    esp_http_client_set_header(client, "Accept", "application/json");

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "STT upload: pcm_bytes=%u content_length=%u",
             (unsigned)pcm_bytes, (unsigned)content_length);
#endif

    // Open the TLS connection and send the request line + headers (Content-Length = content_length)
    err = esp_http_client_open(client, (int)content_length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: esp_http_client_open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // Stream the body in four pieces. Order matters: prefix headers, WAV header, PCM payload, closing boundary.
    err = stt_http_write_all(client, (const uint8_t *)mp_prefix, (size_t)prefix_len);
    if (err != ESP_OK) goto cleanup;

    err = stt_http_write_all(client, wav_hdr, sizeof(wav_hdr));
    if (err != ESP_OK) goto cleanup;

    // PCM samples are int16_t; ESP32-C5 is little-endian which matches the WAV spec, so write raw bytes
    err = stt_http_write_all(client, (const uint8_t *)pcm16, pcm_bytes);
    if (err != ESP_OK) goto cleanup;

    err = stt_http_write_all(client, (const uint8_t *)mp_suffix, (size_t)suffix_len);
    if (err != ESP_OK) goto cleanup;

    // Read response status line + headers. fetch_headers returns Content-Length (>0),
    // 0 if the server uses chunked encoding, or negative on error.
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "STT response: status=%d content_len=%d", status, content_len);
#endif

    // Allocate response buffer (PSRAM preferred, internal heap as fallback).
    // When Content-Length is unknown (chunked, content_len <= 0) size up to the safety cap
    // so a long transcript isn't silently truncated.
    size_t cap = (content_len > 0) ? (size_t)content_len + 1 : STT_RESPONSE_MAX_BYTES;
    if (cap > STT_RESPONSE_MAX_BYTES) {
        cap = STT_RESPONSE_MAX_BYTES;
    }
    resp_buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!resp_buf) {
        resp_buf = (char *)malloc(cap);
    }
    if (!resp_buf) {
        ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: No mem for response buffer (%u bytes)", (unsigned)cap);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    // Drain the body until EOF (read() returns 0) or the buffer fills. We reserve one byte for the NUL.
    while (resp_len + 1 < cap) {
        int r = esp_http_client_read(client, resp_buf + resp_len, (int)(cap - 1 - resp_len));
        if (r < 0) {
            ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: esp_http_client_read failed (r=%d)", r);
            err = ESP_FAIL;
            goto cleanup;
        }
        if (r == 0) {
            break; // EOF - server finished sending the body
        }
        resp_len += (size_t)r;
    }
    resp_buf[resp_len] = '\0';

    // Map documented xAI /v1/stt status codes to a descriptive log + return value.
    // Only 429 gets its own return code so the UI can show the "Out of credits" screen;
    // everything else bubbles up as ESP_FAIL so the generic "thinking failed" screen shows.
    if (status != 200) {
        switch (status) {
            case 400:
                // Malformed request (bad multipart framing, unsupported audio format, etc.)
                ESP_LOGE(TAG, "STT 400 Bad Request: %s", resp_buf);
                err = ESP_FAIL;
                break;
            case 401:
                // API key is missing or invalid - user must fix it in the Wi-Fi portal
                ESP_LOGE(TAG, "STT 401 Unauthorized - check your xAI API key: %s", resp_buf);
                err = ESP_FAIL;
                break;
            case 413:
                // File > 500 MB. PolyCast5 caps at ~4 min = ~8 MB, so this shouldn't fire.
                ESP_LOGE(TAG, "STT 413 Payload Too Large (>500 MB): %s", resp_buf);
                err = ESP_FAIL;
                break;
            case 429:
                // Rate limited / out of credits - surface as a distinct UI state
                ESP_LOGE(TAG, "STT 429 Rate Limited / out of credits: %s", resp_buf);
                err = AI_VOICE_ERR_RATE_LIMITED;
                break;
            case 502:
                // xAI-side upstream failure (not expected for direct file upload)
                ESP_LOGE(TAG, "STT 502 Bad Gateway: %s", resp_buf);
                err = ESP_FAIL;
                break;
            case 503:
                // xAI backend temporarily unavailable - user can retry
                ESP_LOGE(TAG, "STT 503 Service Unavailable - retry later: %s", resp_buf);
                err = ESP_FAIL;
                break;
            default:
                // Any status we don't recognize (still log body for diagnosis)
                ESP_LOGE(TAG, "STT unexpected HTTP %d: %s", status, resp_buf);
                err = ESP_FAIL;
                break;
        }
        goto cleanup;
    }

    // Parse JSON and pull out the top-level "text" field (xAI STT schema: {"text": "..."})
    cJSON *j = cJSON_ParseWithLength(resp_buf, resp_len);
    if (!j) {
        ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: cJSON_Parse failed");
        err = ESP_FAIL;
        goto cleanup;
    }

    const cJSON *text = cJSON_GetObjectItem(j, "text");
    if (!cJSON_IsString(text) || !text->valuestring) {
        ESP_LOGE(TAG, "ai_voice_stt_transcribe_pcm16_xai: response missing 'text'");
        cJSON_Delete(j);
        err = ESP_FAIL;
        goto cleanup;
    }

    // Copy transcript into caller's buffer, truncating if needed and always NUL-terminating
    strncpy(out_text, text->valuestring, out_text_sz - 1);
    out_text[out_text_sz - 1] = '\0';

    cJSON_Delete(j);

    // Treat an empty transcript as failure so downstream parsing doesn't act on "nothing said"
    err = (out_text[0] != '\0') ? ESP_OK : ESP_FAIL;

cleanup:
    // heap_caps_free handles both heap_caps_malloc and malloc pointers on ESP-IDF
    if (resp_buf) {
        heap_caps_free(resp_buf);
    }
    // Safe to close/cleanup even if open() failed - esp_http_client tracks its own state
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return err;
}
