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
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_websocket_client.h"

#include "driver/i2s_std.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

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

// xAI realtime WS
#define XAI_REALTIME_URI         "wss://api.x.ai/v1/realtime"
#define WS_CHUNK_SAMPLES         320 // 20ms @ 16kHz (320 samples)
#define WS_CONNECT_TIMEOUT_MS    10000
#define WS_TRANSCRIBE_TIMEOUT_MS 20000

static i2s_chan_handle_t i2s_rx_channel = NULL;
static bool voice_inited = false;

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;

    char *out;
    size_t out_sz;
    size_t out_len;
} stt_ws_ctx_t;

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

    // Create an RX channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
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
            .mclk = I2S_GPIO_UNUSED, // No master clock
            .bclk = I2S_T5848_SCK_PIN, // Bit clock
            .ws   = I2S_T5848_WS_PIN, // Word select (LR clock)
            .dout = I2S_GPIO_UNUSED, // No data out
            .din  = I2S_T5848_SD_PIN, // Data in
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
    POLYCAST5_USE_PSRAM static int32_t i2s_words[FRAMES_PER_READ * 2];
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

// Realtime WS flow:
//   1) Connect to wss: //api.x.ai/v1/realtime with Authorization: Bearer <key>
//   2) session.update:
//        - input format = audio/pcm, rate=16000
//        - turn_detection.type = null (so we manually commit)
//   3) Stream audio bytes via input_audio_buffer.append (base64)
//   4) input_audio_buffer.commit
//   5) response.create (triggers server processing)
//   6) Accumulate transcript from response.output_audio_transcript.delta
//   7) Finish on response.output_audio_transcript.done or response.done

// Clears the STT WebSocket context output buffer
static void stt_ws_clear(stt_ws_ctx_t *ctx)
{
    // Validate arguments
    if (!ctx || !ctx->out || ctx->out_sz == 0) {
        return;
    }

    // Clear ctx
    ctx->out[0] = '\0';
    ctx->out_len = 0;
}

// Appends a string to the STT WebSocket context output buffer
static void stt_ws_append(stt_ws_ctx_t *ctx, const char *s)
{
    // Validate arguments
    if (!ctx || !ctx->out || ctx->out_sz == 0 || !s) {
        return;
    }

    // Always keep NULL-terminated, never overflow
    size_t cap = ctx->out_sz - 1;
    while (*s && ctx->out_len < cap) {
        ctx->out[ctx->out_len++] = *s++;
    }
    ctx->out[ctx->out_len] = '\0';
}

// Sends a text message over the WebSocket
static esp_err_t ws_send_json(esp_websocket_client_handle_t ws, const char *json)
{
    // Validate arguments
    if (!ws || !json) {
        return ESP_ERR_INVALID_ARG;
    }

    // Send text message
    int sent = esp_websocket_client_send_text(ws, json, (int)strlen(json), portMAX_DELAY);

    return (sent > 0) ? ESP_OK : ESP_FAIL;
}

// WebSocket event handler for STT
static void ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;

    stt_ws_ctx_t *ctx = (stt_ws_ctx_t *)handler_args;
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)event_data;

    // Validate arguments
    if (!ctx) {
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_ERROR) {
        ESP_LOGW(TAG, "ws_event_handler: WebSocket disconnected or error");
        ctx->result = ESP_FAIL;
        xSemaphoreGive(ctx->done);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA) {
        return;
    }
    if (!e || !e->data_ptr || e->data_len <= 0) {
        return;
    }

    // Parse JSON server message
    cJSON *j = cJSON_ParseWithLength((const char *)e->data_ptr, e->data_len);
    if (!j) {
        return;
    }

    // Get "type" field
    const cJSON *type = cJSON_GetObjectItem(j, "type");
    const char *t = (cJSON_IsString(type) && type->valuestring) ? type->valuestring : NULL;
    if (!t) {
        cJSON_Delete(j);
        return;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "WS event type=%s", t);
#endif

    // If server sends an error object, fail immediately
    const cJSON *err = cJSON_GetObjectItem(j, "error");
    if (cJSON_IsObject(err)) {
        // Log error message if present
        const cJSON *msg = cJSON_GetObjectItem(err, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            ESP_LOGE(TAG, "ws_event_handler: WS error: %s", msg->valuestring);
        }

        // Signal failure
        ctx->result = ESP_FAIL;
        xSemaphoreGive(ctx->done);
        cJSON_Delete(j);
        return;
    }

    // Input audio transcription completed (user speech recognized)
    if (strcmp(t, "conversation.item.input_audio_transcription.completed") == 0) {
        const cJSON *tr = cJSON_GetObjectItem(j, "transcript");
        if (cJSON_IsString(tr) && tr->valuestring) {
            ESP_LOGI(TAG, "User audio transcript (debug): %s", tr->valuestring);
            // Append to ctx->out here (we only want transcript)
            stt_ws_append(ctx, tr->valuestring);
            // Signal done early (skip response generation)
            ctx->result = (ctx->out && ctx->out[0] != '\0') ? ESP_OK : ESP_FAIL;
            xSemaphoreGive(ctx->done);
        }
        cJSON_Delete(j);
        return;
    }

    // Response text from audio transcript delta (xAI uses this for response text)
    if (strcmp(t, "response.output_audio_transcript.delta") == 0) {
        const cJSON *delta = cJSON_GetObjectItem(j, "delta");
        if (cJSON_IsString(delta) && delta->valuestring) {
            stt_ws_append(ctx, delta->valuestring);
        }
    }

    // Response done events (use transcript.done as primary, fallback to response.done)
    if (strcmp(t, "response.output_audio_transcript.done") == 0 ||
        strcmp(t, "response.done") == 0) {
        ctx->result = (ctx->out && ctx->out[0] != '\0') ? ESP_OK : ESP_FAIL;
        xSemaphoreGive(ctx->done);
        cJSON_Delete(j);
        return;
    }

    cJSON_Delete(j);
}

// Sends the session.update message to configure the STT session
static esp_err_t ws_send_session_update(esp_websocket_client_handle_t ws, const char *system_prompt)
{
    if (!ws || !system_prompt) {
        return ESP_ERR_INVALID_ARG;
    }

    // Root: { "type": "session.update", "session": { ... } }
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;

    cJSON *session = NULL;
    cJSON *turn_detection = NULL;
    cJSON *audio = NULL;
    cJSON *input = NULL;
    cJSON *format = NULL;

    // root.type
    cJSON_AddStringToObject(root, "type", "session.update");

    // root.session
    session = cJSON_AddObjectToObject(root, "session");
    if (!session) goto oom;

    // session.instructions  <-- this is the important part (auto-escaped)
    cJSON_AddStringToObject(session, "instructions", system_prompt);

    // session.turn_detection = { "type": null }
    turn_detection = cJSON_AddObjectToObject(session, "turn_detection");
    if (!turn_detection) goto oom;
    cJSON_AddNullToObject(turn_detection, "type");

    // session.audio.input.format = { "type":"audio/pcm", "rate":16000 }
    audio = cJSON_AddObjectToObject(session, "audio");
    if (!audio) goto oom;

    input = cJSON_AddObjectToObject(audio, "input");
    if (!input) goto oom;

    format = cJSON_AddObjectToObject(input, "format");
    if (!format) goto oom;

    cJSON_AddStringToObject(format, "type", "audio/pcm");
    cJSON_AddNumberToObject(format, "rate", 16000);

    // Enable input transcription (get transcript without full response)
    cJSON *transcription = cJSON_AddObjectToObject(session, "input_audio_transcription");
    if (!transcription) goto oom;
    cJSON_AddStringToObject(transcription, "model", "whisper-1");  // xAI-compatible model for STT

    // session.modalities = ["text"]
    cJSON *modalities = cJSON_AddArrayToObject(session, "modalities");
    if (!modalities) goto oom;
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));

    // Serialize to compact JSON
    char *json = cJSON_PrintUnformatted(root);
    if (!json) goto oom;

    // Send over websocket
    esp_err_t err = ws_send_json(ws, json);

    // Free print buffer + JSON tree
    cJSON_free(json);
    cJSON_Delete(root);
    return err;

oom:
    cJSON_Delete(root);
    return ESP_ERR_NO_MEM;
}

// Performs transcription via xAI realtime WebSocket API
esp_err_t ai_voice_stt_ws_transcribe_pcm16_xai(const int16_t *pcm16, size_t samples, char *out_text, size_t out_text_sz)
{
    // Validate arguments
    if (!pcm16 || samples == 0 || !out_text || out_text_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    // Load xAI API key from NVS
    POLYCAST5_USE_PSRAM static char api_key[AI_API_KEY_MAX_LEN] = {0};
    memset(api_key, 0, sizeof(api_key));
    esp_err_t err = ai_utils_load_api_key_nvs(api_key, sizeof(api_key));
    if (err != ESP_OK || api_key[0] == '\0') {
        ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: ai_utils_load_api_key_nvs failed: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // Context shared with the websocket event handler
    stt_ws_ctx_t ctx = {
        .done    = xSemaphoreCreateBinary(),
        .result  = ESP_FAIL,
        .out     = out_text,
        .out_sz  = out_text_sz,
        .out_len = 0,
    };
    if (!ctx.done) {
        return ESP_ERR_NO_MEM;
    }
    stt_ws_clear(&ctx);

    // Authorization header required by xAI
    POLYCAST5_USE_PSRAM static char headers[512];
    memset(headers, 0, sizeof(headers));
    snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", api_key);

    // WebSocket client configuration
    esp_websocket_client_config_t cfg = {
        .uri = XAI_REALTIME_URI,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach, // Validates server cert
        .buffer_size = 4096, // Increased to handle larger headers
        .network_timeout_ms = 30000, // Network ops timeout
        .disable_auto_reconnect = true, // No auto-reconnect
    };

    // Create websocket client
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) {
        ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: esp_websocket_client_init failed");
        vSemaphoreDelete(ctx.done);
        return ESP_FAIL;
    }

    // Attach handler
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, &ctx);

    // Start the websocket client task
    err = esp_websocket_client_start(ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: esp_websocket_client_start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(ws);
        vSemaphoreDelete(ctx.done);
        return err;
    }

    // Wait until connected or time out
    int64_t t0 = esp_timer_get_time();
    while (!esp_websocket_client_is_connected(ws)) {
        vTaskDelay(pdMS_TO_TICKS(20));

        // Check for connect timeout
        if ((esp_timer_get_time() - t0) > (int64_t)WS_CONNECT_TIMEOUT_MS * 1000LL) {
            ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: WS connect timeout");
            esp_websocket_client_stop(ws);
            esp_websocket_client_destroy(ws);
            vSemaphoreDelete(ctx.done);
            return ESP_FAIL;
        }
    }

    // Use cJSON to build session_update to handle newline, etc.
    err = ws_send_session_update(ws, "Transcribe the user's speech input to text.");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: ws_send_session_update failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // Stream PCM in small chunks: Each chunk is base64 of raw PCM16 bytes (little-endian)
    size_t idx = 0;
    while (idx < samples) {
        size_t n = samples - idx;

        // Limit to WS_CHUNK_SAMPLES
        if (n > WS_CHUNK_SAMPLES) {
            n = WS_CHUNK_SAMPLES;
        }

        const uint8_t *raw = (const uint8_t *)&pcm16[idx];
        size_t raw_len = n * sizeof(int16_t);

        // Base64 output capacity: 4 * ceil(n / 3) + 1
        size_t b64_cap = 4 * ((raw_len + 2) / 3) + 1;
        char *b64 = (char *)malloc(b64_cap);
        if (!b64) {
            ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: No mem for base64 buffer (%u bytes)", (unsigned)b64_cap);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        // Base64 encode
        size_t olen = 0;
        int mbed = mbedtls_base64_encode((unsigned char *)b64, b64_cap, &olen, (const unsigned char *)raw, raw_len);
        if (mbed != 0) {
            free(b64);
            err = ESP_FAIL;
            goto cleanup;
        }
        b64[olen] = '\0';

        // Build JSON message: {"type":"input_audio_buffer.append","audio":"..."}
        size_t msg_cap = olen + 64;
        char *msg = (char *)malloc(msg_cap);
        if (!msg) {
            free(b64);
            ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: No mem for WS message (%u bytes)", (unsigned)msg_cap);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        snprintf(msg, msg_cap, "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);

        free(b64);

        // Send audio chunk
        err = ws_send_json(ws, msg);

        // Free message buffer
        free(msg);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: ws_send_json(audio chunk) failed: %s", esp_err_to_name(err));
            goto cleanup;
        }

        idx += n;

        // Tiny yield to keep the system responsive
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Commit the audio buffer (because turn_detection.type=null)
    err = ws_send_json(ws, "{\"type\":\"input_audio_buffer.commit\"}");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: ws_send_json(input_audio_buffer.commit) failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    // Skip triggering processing / response generation (only get transcript)
    // err = ws_send_json(ws, "{\"type\":\"response.create\",\"response\":{\"modalities\":[\"text\"]}}");
    // if (err != ESP_OK) {
    //     ESP_LOGE(TAG, "ai_voice_stt_ws_send_pcm16_xai: ws_send_json(response.create) failed: %s", esp_err_to_name(err));
    //     goto cleanup;
    // }

    // Wait for "done" event from the handler
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(WS_TRANSCRIBE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "STT timeout waiting transcript");
        err = ESP_FAIL;
        goto cleanup;
    }

    err = ctx.result;

cleanup:
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    vSemaphoreDelete(ctx.done);
    return err;
}
