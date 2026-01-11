#ifndef AI_UTILS_H
#define AI_UTILS_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// NVS keys for xAI (Grok) API key
#define XAI_NS "xai"
#define XAI_KEY "api_key"

#define AI_RESPONSE_MAX_LEN (1024 * 64)
#define AI_CMD_MAX_LEN (1024 * 256) // Large to send raw frames
#define AI_API_KEY_MAX_LEN 256

typedef enum {
    AI_CMD_NORMAL = 0,
    AI_CMD_CRED_USERNAME,
    AI_CMD_CRED_PASSWORD,
    AI_CMD_RAW_FRAMES,
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
 * @brief Send command to xAI Grok and get autokey script response
 *
 * @param [in] system_prompt The system prompt string to use
 * @param [in] command The user command string to send
 * @param [out] response_buf Buffer to store the Grok response script
 * @param [in] buf_sz Size of the response buffer
 * @param [in] reasoning True to use reasoning model, false for non-reasoning
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_send_command_xai(const char *system_prompt, const char *command, char *response_buf, size_t buf_sz, bool reasoning);

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
 * @brief Save AI prompt override to NVS (empty string => use compiled default)
 *
 * @param [in] prompt Prompt text to save
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_prompt_save_nvs(const char *prompt);

/**
 * @brief Load AI prompt override from NVS
 *
 * @param [out] out Buffer to store the loaded prompt
 * @param [in] out_sz Size of the output buffer
 *
 * @returns ESP error status
 */
esp_err_t ai_utils_prompt_load_nvs(char *out, size_t out_sz);

/** 
 * @brief Get the autotype prompt, either from NVS override or compiled default
 *
 * @param [out] buf Buffer to store the prompt (if provided)
 * @param [in] buf_sz Size of the buffer
 *
 * @returns Pointer to the prompt string (either buf or compiled default)
 */
const char *ai_utils_get_autokey_prompt(char *buf, size_t buf_sz);

#endif // AI_UTILS_H
