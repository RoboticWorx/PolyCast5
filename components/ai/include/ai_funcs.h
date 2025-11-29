#ifndef AI_FUNCS_H
#define AI_FUNCS_H

#include "esp_err.h"

#define AI_RESPONSE_MAX_LEN 2048
#define AI_CMD_MAX_LEN 2048
#define AI_API_KEY_MAX_LEN 256

typedef struct {
	char cmd[AI_CMD_MAX_LEN];
} ai_cmd_t;

/** 
 * @brief Save OpenAI API key to NVS
 *
 * @param [in] api_key The API key string to save
 *
 * @returns ESP error status
 */
esp_err_t openai_save_api_key_nvs(const char *api_key);

/** 
 * @brief Save OpenAI API key to NVS
 *
 * @param [out] out Buffer to store the loaded API key
 * @param [in] out_sz Size of the output buffer
 *
 * @returns ESP error status
 */
esp_err_t openai_load_api_key_nvs(char *out, size_t out_sz);

/** 
 * @brief Send command to ChatGPT and get keyboard script response
 *
 * @param [in] command The user command string to send
 * @param [out] response_buf Buffer to store the ChatGPT response script
 * @param [in] buf_sz Size of the response buffer
 *
 * @returns ESP error status
 */
esp_err_t openai_send_command(const char *command, char *response_buf, size_t buf_sz);


#endif // AI_FUNCS_H
