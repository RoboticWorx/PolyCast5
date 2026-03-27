// ┌──────────────────────────────────────────────────────────────────────────────┐
// │              LoRa Binary Protocol (AES-128-CBC, Explicit Header)             │
// ├──────────────────────────────────────────────────────────────────────────────┤
// │                        Command Packet (64 bytes on air)                      │
// ├────────────────┬─────────────────────────────────────────────────────────────┤
// │   IV (16B)     │              AES-CBC Ciphertext (48B)                       │
// │   Random IV    ├────────┬──────┬──────────┬───────┬────────────┬─────────────┤
// │                │ Magic  │ Type │  Msg ID  │ Index │   Instr    │  Zero Pad   │
// │                │  2B    │  1B  │   4B     │  1B   │   32B      │    8B       │
// │                │ 0x5043 │ 0x01 │ uint32   │ uint8 │ char[32]   │  (AES pad)  │
// ├────────────────┼────────┴──────┴──────────┴───────┴────────────┴─────────────┤
// │                │                                                             │
// │                │       ACK Packet (32 bytes on air)                          │
// ├────────────────┼─────────────────────────────────────────────────────────────┤
// │   IV (16B)     │              AES-CBC Ciphertext (16B)                       │
// │   Random IV    ├────────┬──────┬──────────┬──────────────────────────────────┤
// │                │ Magic  │ Type │  Msg ID  │           Zero Pad               │
// │                │  2B    │  1B  │   4B     │              9B                  │
// │                │ 0x5043 │ 0x02 │  uint32  │           (AES pad)              │
// └────────────────┴────────┴──────┴──────────┴──────────────────────────────────┘

#ifndef LORA_UTILS_H
#define LORA_UTILS_H

#include <stdbool.h>

#include "aes.h"
#include "sx126x_hal.h"

#define LORA_IV_LENGTH 16
#define LORA_ENC_KEY_LEN 16
#define LORA_MAX_INSTR_LEN 32

// Binary wire protocol
#define LORA_MSG_MAGIC   0x5043 // "PC" - brute-force guard
#define LORA_MSG_COMMAND 0x01
#define LORA_MSG_ACK     0x02

// Command message (40 bytes, padded to 48 for AES-CBC)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;
    uint32_t msg_id;
    uint8_t  index;
    char     instr[LORA_MAX_INSTR_LEN];
} lora_cmd_msg_t;

// ACK message (7 bytes, padded to 16 for AES-CBC)
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;
    uint32_t msg_id;
} lora_ack_msg_t;

// Ciphertext sizes per message type (multiple of 16)
#define LORA_CMD_CIPHERTEXT_LEN 48
#define LORA_ACK_CIPHERTEXT_LEN 16

// Max ciphertext size (command message)
#define LORA_CYPHERTEXT_LENGTH (LORA_CMD_CIPHERTEXT_LEN)
#define LORA_PAYLOAD_LENGTH    (LORA_CYPHERTEXT_LENGTH + LORA_IV_LENGTH)

typedef struct sx126x_s {
    void *context;
    sx126x_hal_status_t (*hal_write)(const void *context, const uint8_t *command, const uint16_t command_length, const uint8_t *data, const uint16_t data_length);
    sx126x_hal_status_t (*hal_read)(const void *context, const uint8_t *command, const uint16_t command_length, uint8_t *data, const uint16_t data_length);
    sx126x_hal_status_t (*hal_reset)(const void *context);
    sx126x_hal_status_t (*hal_wakeup)(const void *context);
} sx126x_t;

typedef struct {
    char instr[LORA_MAX_INSTR_LEN];
    uint8_t key[LORA_ENC_KEY_LEN];
    int index;
} lora_cmd_t;

extern uint8_t encryption_key[LORA_ENC_KEY_LEN];

/** 
 * @brief Generate random LoRa encryption key
 */
void lora_utils_generate_random_key(void);

/** 
 * @brief Generate random LoRa message ID for receipt confirmation
 *
 * @returns The ID created
 */
uint32_t lora_utils_create_msg_id(void);

/** 
 * @brief Sets SX1262 radio in receive mode
 */
void lora_utils_set_rx_mode(void);

/** 
 * @brief Process the received LoRa message (decrypt, etc.)
 *
 * @param [in] message The message received
 * @param [in] message_len Length of the message received
 */
void lora_utils_process_received_message(uint8_t *message, size_t message_len);

/** 
 * @brief Transmits the data over LoRa
 *
 * @param [in] tx_data Data to transmit
 * @param [in] data_len Length of data to transmit
 *
 * @returns True on success
 */
bool lora_utils_transmit(uint8_t tx_data[], uint8_t data_len);

/**
 * @brief Encrypts the data then transmits via lora_utils_transmit
 *
 * @param [in] plaintext Data to encrypt and transmit
 * @param [in] plaintext_len Length of the plaintext data
 *
 * @returns True on success
 */
bool lora_utils_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len);

#endif // LORA_UTILS_H