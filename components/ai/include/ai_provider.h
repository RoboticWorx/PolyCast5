#ifndef AI_PROVIDER_H
#define AI_PROVIDER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NVS namespace + keys for AI provider selection/config (all key strings <=15 chars).
// The primary/chat API key stays in the existing "xai"/"api_key" slot (ai_utils.h) for
// backward-compat; everything else lives here.
#define AI_CFG_NS          "ai_cfg"
#define AI_CFG_CHAT_PROV   "chat_prov"  // u8 provider index
#define AI_CFG_CHAT_MODEL  "chat_model" // str model override (empty => preset default)
#define AI_CFG_CUST_URL    "cust_url"   // str full chat-completions URL (custom provider)
#define AI_CFG_STT_SEP     "stt_sep"    // u8 bool: 1 => use a separate STT provider + key
#define AI_CFG_STT_PROV    "stt_prov"   // u8 STT provider index (only when stt_sep=1)
#define AI_CFG_STT_MODEL   "stt_model"  // str STT model override (only when stt_sep=1)
#define AI_CFG_STT_KEY     "stt_key"    // str separate STT API key (only when stt_sep=1)
#define AI_CFG_KEY_PROV    "key_prov"   // u8: provider the primary (xai/api_key) key was saved for
#define AI_CFG_SKEY_PROV   "skey_prov"  // u8: provider the separate STT key was saved for

// Max lengths for user-entered strings
#define AI_MODEL_MAX_LEN 96
#define AI_URL_MAX_LEN   256

// Provider registry indices
// APPEND ONLY: the raw enum byte is persisted to NVS, so existing values must never be renumbered
typedef enum {
    AI_PROVIDER_XAI        = 0,
    AI_PROVIDER_OPENAI     = 1,
    AI_PROVIDER_GROQ       = 2,
    AI_PROVIDER_DEEPSEEK   = 3,
    AI_PROVIDER_OPENROUTER = 4,
    AI_PROVIDER_CUSTOM     = 5,
    AI_PROVIDER_COUNT            // Not a provider; count for range clamping
} ai_provider_id_t;

#define AI_PROVIDER_DEFAULT AI_PROVIDER_XAI // Preserves the original hardcoded xAI behavior

// One row per ai_provider_id_t (table in ai_provider.c)
// All targets speak the OpenAI Chat-Completions schema, so only URL/model/key/quirks vary
typedef struct {
    const char *id;              // Stable slug for logs, e.g. "xai"
    const char *display;         // UI name, e.g. "xAI (Grok)"
    const char *chat_url;        // Chat endpoint ("" for custom => user-entered cust_url)
    const char *chat_model_def;  // Default chat model (editable in UI)
    bool        send_reasoning_effort; // false => omit the field (non-reasoning models / servers that reject it)
    const char *reasoning_on;    // reasoning_effort value when cmd.reasoning==true
    const char *reasoning_off;   // reasoning_effort value when cmd.reasoning==false
    bool        key_required;    // false for custom/local (keyless allowed)
    bool        has_stt;         // Provider also offers STT under the same key
    const char *stt_url;         // Audio/STT endpoint (only if has_stt)
    const char *stt_model_def;   // Default STT model (only if has_stt); unused when stt_send_model is false
    const char *stt_fmt_field;   // Format form field: "response_format" (OpenAI/Groq) vs "format" (xAI)
    bool        stt_send_model;  // Send a "model" multipart part? OpenAI/Groq Whisper need it; xAI /v1/stt has no model field
    const char *stt_fmt_value;   // Value for stt_fmt_field: "json" (OpenAI/Groq) vs "true" (xAI format => Inverse Text Normalization, e.g. "$100")
} ai_provider_t;

/**
 * @brief Number of registry rows (== AI_PROVIDER_COUNT)
 *
 * @returns Count of providers in the registry
 */
size_t ai_provider_count(void);

/**
 * @brief Get the registry row for a provider index
 *
 * @param [in] idx Provider index (clamped to a valid provider)
 *
 * @returns Pointer to the registry row (never NULL)
 */
const ai_provider_t *ai_provider_get(int idx);

// Raw stored selection + overrides (missing keys fall back to defaults)
typedef struct {
    uint8_t chat_prov;
    char    chat_model[AI_MODEL_MAX_LEN];
    char    cust_url[AI_URL_MAX_LEN];
    uint8_t stt_sep;
    uint8_t stt_prov;
    char    stt_model[AI_MODEL_MAX_LEN];
} ai_provider_cfg_t;

/**
 * @brief Load the raw config from NVS (fills defaults on any missing key)
 *
 * @param [out] out Struct to fill with the stored selection + overrides
 *
 * @returns ESP error status
 */
esp_err_t ai_provider_load_config_nvs(ai_provider_cfg_t *out);

/**
 * @brief Persist the raw config to NVS (provider indices are clamped)
 *
 * @param [in] in Config to save
 *
 * @returns ESP error status
 */
esp_err_t ai_provider_save_config_nvs(const ai_provider_cfg_t *in);

/**
 * @brief Save the separate STT API key (NVS ai_cfg/stt_key)
 *
 * @param [in] key The STT API key string to save
 *
 * @returns ESP error status
 */
esp_err_t ai_provider_save_stt_key_nvs(const char *key);

/**
 * @brief Load the separate STT API key (NVS ai_cfg/stt_key)
 *
 * @param [out] out Buffer to store the loaded STT key
 * @param [in] out_sz Size of the output buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_provider_load_stt_key_nvs(char *out, size_t out_sz);

/**
 * @brief Stamp which provider the primary (xai/api_key) key belongs to
 *
 * Ensures a stored key is never attached to a different provider's endpoint. Write the
 * stamp only when the key value itself is written.
 *
 * @param [in] prov Provider index the primary key was saved for
 *
 * @returns ESP error status
 */
esp_err_t ai_provider_save_key_provider_nvs(uint8_t prov);

/**
 * @brief Stamp which provider the separate STT key belongs to
 *
 * Ensures a stored key is never attached to a different provider's endpoint. Write the
 * stamp only when the key value itself is written.
 *
 * @param [in] prov Provider index the separate STT key was saved for
 *
 * @returns ESP error status
 */
esp_err_t ai_provider_save_stt_key_provider_nvs(uint8_t prov);

/**
 * @brief Provider the primary key was saved for
 *
 * @returns Provider index; AI_PROVIDER_XAI if unstamped (legacy)
 */
uint8_t ai_provider_load_key_provider_nvs(void);

/**
 * @brief Provider the separate STT key was saved for
 *
 * @returns Provider index; AI_PROVIDER_XAI if unstamped
 */
uint8_t ai_provider_load_stt_key_provider_nvs(void);

// Resolved chat config: exactly what the request builders need.
typedef struct {
    char        url[AI_URL_MAX_LEN];
    char        model[AI_MODEL_MAX_LEN];
    bool        send_reasoning_effort;
    const char *reasoning_on;   // Points into the const registry (static lifetime)
    const char *reasoning_off;
    bool        key_required;
    uint8_t     provider_idx;   // Provider this request targets (for key<->provider binding)
} ai_chat_cfg_t;

/**
 * @brief Resolve the effective chat endpoint/model/reasoning from the NVS selection
 *
 * @param [out] out Resolved chat config the request builders need
 *
 * @returns ESP_ERR_INVALID_STATE if provider=custom but no custom URL is configured, else ESP_OK
 */
esp_err_t ai_provider_resolve_chat(ai_chat_cfg_t *out);

// Resolved STT config.
typedef struct {
    char        url[AI_URL_MAX_LEN];
    char        model[AI_MODEL_MAX_LEN];
    const char *fmt_field;        // Points into the const registry (static lifetime)
    const char *fmt_value;        // Value to send for fmt_field ("json" or xAI's boolean "true"); const registry lifetime
    bool        send_model;       // Whether the request includes a "model" part (false for xAI /v1/stt)
    bool        use_separate_key; // true => load key via ai_provider_load_stt_key_nvs, else the primary key
    bool        primary_key_ok;   // true => STT provider == chat provider, so the primary key is a valid fallback
    bool        available;        // false => selected chat provider has no STT and no separate STT is set
    uint8_t     provider_idx;     // Provider the STT request targets (for key<->provider binding)
} ai_stt_cfg_t;

/**
 * @brief Resolve the effective STT endpoint/model/field + which key to use
 *
 * @param [out] out Resolved STT config (check out->available before use)
 *
 * @returns ESP_ERR_INVALID_STATE (and sets out->available=false) when no STT is configured, else ESP_OK
 */
esp_err_t ai_provider_resolve_stt(ai_stt_cfg_t *out);

#endif // AI_PROVIDER_H
