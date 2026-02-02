#include "polycast5_macros.h"
#include "polycast5_gpios.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "sx126x.h"
#include "sx126x_hal.h"

#include "lora_utils.h"
#include "lora_task.h"
#include "espnow_task.h"
#include "gpio_task.h"

static const char *TAG = "LORA_UTILS";

uint32_t expected_rx_id = 0;
uint8_t encryption_key[LORA_ENC_KEY_LEN] = {0};

bool waiting_for_ack = false;

static void generate_random_iv(uint8_t *iv, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        iv[i] = (uint8_t)(esp_random() % (255 + 1)); // Generate number 0 - 255
    }
}

uint32_t lora_utils_create_msg_id(void)
{
    uint32_t id;
    do {
        id = esp_random();
    } while(id == 0);
    
    return id;
}

void lora_utils_generate_random_key(void)
{
    // Generate random encryption key
    esp_fill_random(encryption_key, sizeof(encryption_key));
    
#ifdef POLYCAST5_DEBUG
    ESP_LOG_BUFFER_HEX("LORA KEY GENERATED", encryption_key, sizeof(encryption_key));
#endif
    
    if (xQueueSend(xEspSendEncKeyQueue, encryption_key, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGE("LORA", "Failed to queue encryption key");
    }
}

void lora_utils_set_rx_mode(void) // Call once to set RX mode and receive on EXTI8
{
    #define RTC_FREQ_HZ 32768U
    #define MS_TO_RTC_STEP(ms) ((uint32_t)(((uint64_t)(ms) * RTC_FREQ_HZ) / 1000U))

    // Poll for SX1262 to be ready
    while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Enter RX mode
    // Use timeout in case receipt is never received
    uint32_t timeout_steps = MS_TO_RTC_STEP(2000);
    sx126x_status_t status = sx126x_set_rx_with_timeout_in_rtc_step(NULL, timeout_steps);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to enter continuous RX mode\n");
        return;
    }
}

void lora_utils_transmit(uint8_t tx_data[], uint8_t data_len)
{
    // Poll for SX1262 to be ready
    while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    sx126x_status_t status = sx126x_write_buffer(NULL, 0, tx_data, data_len);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to write to buffer\n");
    }

    // Start transmission
    status = sx126x_set_tx(NULL, SX126X_MAX_TIMEOUT_IN_MS);

    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to start transmission\n");
    }
}

void lora_utils_process_received_message(uint8_t *message, size_t message_len) {
    // Verify that the message length is at least 16 bytes (for IV) + 16 bytes
    // (minimum ciphertext)
    if (message_len < 32) {
        ESP_LOGE(TAG, "Received message too short!\n");
        return;
    }

    // The expected message length is 80 bytes (16 IV + 64 cyphertext)
    if (message_len != LORA_CYPHERTEXT_LENGTH + 16) {
        ESP_LOGE(TAG, "Unexpected message length: %u bytes\n", (unsigned)message_len);
        return;
    }

    uint8_t iv[LORA_IV_LENGTH]; // To hold IV
    memcpy(iv, message, LORA_IV_LENGTH); // Extract the IV (first 16 bytes)

    uint8_t ciphertext[LORA_CYPHERTEXT_LENGTH]; // Buffer to hold cyphertext
    memcpy(ciphertext, message + LORA_IV_LENGTH, LORA_CYPHERTEXT_LENGTH); // Extract the ciphertext (remaining 64 bytes)

#ifdef POLYCAST5_DEBUG
    ESP_LOG_BUFFER_HEX(TAG, iv, LORA_IV_LENGTH);
#endif
    
    // Initialize the AES context with the key and received IV.
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, encryption_key, iv);

    // Decrypt 'ciphertext'
    AES_CBC_decrypt_buffer(&ctx, ciphertext, sizeof(ciphertext));

    ciphertext[sizeof(ciphertext) - 1] = '\0'; // Ensure NULL termination
    
    // 'cyphertext' is now decrypted
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Decrypted text: %s\n", ciphertext);
#endif
    
    uint32_t received_rx_id;
    
    // If received valid receipt
    if (sscanf((char*)ciphertext, "PolyCast_Command_Value_Received:%" SCNu32, &received_rx_id) == 1) {
        if (received_rx_id == expected_rx_id) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "ACK matches id=%" PRIu32, received_rx_id);
#endif
            
            xQueueReset(xLoraSendEncQueue); // Clear pending commands
            waiting_for_ack = false;
            
            xSemaphoreGive(xLoraReceiptValidSemaphore);
        } else {
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "ACK ID wrong (got=%" PRIu32 ", want=%" PRIu32 ")", received_rx_id, expected_rx_id);
#endif
        }
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Decrypted text does NOT match. Got: \"%s\"", ciphertext);
#endif
    }
}

void lora_utils_encrypt_and_transmit(uint8_t plaintext[])
{
    // Measure how many bytes of real data we have, up to the max
    size_t plaintext_len = strnlen((char*)plaintext, LORA_CYPHERTEXT_LENGTH + 1);
    
    // Check length
    if (plaintext_len > LORA_CYPHERTEXT_LENGTH) {
        ESP_LOGE(TAG,
            "LoRa plaintext too long (%u bytes), max is %u",
            (unsigned)plaintext_len,
            (unsigned)LORA_CYPHERTEXT_LENGTH);
        return;
    }
    
    uint8_t buffer[LORA_CYPHERTEXT_LENGTH] = {0}; // Padded to 64 bytes (must be multiple of 16)
    memcpy(buffer, plaintext, sizeof(buffer)); // Copy the 64 bytes into buffer

    uint8_t iv[LORA_IV_LENGTH]; // To hold IV
    generate_random_iv(iv, sizeof(iv)); // Generate random IV into iv[16]

    /*ESP_LOGE(TAG, "Generated IV: ");
    for (int i = 0; i < 16; i++) {
        ESP_LOGE(TAG, "%02X ", iv[i]);
    }
    ESP_LOGE(TAG, "\n");*/

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, encryption_key,
                    iv); // Initialize AES context with key and IV

    AES_CBC_encrypt_buffer(&ctx, buffer, sizeof(buffer)); // Encrypt buffer

    uint8_t message[LORA_IV_LENGTH + LORA_CYPHERTEXT_LENGTH]; // New buffer to send
    memcpy(message, iv, LORA_IV_LENGTH); // First 16 bytes are IV
    memcpy(message + LORA_IV_LENGTH, buffer, LORA_CYPHERTEXT_LENGTH); // Next are the cyphertext

    /*ESP_LOGE(TAG, "Message to send (hex): ");
    for (int i = 0; i < (int)sizeof(message); i++)
    {
        ESP_LOGE(TAG, "%02X ", message[i]);
    }
    ESP_LOGE(TAG, "\n");*/

    lora_utils_transmit(message, sizeof(message)); // Send the data
}
