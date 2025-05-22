#ifndef LORA_FUNCS_H
#define LORA_FUNCS_H

#include "aes.h"
#include "sx126x.h"
#include "sx126x_hal.h"

#define CYPHERTEXT_LENGTH 64
#define IV_LENGTH 16
#define PAYLOAD_LENGTH (CYPHERTEXT_LENGTH + IV_LENGTH)

void lora_generate_random_key(void);

void set_lora_rx_mode(void);
void process_received_message(uint8_t *message, size_t message_len);

void lora_tx(uint8_t tx_data[], uint8_t data_len);
void encrypt_and_transmit(uint8_t plaintext[]);

#endif // LORA_FUNCS_H