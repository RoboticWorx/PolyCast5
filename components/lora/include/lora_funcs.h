#ifndef LORA_FUNCS_H
#define LORA_FUNCS_H

#include "aes.h"
#include "sx126x_hal.h"

#define LORA_CYPHERTEXT_LENGTH 64
#define LORA_IV_LENGTH 16
#define LORA_PAYLOAD_LENGTH (LORA_CYPHERTEXT_LENGTH + LORA_IV_LENGTH)

#define LORA_ENC_KEY_LEN 16
#define LORA_MAX_INSTR_LEN 32

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

void lora_generate_random_key(void);
uint32_t lora_create_msg_id(void);

void lora_set_rx_mode(void);
void lora_process_received_message(uint8_t *message, size_t message_len);

void lora_tx(uint8_t tx_data[], uint8_t data_len);
void lora_encrypt_and_transmit(uint8_t plaintext[]);

#endif // LORA_FUNCS_H