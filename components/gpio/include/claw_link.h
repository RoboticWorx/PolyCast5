#ifndef CLAW_LINK_H
#define CLAW_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "claw_link_proto.h"

// Status snapshot read back from the Claw expansion
typedef struct {
    uint8_t state; // CLAW_LINK_STATE_*
    uint8_t flags; // CLAW_LINK_FLAG_* bitmask
    uint8_t seq; // Bumped by the expansion whenever its display text changes
    uint16_t total_len; // Total length of the display text across all pages
    char text[CLAW_LINK_FRAME_TEXT_MAX + 1]; // First page of the display text
} claw_link_status_t;

/**
 * @brief Check whether the PolyCast5-Claw expansion is present on the I2C bus
 *
 * @returns ESP_OK if the expansion ACKed its address
 */
esp_err_t claw_link_probe(void);

/**
 * @brief Hand the host's Wi-Fi credentials to the expansion so it joins the same network
 *
 * @param [in] ssid Network SSID
 * @param [in] password Network password (may be empty for an open network)
 *
 * @returns ESP error status
 */
esp_err_t claw_link_send_wifi(const char *ssid, const char *password);

/**
 * @brief Hand the host's LLM credentials to the expansion so it needs no setup of its own
 *
 * @param [in] api_key API key to authenticate with
 * @param [in] model Model name to run
 * @param [in] base_url API base URL
 *
 * @returns ESP error status
 */
esp_err_t claw_link_send_llm(const char *api_key, const char *model, const char *base_url);

/**
 * @brief Send a natural language command for the expansion's AI agent to carry out
 *
 * Text longer than CLAW_LINK_PAYLOAD_MAX is truncated.
 *
 * @param [in] text Command text
 *
 * @returns ESP error status
 */
esp_err_t claw_link_send_command(const char *text);

/**
 * @brief Read the expansion's current state, flags and first page of display text
 *
 * @param [out] out Status snapshot
 *
 * @returns ESP error status
 */
esp_err_t claw_link_poll(claw_link_status_t *out);

/**
 * @brief Assemble the expansion's full display text across every page
 *
 * @param [out] out Buffer to store the text in
 * @param [in] out_sz Size of the output buffer
 *
 * @returns ESP error status
 */
esp_err_t claw_link_read_result(char *out, size_t out_sz);

/**
 * @brief Ask the expansion to cancel whatever it is working on
 *
 * @returns ESP error status
 */
esp_err_t claw_link_abort(void);

/**
 * @brief Get the last command sent to the expansion (for the UI to echo back)
 *
 * @returns Pointer to the stored command, empty string if nothing has been sent
 */
const char *claw_link_last_command(void);

#endif // CLAW_LINK_H
