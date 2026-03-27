//    Behold, PolyCast5's very own LoRa protocol: the Poly Cipher Protocol (PCP)!
// ┌──────────────────────────────────────────────────────────────────────────────┐
// │         Poly Cipher Protocol (PCP) — AES-128-CBC, Explicit Header            │
// ├──────────────────────────────────────────────────────────────────────────────┤
// │                      Command Packet (64 bytes on air)                        │
// ├────────────────┬─────────────────────────────────────────────────────────────┤
// │   IV (16B)     │               AES-CBC Ciphertext (48B)                      │
// │   Random IV    ├────────┬──────┬──────────┬───────┬────────────┬─────────────┤
// │                │ Magic  │ Type │  Msg ID  │ Index │   Instr    │  Zero Pad   │
// │                │   2B   │  1B  │    4B    │  1B   │    32B     │     8B      │
// │                │ 0x5043 │ 0x01 │ uint32   │ uint8 │  char[32]  │  (AES pad)  │
// ├────────────────┴────────┴──────┴──────────┴───────┴────────────┴─────────────┤
// │                        ACK Packet (32 bytes on air)                          │
// ├────────────────┬─────────────────────────────────────────────────────────────┤
// │   IV (16B)     │               AES-CBC Ciphertext (16B)                      │
// │   Random IV    ├────────┬──────┬──────────┬──────────────────────────────────┤
// │                │ Magic  │ Type │  Msg ID  │           Zero Pad               │
// │                │   2B   │  1B  │    4B    │              9B                  │
// │                │ 0x5043 │ 0x02 │  uint32  │           (AES pad)              │
// └────────────────┴────────┴──────┴──────────┴──────────────────────────────────┘

#ifndef LORA_PCP_H
#define LORA_PCP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define LORA_PCP_IV_LENGTH     16
#define LORA_PCP_ENC_KEY_LEN   16
#define LORA_PCP_MAX_INSTR_LEN 32

// Binary wire protocol
#define LORA_PCP_MAGIC       0x5043 // "PC" - brute-force guard
#define LORA_PCP_MSG_COMMAND 0x01
#define LORA_PCP_MSG_ACK     0x02

// Command message (40 bytes, padded to 48 for AES-CBC)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;
    uint32_t msg_id;
    uint8_t  index;
    char     instr[LORA_PCP_MAX_INSTR_LEN];
} lora_pcp_cmd_msg_t;

// ACK message (7 bytes, padded to 16 for AES-CBC)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;
    uint32_t msg_id;
} lora_pcp_ack_msg_t;

// Ciphertext sizes per message type (multiple of 16)
#define LORA_PCP_CMD_CIPHERTEXT_LEN 48
#define LORA_PCP_ACK_CIPHERTEXT_LEN 16

// Max ciphertext size (command message)
#define LORA_PCP_CIPHERTEXT_LENGTH (LORA_PCP_CMD_CIPHERTEXT_LEN)
#define LORA_PCP_PAYLOAD_LENGTH    (LORA_PCP_CIPHERTEXT_LENGTH + LORA_PCP_IV_LENGTH)

typedef struct {
    char instr[LORA_PCP_MAX_INSTR_LEN];
    uint8_t key[LORA_PCP_ENC_KEY_LEN];
    int index;
} lora_pcp_cmd_t;

extern uint32_t expected_rx_id;
extern bool waiting_for_ack;

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
 * @brief Generate a random non-zero message ID for receipt confirmation
 *
 * @returns The ID created
 */
uint32_t lora_pcp_create_msg_id(void);

/**
 * @brief Process a received LoRa message (decrypt, validate magic, handle ACK)
 *
 * @param [in] message The raw message received (IV + ciphertext)
 * @param [in] message_len Length of the message received
 */
void lora_pcp_process_received_message(uint8_t *message, size_t message_len);

/**
 * @brief Encrypt plaintext with AES-CBC and transmit over LoRa
 *
 * @param [in] plaintext Data to encrypt and transmit
 * @param [in] plaintext_len Length of the plaintext data
 *
 * @returns True on success
 */
bool lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len);

#endif // LORA_PCP_H