#include "lora_funcs.h"
#include "lora_task.h"

#include "esp_log.h"
#include "esp_random.h"

#include "sx126x_hal.h"

#include <string.h>

static const char *TAG = "LORA_FUNCS";

static uint8_t encryption_key[16] = {0};

static void generate_random_iv(uint8_t *iv, size_t length) {
	for (size_t i = 0; i < length; i++) {
		iv[i] = (uint8_t)(rand() % (255 + 1)); // Generate number 0 - 255
	}
}

void lora_generate_random_key(void)
{
	esp_fill_random(encryption_key, sizeof(encryption_key));
	ESP_LOG_BUFFER_HEX("LORA KEY GENERATED:", encryption_key, sizeof(encryption_key));
}

void set_lora_rx_mode(void) // Call once to set RX mode and receive on EXTI8
{

	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1)); // Poll for SX1262 to be ready
	}

	// gpio_set_level(SX126X_DIO2_PIN, 1);

	// Enter continuous RX mode
	sx126x_status_t status = sx126x_set_rx(
		NULL,
		SX126X_RX_SINGLE_MODE); // sx126x_set_rx_with_timeout_in_rtc_step(NULL,
								// SX126X_RX_SINGLE_MODE);
	if (status != SX126X_STATUS_OK) {
		printf("Failed to enter continuous RX mode\n");
		return;
	}
}

void lora_tx(uint8_t tx_data[], uint8_t data_len) {

	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1)); // Poll for SX1262 to be ready
	}

	sx126x_status_t status = sx126x_write_buffer(NULL, 0, tx_data, data_len);
	if (status != SX126X_STATUS_OK) {
		printf("Failed to write to buffer\n");
	}

	// Start transmission
	status = sx126x_set_tx(NULL, SX126X_MAX_TIMEOUT_IN_MS);

	if (status != SX126X_STATUS_OK) {
		printf("Failed to start transmission\n");
	}
}

void process_received_message(uint8_t *message, size_t message_len) {
	// Verify that the message length is at least 16 bytes (for IV) + 16 bytes
	// (minimum ciphertext)
	if (message_len < 32) {
		printf("Received message too short!\n");
		return;
	}

	// The expected message length is 80 bytes (16 IV + 64 cyphertext)
	if (message_len != CYPHERTEXT_LENGTH + 16) {
		printf("Unexpected message length: %u bytes\n", (unsigned)message_len);
		return;
	}

	uint8_t iv[IV_LENGTH];			// To hold IV
	memcpy(iv, message, IV_LENGTH); // Extract the IV (first 16 bytes)

	uint8_t ciphertext[CYPHERTEXT_LENGTH]; // To hold cyphertext
	memcpy(ciphertext, message + IV_LENGTH,
		   CYPHERTEXT_LENGTH); // Extract the ciphertext (remaining 64 bytes)

	// Print the received IV
	ESP_LOGI(TAG, "Received IV: ");
	for (int i = 0; i < 16; i++) {
		ESP_LOGI(TAG, "%s", iv[i]);
	}
	ESP_LOGI(TAG, "\n");

	// osStatus_t status = osMessageQueuePut(lora_hex_queue_rx, message, 0, 0);
	// if (status != osOK)
	//{
	// printf("Failed to send lora_hex_queue_rx\n");
	//}

	// Initialize the AES context with the key and received IV.
	struct AES_ctx ctx;
	AES_init_ctx_iv(&ctx, encryption_key, iv);

	// Decrypt "ciphertext"
	AES_CBC_decrypt_buffer(&ctx, ciphertext, sizeof(ciphertext));

	ciphertext[sizeof(ciphertext) - 1] = '\0'; // Ensure null termination

	// status = osMessageQueuePut(lora_decrypted_queue_rx, ciphertext, 0, 0);
	// if (status != osOK)
	//{
	//	printf("Failed to send lora_decrypted_queue_rx\n");
	// }

	// "cyphertext" is now decrypted - print
	ESP_LOGI(TAG, "Decrypted text: %s\n", ciphertext);
}

void encrypt_and_transmit(uint8_t plaintext[]) {

	uint8_t buffer[CYPHERTEXT_LENGTH]; // Padded to 64 bytes (must be multiple
									   // of 16)
	memcpy(buffer, plaintext, sizeof(buffer)); // Copy the 64 bytes into buffer

	uint8_t iv[IV_LENGTH];				// To hold IV
	generate_random_iv(iv, sizeof(iv)); // Generate random IV into iv[16]

	/*printf("Generated IV: ");
	for (int i = 0; i < 16; i++) {
		printf("%02X ", iv[i]);
	}
	printf("\n");*/

	struct AES_ctx ctx;
	AES_init_ctx_iv(&ctx, encryption_key,
					iv); // Initialize AES context with key and IV

	AES_CBC_encrypt_buffer(&ctx, buffer, sizeof(buffer)); // Encrypt buffer

	uint8_t message[IV_LENGTH + CYPHERTEXT_LENGTH]; // New buffer to send
	memcpy(message, iv, IV_LENGTH);					// First 16 bytes are IV
	memcpy(message + IV_LENGTH, buffer,
		   CYPHERTEXT_LENGTH); // Next are the cyphertext

	/*printf("Message to send (hex): ");
	for (int i = 0; i < (int)sizeof(message); i++)
	{
		printf("%02X ", message[i]);
	}
	printf("\n");*/

	// osStatus_t status = osMessageQueuePut(lora_hex_queue_tx, message, 0, 0);
	// if (status != osOK)
	//{
	//	printf("Failed to send data to lora_hex_queue_tx\n");
	// }

	lora_tx(message, sizeof(message)); // Send the data
}
