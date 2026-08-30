#ifndef AI_UTILS_H
#define AI_UTILS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// NVS keys for xAI (Grok) API key
#define XAI_NS "xai"
#define XAI_KEY "api_key"

#define AI_PROMPT_NVS_MAX_LEN (1024 * 8)

// This is also the size of all BT autotype script send buffers!
// Soft cap for AI responses is now max_tokens in xai_stream
#define AI_RESPONSE_MAX_LEN (1024 * 64)

#define AI_USER_TRANSCRIPT_MAX_LEN (AI_RESPONSE_MAX_LEN)
#define AI_CMD_MAX_LEN (1024 * 256) // Large to send raw frames
#define AI_API_KEY_MAX_LEN 256

typedef enum {
    AI_CMD_NONE = 0,
    AI_CMD_KEYBOARD_START_REC,
    AI_CMD_KEYBOARD_DONE_REC,
    AI_CMD_CRED_USERNAME,
    AI_CMD_CRED_PASSWORD,
    AI_CMD_CUSTOM,
    AI_CMD_DICTATE,
    AI_CMD_RAW_FRAMES,
    AI_CMD_KEYBOARD_ABORT_REC, // Page exited mid-recording: deinit mic + free capture, no STT
} ai_cmd_type_t;

typedef struct {
    //bool thinking; // True = reasoning, false = non-reasoning model
    ai_cmd_type_t type;
    const char *msg;
    size_t msg_len;

    // If free_on_done is true, ai_task will free(free_ptr) after processing
    void *free_ptr;
    bool free_on_done;

    // True = use reasoning model, false = non-reasoning
    bool reasoning;
} ai_cmd_t;

/** 
 * @brief Save xAI API key to NVS
 *
 * @param [in] api_key The API key string to save
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_save_api_key_nvs(const char *api_key);

/** 
 * @brief Save xAI API key to NVS
 *
 * @param [out] out Buffer to store the loaded API key
 * @param [in] out_sz Size of the output buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_load_api_key_nvs(char *out, size_t out_sz);

/**
 * @brief Whether the selected AI provider is configured and usable right now.
 *
 * True when the chat provider resolves (custom endpoints have a URL) AND either the provider is
 * keyless (local) or a usable API key stamped for that provider is stored. Use this to decide
 * whether to enter an AI feature or route the user to the web config portal - it correctly
 * handles keyless local providers (which have no stored key) and erased keys.
 *
 * @returns true if AI features can run, false if the user should be sent to the config page.
 */
bool ai_config_is_ready(void);

/**
 * @brief Whether voice dictation (speech-to-text) can actually run right now.
 *
 * Stricter than ai_config_is_ready(): in addition to a usable chat provider, this requires a
 * resolvable STT endpoint AND a usable STT key, mirroring the key-selection rules in
 * ai_voice_stt_transcribe_pcm16_xai(). Use this to gate the voice-only AI keyboard page so a
 * chat-only provider (has_stt=false) with no separate STT configured routes the user to the
 * config portal instead of into a page where every dictation attempt fails.
 *
 * @returns true if voice input can run, false if the user should be sent to the config page.
 */
bool ai_config_is_ready_for_voice(void);

/** 
 * @brief Send command to the selected AI chat provider and get autokey script response
 *
 * Endpoint, model, and reasoning behavior come from the provider registry via
 * ai_provider_resolve_chat() (xAI by default; OpenAI/Groq/DeepSeek/OpenRouter/custom).
 *
 * @param [in] system_prompt The system prompt string to use
 * @param [in] command The user command string to send
 * @param [out] response_buf Buffer to store the model response script
 * @param [in] buf_sz Size of the response buffer
 * @param [in] reasoning True to use reasoning model, false for non-reasoning
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_send_command_xai(const char *system_prompt, const char *command, char *response_buf, size_t buf_sz, bool reasoning);

/**
 * @brief Callback invoked for each content delta during SSE streaming
 *
 * @param [in] delta_text The partial text chunk from the model
 * @param [in] user_ctx Opaque pointer passed through from the caller
 *
 * @returns ESP_OK to continue streaming, any other value to abort
 */
typedef esp_err_t (*ai_stream_cb_t)(const char *delta_text, void *user_ctx);

/**
 * @brief Send command to the selected AI chat provider with SSE streaming, invoking a callback for each content delta
 *
 * The full assembled response is also written into response_buf (same as the non-streaming variant).
 * If on_delta is NULL this behaves identically to ai_utils_send_command_xai().
 *
 * @param [in] system_prompt System prompt string
 * @param [in] command User command string
 * @param [out] response_buf Buffer to accumulate the full response
 * @param [in] buf_sz Size of response_buf
 * @param [in] reasoning True for reasoning model, false for non-reasoning
 * @param [in] on_delta Callback for each streaming content delta (may be NULL)
 * @param [in] user_ctx Opaque pointer forwarded to on_delta
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_send_command_xai_stream(const char *system_prompt, const char *command, char *response_buf, size_t buf_sz,
        bool reasoning, ai_stream_cb_t on_delta, void *user_ctx);

/** 
 * @brief Lookup credentials via AI and get the corresponding Bluetooth script
 *
 * @param [in] type Type of credential to look up (username or password)
 * @param [in] query The query string to find matching credentials
 * @param [out] out_script Buffer to store the resulting Bluetooth script
 * @param [in] out_sz Size of the output script buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_lookup_creds(ai_cmd_type_t type, const char *query, char *out_script, size_t out_sz);

/**
 * @brief Lookup a custom command via AI from scripts in the "Custom" category
 *
 * @param [in] query The user's description of what they want to do
 * @param [out] out_script Buffer to store the resulting Bluetooth script
 * @param [in] out_sz Size of the output script buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_lookup_custom(const char *query, char *out_script, size_t out_sz);

/**
 * @brief Save AI keyboard prompt override to NVS (empty string => use compiled default)
 *
 * @param [in] prompt Prompt text to save
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_keyboard_prompt_save_nvs(const char *prompt);

/**
 * @brief Load AI keyboard prompt override from NVS
 *
 * @param [out] out Buffer to store the loaded prompt
 * @param [in] out_sz Size of the output buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_keyboard_prompt_load_nvs(char *out, size_t out_sz);

/**
 * @brief Save AI packet analysis prompt override to NVS (empty string => use compiled default)
 *
 * @param [in] prompt Prompt text to save
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_pkt_analysis_prompt_save_nvs(const char *prompt);

/**
 * @brief Load AI packet analysis prompt override from NVS
 *
 * @param [out] out Buffer to store the loaded prompt
 * @param [in] out_sz Size of the output buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_pkt_analysis_prompt_load_nvs(char *out, size_t out_sz);

/** 
 * @brief Get the autotype prompt, either from NVS override or compiled default
 *
 * @param [out] buf Buffer to store the prompt (if provided)
 * @param [in] buf_sz Size of the buffer
 *
 * @returns Pointer to the prompt string (either buf or compiled default)
 */
const char *ai_utils_get_autokey_prompt(char *buf, size_t buf_sz);

/** 
 * @brief Get the packet analysis prompt, either from NVS override or compiled default
 *
 * @param [out] buf Buffer to store the prompt (if provided)
 * @param [in] buf_sz Size of the buffer
 *
 * @returns Pointer to the prompt string (either buf or compiled default)
 */
const char *ai_utils_get_pkt_analysis_prompt(char *buf, size_t buf_sz);

#endif // AI_UTILS_H
