#ifndef LORA_FUNCS_H
#define LORA_FUNCS_H

#include "aes.h"
#include "lora_task.h"

#define CYPHERTEXT_LENGTH 64
#define IV_LENGTH 16
#define PAYLOAD_LENGTH (CYPHERTEXT_LENGTH + IV_LENGTH)

extern uint8_t encryption_key[ENC_KEY_LEN];

void lora_generate_random_key(void);
uint32_t lora_create_msg_id(void);

void lora_set_rx_mode(void);
void lora_process_received_message(uint8_t *message, size_t message_len);

void lora_tx(uint8_t tx_data[], uint8_t data_len);
void lora_encrypt_and_transmit(uint8_t plaintext[]);

#endif // LORA_FUNCS_H