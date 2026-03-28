#include "polycast5_macros.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "lora_pcp.h"
#include "lora_radio.h"
#include "lora_task.h"
#include "espnow_task.h"
#include "gpio_task.h"

#include "aes.h"

static const char *TAG = "LORA_PCP";

volatile uint32_t expected_rx_id = 0;
static uint32_t msg_id_counter = 0;
static uint8_t encryption_key[LORA_PCP_ENC_KEY_LEN] = {0};

volatile bool waiting_for_ack = false;

#define PCP_NVS_NS     "pcp"
#define PCP_NVS_MSG_ID "msg_id"

static void save_msg_id_nvs(void)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(PCP_NVS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "save_msg_id_nvs: NVS open failed: %s", esp_err_to_name(err));
		return;
	}

	err = nvs_set_u32(h, PCP_NVS_MSG_ID, msg_id_counter);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "save_msg_id_nvs: NVS set failed: %s", esp_err_to_name(err));
	}

	err = nvs_commit(h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "save_msg_id_nvs: NVS commit failed: %s", esp_err_to_name(err));
	}

	nvs_close(h);
}

void lora_pcp_load_msg_id_nvs(void)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(PCP_NVS_NS, NVS_READONLY, &h);
	if (err == ESP_OK) {
		err = nvs_get_u32(h, PCP_NVS_MSG_ID, &msg_id_counter);
		if (err == ESP_ERR_NVS_NOT_FOUND) {
#ifdef POLYCAST5_DEBUG
			ESP_LOGI(TAG, "No persisted msg_id_counter, starting at 0");
#endif
            msg_id_counter = 0;
		} else if (err != ESP_OK) {
			ESP_LOGE(TAG, "lora_pcp_load_msg_id_nvs: NVS get failed: %s", esp_err_to_name(err));
		}
		nvs_close(h);
	} else if (err != ESP_ERR_NVS_NOT_FOUND) {
		ESP_LOGE(TAG, "lora_pcp_load_msg_id_nvs: NVS open failed: %s", esp_err_to_name(err));
	}

#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Loaded msg_id_counter=%" PRIu32, msg_id_counter);
#endif
}

void lora_pcp_set_key(const uint8_t *key)
{
    memcpy(encryption_key, key, LORA_PCP_ENC_KEY_LEN);
}

uint32_t lora_pcp_create_msg_id(void)
{
    ++msg_id_counter;
    save_msg_id_nvs();
    return msg_id_counter;
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

    if (msg_type == LORA_PCP_ACK && ct_len == LORA_PCP_ACK_CIPHERTEXT_LEN) {
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
    esp_fill_random(iv, sizeof(iv)); // Generate random IV into iv[16]

    struct AES_ctx ctx;
    AES_init_ctx_iv(&ctx, encryption_key, iv); // Initialize AES context with key and IV

    AES_CBC_encrypt_buffer(&ctx, buffer, padded_len); // Encrypt padded data

    uint8_t message[LORA_PCP_IV_LENGTH + LORA_PCP_CIPHERTEXT_LENGTH]; // Buffer to send
    memcpy(message, iv, LORA_PCP_IV_LENGTH); // First 16 bytes are IV
    memcpy(message + LORA_PCP_IV_LENGTH, buffer, padded_len); // Next are the ciphertext

    return lora_radio_tx(message, LORA_PCP_IV_LENGTH + padded_len);
}
