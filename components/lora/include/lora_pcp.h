// Behold, PolyCast5's very own LoRa protocol: the Poly Cipher Protocol (PCP)!
// ┌────────────────────────────────────────────────────────────────────────┐
// │       Poly Cipher Protocol (PCP) — AES-128-CCM, Explicit Header        │
// ├────────────────────────────────────────────────────────────────────────┤
// │                     Command Packet (55 bytes on air)                   │
// ├─────────────┬─────────────────────────────────────────────┬────────────┤
// │ Nonce (13B) │           AES-CCM Ciphertext (38B)          │  MIC (4B)  │
// │  Random     ├──────┬──────────┬───────┬───────────────────┤  Auth Tag  │
// │             │ Type │  Msg ID  │ Index │       Instr       │            │
// │             │  1B  │    4B    │  1B   │        32B        │            │
// │             │ 0x01 │  uint32  │ uint8 │      char[32]     │            │
// ├─────────────┴──────┴──────────┴───────┴───────────────────┴────────────┤
// │                      ACK Packet (22 bytes on air)                      │
// ├─────────────┬─────────────────────────────────────────────┬────────────┤
// │ Nonce (13B) │           AES-CCM Ciphertext (5B)           │  MIC (4B)  │
// │  Random     ├──────┬──────────────────────────────────────┤  Auth Tag  │
// │             │ Type │                Msg ID                │            │
// │             │  1B  │                  4B                  │            │
// │             │ 0x02 │                uint32                │            │
// └─────────────┴──────┴──────────────────────────────────────┴────────────┘

#ifndef LORA_PCP_H
#define LORA_PCP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#define LORA_PCP_NONCE_LENGTH  13
#define LORA_PCP_MIC_LENGTH    4
#define LORA_PCP_ENC_KEY_LEN   16
#define LORA_PCP_INSTR_MAX_LEN 32

// User-selectable LoRa spreading factor range
#define LORA_PCP_SF_MIN     7  // SX126X_LORA_SF7
#define LORA_PCP_SF_MAX     12 // SX126X_LORA_SF12
#define LORA_PCP_SF_DEFAULT 7  // SX126X_LORA_SF7

// User-selectable LoRa region: picks the RF band that matches the attached antenna
// The PCP carrier and SX1262 image-calibration band both follow this selection
typedef enum {
    LORA_REGION_US = 0, // US 902-928 MHz ISM band
    LORA_REGION_EU = 1, // EU 863-870 MHz ISM band (ETSI EN 300 220)
    LORA_REGION_COUNT   // Not a region; count for range clamping
} lora_region_t;

#define LORA_REGION_DEFAULT LORA_REGION_US // Preserves the original hardcoded 915 MHz behavior

// PCP carrier frequency per region
#define LORA_PCP_FREQ_US_HZ 915000000UL // US 915 MHz band center
// EU: the 869.4-869.65 MHz high-power sub-band (ETSI allows 500 mW / 27 dBm, 10% duty), so PCP's 22 dBm TX stays legal
// BW125 at 869.5 spans 869.4375-869.5625, inside the sub-band
#define LORA_PCP_FREQ_EU_HZ 869500000UL

// Binary wire protocol
#define LORA_PCP_COMMAND 0x01
#define LORA_PCP_ACK     0x02

// Command message (38 bytes plaintext, 38 bytes ciphertext - no padding with CCM)
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t msg_id;
    uint8_t  index;
    char     instr[LORA_PCP_INSTR_MAX_LEN];
} lora_pcp_cmd_msg_t;

// ACK message (5 bytes plaintext, 5 bytes ciphertext - no padding with CCM)
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint32_t msg_id;
} lora_pcp_ack_msg_t;

// Ciphertext sizes per message type (plaintext size, no block padding)
#define LORA_PCP_CMD_CIPHERTEXT_LEN (sizeof(lora_pcp_cmd_msg_t)) // 38
#define LORA_PCP_ACK_CIPHERTEXT_LEN (sizeof(lora_pcp_ack_msg_t)) // 5

// Max ciphertext size (command message)
#define LORA_PCP_CIPHERTEXT_LENGTH (LORA_PCP_CMD_CIPHERTEXT_LEN)

// Max on-air payload: nonce + largest ciphertext + MIC
#define LORA_PCP_PAYLOAD_LENGTH (LORA_PCP_NONCE_LENGTH + LORA_PCP_CIPHERTEXT_LENGTH + LORA_PCP_MIC_LENGTH)

typedef struct {
    char instr[LORA_PCP_INSTR_MAX_LEN];
    uint8_t key[LORA_PCP_ENC_KEY_LEN];
    int index;
} lora_pcp_cmd_t;

extern volatile uint32_t expected_rx_id;
extern volatile bool waiting_for_ack;

/**
 * @brief Loads persisted PCP msg_id counter from NVS
 */
void lora_pcp_load_msg_id_nvs(void);

/**
 * @brief Load the persisted LoRa spreading factor from NVS
 *
 * @returns The stored SF as a plain numeric value (LORA_PCP_SF_MIN..LORA_PCP_SF_MAX).
 *          Returns LORA_PCP_SF_DEFAULT when no value is stored or the stored value
 *          is out of range, so callers always receive a radio-safe SF.
 */
uint8_t lora_pcp_load_sf_nvs(void);

/**
 * @brief Persist the LoRa spreading factor to NVS
 *
 * @param [in] sf Spreading factor as a plain numeric value; clamped to
 *                LORA_PCP_SF_MIN..LORA_PCP_SF_MAX before it is stored.
 *
 * @returns ESP_OK on success, otherwise the failing NVS error code.
 */
esp_err_t lora_pcp_save_sf_nvs(uint8_t sf);

/**
 * @brief Load the persisted LoRa region from NVS
 *
 * @returns The stored region (LORA_REGION_US or LORA_REGION_EU). Returns
 *          LORA_REGION_DEFAULT when no value is stored or the stored value is
 *          out of range, so callers always receive a valid region.
 */
lora_region_t lora_pcp_load_region_nvs(void);

/**
 * @brief Persist the LoRa region to NVS
 *
 * @param [in] region Region to store; clamped to a valid lora_region_t before
 *                    it is written.
 *
 * @returns ESP_OK on success, otherwise the failing NVS error code.
 */
esp_err_t lora_pcp_save_region_nvs(lora_region_t region);

/**
 * @brief Set the PCP encryption key
 *
 * @param [in] key Pointer to LORA_PCP_ENC_KEY_LEN bytes
 */
void lora_pcp_set_key(const uint8_t *key);

/**
 * @brief Generate and set a random encryption key, then queue it for ESP-NOW distribution
 */
void lora_pcp_generate_random_key(void);

/**
 * @brief Create a monotonically increasing message ID for replay protection
 *
 * @returns The next msg_id
 */
uint32_t lora_pcp_create_msg_id(void);

/**
 * @brief Process a received LoRa message (CCM auth-decrypt, handle ACK)
 *
 * @param [in] message The raw message received (Nonce + ciphertext + MIC)
 * @param [in] message_len Length of the message received
 */
void lora_pcp_process_received_message(uint8_t *message, size_t message_len);

/**
 * @brief Encrypt plaintext with AES-128-CCM and transmit over LoRa
 *
 * @param [in] plaintext Data to encrypt and transmit
 * @param [in] plaintext_len Length of the plaintext data
 *
 * @returns True on success
 */
bool lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len);

#endif // LORA_PCP_H
