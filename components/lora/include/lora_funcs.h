#ifndef LORA_FUNCS_H
#define LORA_FUNCS_H

#include "aes.h"
#include "sx126x_hal.h"

#define CYPHERTEXT_LENGTH 64
#define IV_LENGTH 16
#define PAYLOAD_LENGTH (CYPHERTEXT_LENGTH + IV_LENGTH)

#define ENC_KEY_LEN 16
#define INSTR_LEN 20

typedef struct sx126x_s {
	void *context;
	sx126x_hal_status_t (*hal_write)(const void *context, const uint8_t *command, const uint16_t command_length, const uint8_t *data, const uint16_t data_length);
	sx126x_hal_status_t (*hal_read)(const void *context, const uint8_t *command, const uint16_t command_length, uint8_t *data, const uint16_t data_length);
	sx126x_hal_status_t (*hal_reset)(const void *context);
	sx126x_hal_status_t (*hal_wakeup)(const void *context);
} sx126x_t;

typedef struct {
    uint8_t key[ENC_KEY_LEN];
    int index;
    char instr[INSTR_LEN];
} lora_cmd_t;

extern uint8_t encryption_key[ENC_KEY_LEN];

void lora_generate_random_key(void);
uint32_t lora_create_msg_id(void);

void lora_set_rx_mode(void);
void lora_process_received_message(uint8_t *message, size_t message_len);

void lora_tx(uint8_t tx_data[], uint8_t data_len);
void lora_encrypt_and_transmit(uint8_t plaintext[]);

#endif // LORA_FUNCS_H