#include "polycast5_macros.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "psa/crypto.h"

#include "espnow_auth.h"
#include "espnow_utils.h"

#define TAG "ESPNOW_AUTH"

#define ESPNOW_AUTH_NVS_NS  "espnow_auth" // Namespace
#define ESPNOW_AUTH_NVS_CTR "ctr_hi"      // Highest counter value reserved so far
#define ESPNOW_AUTH_NVS_SEED "seeded"     // Marker proving a counter was once written here

// Counter values claimed per NVS write - only the ceiling is persisted
#define ESPNOW_AUTH_CTR_RESERVE 32

#define ESPNOW_AUTH_CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, ESPNOW_AUTH_MIC_LEN)

static uint32_t send_counter = 0;    // Last value handed out
static uint32_t counter_ceiling = 0; // First value not yet reserved in NVS
static bool counter_ready = false;   // False if the counter could not be established safely

static esp_err_t save_ceiling_nvs(uint32_t ceiling)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ESPNOW_AUTH_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_ceiling_nvs: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(h, ESPNOW_AUTH_NVS_CTR, ceiling);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_ceiling_nvs: NVS set failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    // Marker after the ceiling and non-fatal
    esp_err_t seed_err = nvs_set_u8(h, ESPNOW_AUTH_NVS_SEED, 1);
    if (seed_err != ESP_OK) {
        ESP_LOGE(TAG, "save_ceiling_nvs: seed marker failed: %s", esp_err_to_name(seed_err));
    }

    nvs_close(h);
    return err;
}

void espnow_auth_load_counter_nvs(void)
{
    uint32_t stored = 0;

    counter_ready = false;

    // Only a genuinely untouched namespace may start from zero
    nvs_handle_t h;
    esp_err_t err = nvs_open(ESPNOW_AUTH_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No persisted send counter, starting at 0");
    } else if (err == ESP_OK) {
        // The marker decides how to read a missing counter
        // Absent = nothing was ever issued from this namespace, so zero is correct
        uint8_t seeded = 0;
        esp_err_t seed_err = nvs_get_u8(h, ESPNOW_AUTH_NVS_SEED, &seeded);

        err = nvs_get_u32(h, ESPNOW_AUTH_NVS_CTR, &stored);
        nvs_close(h);

        if (err == ESP_OK) {
            // Counter read cleanly
        } else if (seed_err == ESP_ERR_NVS_NOT_FOUND && err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No persisted send counter, starting at 0");
            stored = 0;
        } else {
            ESP_LOGE(TAG, "Send counter lost (%s); authenticated sends disabled",
                    esp_err_to_name(err));
            return;
        }
    } else {
        ESP_LOGE(TAG, "NVS open failed (%s); authenticated sends disabled", esp_err_to_name(err));
        return;
    }

    counter_ready = true;

    // Resume above every value the previous boot could have used, then reserve the next
    // block up front so the common send path does not touch flash
    send_counter = stored;

    uint32_t ceiling = (stored > UINT32_MAX - ESPNOW_AUTH_CTR_RESERVE)
            ? UINT32_MAX
            : stored + ESPNOW_AUTH_CTR_RESERVE;

    if (save_ceiling_nvs(ceiling) == ESP_OK) {
        counter_ceiling = ceiling;
    } else {
        // Nothing was reserved, so leave the ceiling at the counter
        counter_ceiling = send_counter;
        ESP_LOGE(TAG, "Could not reserve a counter block; each send must reserve its own");
    }

    ESP_LOGI(TAG, "Send counter resumed at %" PRIu32, send_counter);
}

/** Claim the next counter value, extending the reserved block when it runs out */
static bool next_counter(uint32_t *out)
{
    if (!counter_ready) {
        // Whatever stopped the counter being read at boot may since have cleared, so try again
        ESP_LOGW(TAG, "Send counter not established, retrying");
        espnow_auth_load_counter_nvs();

        if (!counter_ready) {
            ESP_LOGE(TAG, "Send counter still unavailable, refusing to send");
            return false;
        }
    }

    if (send_counter == UINT32_MAX) {
        ESP_LOGE(TAG, "Send counter exhausted, refusing to wrap"); // Wrapping would replay nonces
        return false;
    }

    uint32_t next = send_counter + 1;

    if (next >= counter_ceiling) {
        uint32_t ceiling = (next > UINT32_MAX - ESPNOW_AUTH_CTR_RESERVE)
                ? UINT32_MAX
                : next + ESPNOW_AUTH_CTR_RESERVE;

        // Reserve before use: if the write fails, values past the old ceiling are not safe
        if (save_ceiling_nvs(ceiling) != ESP_OK) {
            return false;
        }
        counter_ceiling = ceiling;
    }

    send_counter = next;
    *out = next;
    return true;
}

bool espnow_auth_build_frame(const uint8_t *lmk, uint8_t cmd, uint8_t *frame)
{
    if (lmk == NULL || frame == NULL) {
        ESP_LOGE(TAG, "espnow_auth_build_frame: NULL argument");
        return false;
    }

    // Initialize PSA crypto subsystem (idempotent)
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return false;
    }

    uint32_t counter = 0;
    if (!next_counter(&counter)) {
        return false;
    }

    // Cleartext header: magic | counter (big-endian)
    memcpy(frame, ESPNOW_MAGIC, ESPNOW_MAGIC_LEN);
    frame[ESPNOW_MAGIC_LEN]     = (uint8_t)(counter >> 24);
    frame[ESPNOW_MAGIC_LEN + 1] = (uint8_t)(counter >> 16);
    frame[ESPNOW_MAGIC_LEN + 2] = (uint8_t)(counter >> 8);
    frame[ESPNOW_MAGIC_LEN + 3] = (uint8_t)counter;

    // Nonce: nine zero bytes then the counter, unique for as long as the counter is
    uint8_t nonce[ESPNOW_AUTH_NONCE_LEN] = {0};
    memcpy(nonce + ESPNOW_AUTH_NONCE_LEN - ESPNOW_AUTH_CTR_LEN,
            frame + ESPNOW_AUTH_HDR_LEN - ESPNOW_AUTH_CTR_LEN, ESPNOW_AUTH_CTR_LEN);

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attr, ESPNOW_AUTH_CCM_ALG);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, LMK_LEN * 8);

    psa_key_id_t key_id = 0;
    status = psa_import_key(&attr, lmk, LMK_LEN, &key_id); // The LMK is the CCM key
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key (CCM) failed: %d", (int)status);
        return false;
    }

    // PSA outputs ciphertext || MIC concatenated, straight into the tail of the frame
    uint8_t ct_and_tag[1 + ESPNOW_AUTH_MIC_LEN];
    size_t ct_and_tag_len = 0;

    status = psa_aead_encrypt(
            key_id, ESPNOW_AUTH_CCM_ALG,
            nonce, sizeof(nonce),
            frame, ESPNOW_AUTH_HDR_LEN, // Header authenticated but not encrypted
            &cmd, sizeof(cmd),
            ct_and_tag, sizeof(ct_and_tag),
            &ct_and_tag_len);

    psa_status_t destroy_status = psa_destroy_key(key_id);
    if (destroy_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_destroy_key failed: %d (key slot leak)", (int)destroy_status);
    }

    if (status != PSA_SUCCESS || ct_and_tag_len != sizeof(ct_and_tag)) {
        ESP_LOGE(TAG, "CCM encrypt failed: %d", (int)status);
        return false;
    }

    memcpy(frame + ESPNOW_AUTH_HDR_LEN, ct_and_tag, ct_and_tag_len);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Built authenticated frame, counter=%" PRIu32, counter);
#endif

    return true;
}
