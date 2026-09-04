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

#define LORA_CFG_NVS_NS     "lora_cfg" // Radio config kept separate from the per-TX msg_id counter
#define LORA_CFG_NVS_SF     "sf"       // Persisted spreading factor
#define LORA_CFG_NVS_REGION "region"   // Persisted region (RF band)

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

uint8_t lora_pcp_load_sf_nvs(void)
{
	nvs_handle_t h;
	uint8_t sf = LORA_PCP_SF_DEFAULT; // Default preserves the original hardcoded SF7

	// Open read-only; a missing namespace/key or out-of-range value falls back to the default
	if (nvs_open(LORA_CFG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
		uint8_t stored;
		if (nvs_get_u8(h, LORA_CFG_NVS_SF, &stored) == ESP_OK &&
		    stored >= LORA_PCP_SF_MIN && stored <= LORA_PCP_SF_MAX) {
			sf = stored;
		}
		nvs_close(h);
	}

#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Loaded LoRa SF=%u", sf);
#endif

	return sf;
}

esp_err_t lora_pcp_save_sf_nvs(uint8_t sf)
{
	// Clamp to the supported range so a bad value can never reach the radio
	if (sf < LORA_PCP_SF_MIN) {
		sf = LORA_PCP_SF_MIN;
	} else if (sf > LORA_PCP_SF_MAX) {
		sf = LORA_PCP_SF_MAX;
	}

	nvs_handle_t h;
	esp_err_t err = nvs_open(LORA_CFG_NVS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_sf_nvs: NVS open failed: %s", esp_err_to_name(err));
		return err;
	}

	err = nvs_set_u8(h, LORA_CFG_NVS_SF, sf);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_sf_nvs: NVS write failed: %s", esp_err_to_name(err));
	}

	nvs_close(h);
	return err;
}

// Per-region radio parameters - see lora_region_params_t. Rows are keyed by lora_region_t.
// PCP carrier = band center (EU sits in the 869.4-869.65 MHz high-power sub-band: ETSI allows
//               500 mW / 27 dBm at 10% duty there, so PCP's 22 dBm TX stays legal; BW125 at
//               869.5 spans 869.4375-869.5625, inside the sub-band).
// Mesh slot   = Meshtastic LongFast: slot = djb2("LongFast") % floor((end-start)/0.25),
//               freq = start + 0.125 + slot*0.25 (BW 250 kHz, spacing 0 in every region).
//               Band edges mirror meshtastic/firmware RadioInterface.cpp.
// Cal band    = the SX1262 standard image-cal band containing the region (902-928 or 863-870).
// Note: TW and TH share the same 920-925 MHz band, so identical frequencies are intentional.
static const lora_region_params_t region_params[LORA_REGION_COUNT] = {
	//                    name   full name         PCP carrier  LongFast slot cal img  LCD freq       LCD band
	[LORA_REGION_US]  = { "US",  "United States",  915000000UL, 906875000UL, 902, 928, "915 MHz",    "902-928 MHz" },     // 902-928, n=104, slot 19
	[LORA_REGION_EU]  = { "EU",  "Europe",         869500000UL, 869525000UL, 863, 870, "869.5 MHz",  "863-870 MHz" },     // EU_868 869.4-869.65 sub-band, n=1, slot 0
	[LORA_REGION_ANZ] = { "ANZ", "Australia/NZ",   921500000UL, 919875000UL, 902, 928, "921.5 MHz",  "915-928 MHz" },     // 915-928, n=52, slot 19
	[LORA_REGION_IN]  = { "IN",  "India",          866000000UL, 865875000UL, 863, 870, "866 MHz",    "865-867 MHz" },     // 865-867, n=8, slot 3
	[LORA_REGION_KR]  = { "KR",  "South Korea",    921500000UL, 922875000UL, 902, 928, "921.5 MHz",  "920-923 MHz" },     // 920-923, n=12, slot 11
	[LORA_REGION_JP]  = { "JP",  "Japan",          922000000UL, 923375000UL, 902, 928, "922 MHz",    "920.5-923.5 MHz" }, // 920.5-923.5, n=12, slot 11
	[LORA_REGION_TW]  = { "TW",  "Taiwan",         922500000UL, 923875000UL, 902, 928, "922.5 MHz",  "920-925 MHz" },     // 920-925, n=20, slot 15
	[LORA_REGION_RU]  = { "RU",  "Russia",         868950000UL, 869075000UL, 863, 870, "868.95 MHz", "868.7-869.2 MHz" }, // 868.7-869.2, n=2, slot 1
	[LORA_REGION_TH]  = { "TH",  "Thailand",       922500000UL, 923875000UL, 902, 928, "922.5 MHz",  "920-925 MHz" },     // 920-925, n=20, slot 15
	[LORA_REGION_SG]  = { "SG",  "Singapore",      921000000UL, 917875000UL, 902, 928, "921 MHz",    "917-925 MHz" },     // SG_923 917-925, n=32, slot 3
	[LORA_REGION_MY]  = { "MY",  "Malaysia",       921500000UL, 922875000UL, 902, 928, "921.5 MHz",  "919-924 MHz" },     // MY_919 919-924, n=20, slot 15
};

const lora_region_params_t *lora_region_get_params(lora_region_t region)
{
	// Clamp out-of-range inputs
	int idx = (int)region;
    if (idx < 0 || idx >= LORA_REGION_COUNT) {
        idx = LORA_REGION_DEFAULT;
    }
    return &region_params[idx];
}

lora_region_t lora_pcp_load_region_nvs(void)
{
	nvs_handle_t h;
	lora_region_t region = LORA_REGION_DEFAULT; // Default preserves the original hardcoded 915 MHz band

	// Open read-only; a missing namespace/key or out-of-range value falls back to the default
	if (nvs_open(LORA_CFG_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
		uint8_t stored;
		if (nvs_get_u8(h, LORA_CFG_NVS_REGION, &stored) == ESP_OK &&
		    stored < LORA_REGION_COUNT) {
			region = (lora_region_t)stored;
		}
		nvs_close(h);
	}

#ifdef POLYCAST5_DEBUG
	ESP_LOGI(TAG, "Loaded LoRa region=%s", lora_region_get_params(region)->name);
#endif

	return region;
}

esp_err_t lora_pcp_save_region_nvs(lora_region_t region)
{
	// Clamp to a valid region so a bad value can never reach the radio
	int idx = (int)region;
	if (idx < 0 || idx >= LORA_REGION_COUNT) {
		idx = LORA_REGION_DEFAULT;
	}
	region = (lora_region_t)idx;

	nvs_handle_t h;
	esp_err_t err = nvs_open(LORA_CFG_NVS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_region_nvs: NVS open failed: %s", esp_err_to_name(err));
		return err;
	}

	err = nvs_set_u8(h, LORA_CFG_NVS_REGION, (uint8_t)region);
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "lora_pcp_save_region_nvs: NVS write failed: %s", esp_err_to_name(err));
	}

	nvs_close(h);
	return err;
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

        // Post a failure result so the LCD task waiting on the pairing outcome is released right away
        espnow_enc_key_result_t key_result = { .success = false };
        xQueueOverwrite(xEspSendEncKeyQueueNVS, &key_result);
    }
}

void lora_pcp_process_received_message(uint8_t *message, size_t message_len)
{
    // Minimum valid packet: nonce + smallest (v1) ACK ciphertext + MIC
    size_t min_len = LORA_PCP_NONCE_LENGTH + LORA_PCP_ACK_V1_CIPHERTEXT_LEN + LORA_PCP_MIC_LENGTH;
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

    // Accept both ACK forms: v2 carries the relay state byte, v1 (older PolyPlug
    // firmware) stops after the msg_id and is reported as "no state"
    if (msg_type == LORA_PCP_ACK &&
            (plaintext_len == LORA_PCP_ACK_CIPHERTEXT_LEN || plaintext_len == LORA_PCP_ACK_V1_CIPHERTEXT_LEN)) {
        lora_pcp_ack_msg_t ack = { .state = LORA_PCP_ACK_STATE_NONE }; // v1 leaves state at NONE
        memcpy(&ack, plaintext, plaintext_len);

        if (ack.msg_id == expected_rx_id) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "ACK matches id=%" PRIu32 " state=0x%02X", ack.msg_id, ack.state);
#endif
            waiting_for_ack = false; // Done; a queued next command dispatches normally

            lora_task_post_ack(ack.state); // Publishes the outcome for the UI
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
