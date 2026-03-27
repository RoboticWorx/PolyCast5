#include "polycast5_macros.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "lora_pcp.h"
#include "lora_radio.h"
#include "lora_task.h"
#include "espnow_task.h"
#include "gpio_task.h"

#include "aes.h"

static const char *TAG = "LORA_PCP";

uint32_t expected_rx_id = 0;
static uint8_t encryption_key[LORA_PCP_ENC_KEY_LEN] = {0};

bool waiting_for_ack = false;

static void generate_random_iv(uint8_t *iv, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        iv[i] = (uint8_t)(esp_random() % (255 + 1)); // Generate number 0 - 255
    }
}

void lora_pcp_set_key(const uint8_t *key)
{
    memcpy(encryption_key, key, LORA_PCP_ENC_KEY_LEN);
}

uint32_t lora_pcp_create_msg_id(void)
{
    uint32_t id;
    do {
        id = esp_random();
    } while (id == 0);

    return id;
}

void lora_pcp_generate_random_key(void)
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

void lora_pcp_process_received_message(uint8_t *message, size_t message_len)
{
    // Minimum: 16 bytes IV + 16 bytes ciphertext (one AES block)
    if (message_len < LORA_PCP_IV_LENGTH + 16) {
        ESP_LOGE(TAG, "Received message too short!\n");
        return;
    }

    size_t ct_len = message_len - LORA_PCP_IV_LENGTH;

    // Ciphertext must be a multiple of 16 (AES block size) and within max
    if ((ct_len % 16) != 0 || ct_len > LORA_PCP_CIPHERTEXT_LENGTH) {
        ESP_LOGE(TAG, "Invalid ciphertext length: %u bytes\n", (unsigned)ct_len);
        return;
    }

    uint8_t iv[LORA_PCP_IV_LENGTH]; // To hold IV
    memcpy(iv, message, LORA_PCP_IV_LENGTH); // Extract the IV (first 16 bytes)

    uint8_t ciphertext[LORA_PCP_CIPHERTEXT_LENGTH] = {0}; // Buffer to hold ciphertext
    memcpy(ciphertext, LORA_PCP_IV_LENGTH + message, ct_len); // Extract the ciphertext

#ifdef POLYCAST5_DEBUG
    ESP_LOG_BUFFER_HEX(TAG, iv, LORA_PCP_IV_LENGTH);
#endif

    // Initialize the AES context with the key and received IV.
    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, encryption_key, iv);

    // Decrypt ciphertext
    AES_CBC_decrypt_buffer(&ctx, ciphertext, ct_len);

#ifdef POLYCAST5_DEBUG
    ESP_LOG_BUFFER_HEX("LORA_PCP: Decrypted", ciphertext, ct_len);
#endif

    // Validate magic bytes
    uint16_t magic;
    memcpy(&magic, ciphertext, sizeof(magic));
    if (magic != LORA_PCP_MAGIC) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Bad magic: 0x%04X", magic);
#endif
        return;
    }

    uint8_t msg_type = ciphertext[2];

    if (msg_type == LORA_PCP_MSG_ACK && ct_len == LORA_PCP_ACK_CIPHERTEXT_LEN) {
        lora_pcp_ack_msg_t ack;
        memcpy(&ack, ciphertext, sizeof(ack));

        if (ack.msg_id == expected_rx_id) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "ACK matches id=%" PRIu32, ack.msg_id);
#endif
            xQueueReset(xLoraSendEncQueue); // Clear pending commands
            waiting_for_ack = false;

            xSemaphoreGive(xLoraReceiptValidSemaphore);
        } else {
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "ACK ID wrong (got=%" PRIu32 ", want=%" PRIu32 ")", ack.msg_id, expected_rx_id);
#endif
        }
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "Unknown message type: 0x%02X", msg_type);
#endif
    }
}

bool lora_pcp_encrypt_and_transmit(uint8_t plaintext[], size_t plaintext_len)
{
    // Round up to next AES block size (multiple of 16)
    size_t padded_len = ((plaintext_len + 15) / 16) * 16;

    // Check length
    if (padded_len > LORA_PCP_CIPHERTEXT_LENGTH) {
        ESP_LOGE(TAG,
            "LoRa plaintext too long (%u bytes, padded %u), max is %u",
            (unsigned)plaintext_len,
            (unsigned)padded_len,
            (unsigned)LORA_PCP_CIPHERTEXT_LENGTH);
        return false;
    }

    uint8_t buffer[LORA_PCP_CIPHERTEXT_LENGTH] = {0}; // Zero-padded for AES block alignment
    memcpy(buffer, plaintext, plaintext_len); // Copy only the actual data

    uint8_t iv[LORA_PCP_IV_LENGTH]; // To hold IV
    generate_random_iv(iv, sizeof(iv)); // Generate random IV into iv[16]

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, encryption_key, iv); // Initialize AES context with key and IV

    AES_CBC_encrypt_buffer(&ctx, buffer, padded_len); // Encrypt padded data

    uint8_t message[LORA_PCP_IV_LENGTH + LORA_PCP_CIPHERTEXT_LENGTH]; // Buffer to send
    memcpy(message, iv, LORA_PCP_IV_LENGTH); // First 16 bytes are IV
    memcpy(message + LORA_PCP_IV_LENGTH, buffer, padded_len); // Next are the ciphertext

    return lora_radio_tx(message, LORA_PCP_IV_LENGTH + padded_len);
}
