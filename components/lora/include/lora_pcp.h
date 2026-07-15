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
// (KYOCERA AVX M620720, 863-928 MHz). The PCP carrier, Meshtastic LongFast slot, and
// SX1262 image-calibration band all follow this selection (see region table in lora_pcp.c).
// APPEND ONLY: the raw enum byte rides the ESP-NOW key-sync frame to PolyPlugs, so
// existing values must never be renumbered.
typedef enum {
    LORA_REGION_US  = 0,  // US 902-928 MHz ISM band
    LORA_REGION_EU  = 1,  // EU 863-870 MHz ISM band (ETSI EN 300 220)
    LORA_REGION_ANZ = 2,  // Australia/New Zealand 915-928 MHz
    LORA_REGION_IN  = 3,  // India 865-867 MHz
    LORA_REGION_KR  = 4,  // South Korea 920-923 MHz
    LORA_REGION_JP  = 5,  // Japan 920.5-923.5 MHz (ARIB STD-T108)
    LORA_REGION_TW  = 6,  // Taiwan 920-925 MHz
    LORA_REGION_RU  = 7,  // Russia 868.7-869.2 MHz
    LORA_REGION_TH  = 8,  // Thailand 920-925 MHz
    LORA_REGION_SG  = 9,  // Singapore 917-925 MHz (Meshtastic SG_923)
    LORA_REGION_MY  = 10, // Malaysia 919-924 MHz (Meshtastic MY_919)
    LORA_REGION_COUNT     // Not a region; count for range clamping
} lora_region_t;

#define LORA_REGION_DEFAULT LORA_REGION_US // Preserves the original hardcoded 915 MHz behavior

/**
 * @brief Per-region radio parameters: everything the region selection controls.
 *
 * One row per lora_region_t (table in lora_pcp.c). Single source of truth for the
 * PCP carrier, the Meshtastic LongFast frequency slot, the SX1262 image-calibration
 * band, and the LCD region-page display strings.
 */
typedef struct {
    const char *name;            // Short region code for UI/logs, e.g. "US", "ANZ"
    const char *full_name;       // Full region name for the LCD region page, e.g. "United States"
    uint32_t    pcp_freq_hz;     // PCP carrier (band center; BW125 must fit the band)
    uint32_t    mesh_freq_hz;    // Meshtastic LongFast slot for this region
    uint16_t    cal_img_mhz_min; // SX1262 image-calibration band, MHz (datasheet standard bands)
    uint16_t    cal_img_mhz_max;
    const char *ui_freq;         // LCD region page: PCP carrier label, e.g. "915 MHz"
    const char *ui_band;         // LCD region page: legal band label, e.g. "902-928 MHz"
} lora_region_params_t;

/**
 * @brief Get the radio parameters for a region
 *
 * @param [in] region Region to look up; out-of-range values are clamped to
 *                    LORA_REGION_DEFAULT so callers always get a valid row.
 *
 * @returns Pointer to the static const parameter row (never NULL)
 */
const lora_region_params_t *lora_region_get_params(lora_region_t region);

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
 * @returns The stored region (any valid lora_region_t). Returns
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
