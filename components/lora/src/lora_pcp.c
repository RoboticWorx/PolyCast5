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

#include "psa/crypto.h"

static const char *TAG = "LORA_PCP";

volatile uint32_t expected_rx_id = 0;
static uint32_t msg_id_counter = 0;
static uint8_t encryption_key[LORA_PCP_ENC_KEY_LEN] = {0};
static psa_key_id_t pcp_key_id = 0;

volatile bool waiting_for_ack = false;

#define PCP_NVS_NS     "pcp"
#define PCP_NVS_MSG_ID "msg_id"

#define PCP_CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, LORA_PCP_MIC_LENGTH)

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
    // Initialize PSA crypto subsystem (idempotent)
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return;
    }

    // Import into a temporary ID first - preserve old key on failure
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PCP_CCM_ALG);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, LORA_PCP_ENC_KEY_LEN * 8);

    psa_key_id_t new_key_id = 0;
    status = psa_import_key(&attr, key, LORA_PCP_ENC_KEY_LEN, &new_key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %d, keeping old key", (int)status);
        return;
    }

    // Import succeeded - commit all atomically
    if (pcp_key_id != 0) {
        status = psa_destroy_key(pcp_key_id);
        if (status != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_destroy_key failed: %d (key slot leak)", (int)status);
        }
    }
    pcp_key_id = new_key_id;
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
    // Minimum valid packet: nonce + ACK ciphertext + MIC
    size_t min_len = LORA_PCP_NONCE_LENGTH + LORA_PCP_ACK_CIPHERTEXT_LEN + LORA_PCP_MIC_LENGTH;
    if (message_len < min_len) {
        ESP_LOGE(TAG, "Received message too short!");
        return;
    }

    if (pcp_key_id == 0) {
        ESP_LOGE(TAG, "No key set, ignoring message");
        return;
    }

    // Extract nonce (first 13 bytes) and ciphertext+tag (rest)
    const uint8_t *nonce      = message;
    const uint8_t *ct_and_tag = message + LORA_PCP_NONCE_LENGTH;
    size_t ct_and_tag_len     = message_len - LORA_PCP_NONCE_LENGTH;

    // Decrypt and authenticate (PSA expects ciphertext || tag as one buffer)
    uint8_t plaintext[LORA_PCP_CIPHERTEXT_LENGTH] = {0};
    size_t plaintext_len = 0;

    psa_status_t status = psa_aead_decrypt(
            pcp_key_id, PCP_CCM_ALG,
            nonce, LORA_PCP_NONCE_LENGTH,
            NULL, 0,
            ct_and_tag, ct_and_tag_len,
            plaintext, sizeof(plaintext),
            &plaintext_len);

    if (status != PSA_SUCCESS) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "CCM auth failed: %d", (int)status);
#endif
        return;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOG_BUFFER_HEX("LORA_PCP: Decrypted", plaintext, plaintext_len);
#endif

    uint8_t msg_type = plaintext[0];

    if (msg_type == LORA_PCP_ACK && plaintext_len == LORA_PCP_ACK_CIPHERTEXT_LEN) {
        lora_pcp_ack_msg_t ack;
        memcpy(&ack, plaintext, sizeof(ack));

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
    if (plaintext_len > LORA_PCP_CIPHERTEXT_LENGTH) {
        ESP_LOGE(TAG, "LoRa plaintext too long (%u bytes, max %u)",
                 (unsigned)plaintext_len,
                 (unsigned)LORA_PCP_CIPHERTEXT_LENGTH);
        return false;
    }

    if (pcp_key_id == 0) {
        ESP_LOGE(TAG, "No key set, cannot transmit");
        return false;
    }

    uint8_t nonce[LORA_PCP_NONCE_LENGTH];
    esp_fill_random(nonce, sizeof(nonce));

    // PSA outputs ciphertext || tag concatenated
    uint8_t ct_and_tag[LORA_PCP_CIPHERTEXT_LENGTH + LORA_PCP_MIC_LENGTH];
    size_t ct_and_tag_len = 0;

    psa_status_t status = psa_aead_encrypt(
            pcp_key_id, PCP_CCM_ALG,
            nonce, LORA_PCP_NONCE_LENGTH,
            NULL, 0,
            plaintext, plaintext_len,
            ct_and_tag, sizeof(ct_and_tag),
            &ct_and_tag_len);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "CCM encrypt failed: %d", (int)status);
        return false;
    }

    // Assemble wire message: [nonce | ciphertext | MIC]
    uint8_t message[LORA_PCP_PAYLOAD_LENGTH];
    size_t msg_len = LORA_PCP_NONCE_LENGTH + ct_and_tag_len;

    memcpy(message, nonce, LORA_PCP_NONCE_LENGTH);
    memcpy(message + LORA_PCP_NONCE_LENGTH, ct_and_tag, ct_and_tag_len);

    return lora_radio_tx(message, msg_len);
}
