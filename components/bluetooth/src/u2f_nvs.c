#include "polycast5_macros.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "u2f_nvs.h"

#define TAG "U2F_NVS"

#define U2F_NVS_NS       "u2f"
#define U2F_NVS_K_PRIV   "k_priv"
#define U2F_NVS_K_MAC    "k_mac"
#define U2F_NVS_COUNTER  "counter"
#define U2F_NVS_BLE_ADDR "ble_addr"

/**
 * Read a fixed-size blob, generating and persisting a random one if absent.
 * The handle is already open read/write so the caller can batch a single commit.
 */
static esp_err_t load_or_create_random(nvs_handle_t h, const char *key, uint8_t *out, size_t len,
        bool *created)
{
    size_t got = len;
    esp_err_t err = nvs_get_blob(h, key, out, &got);

    if (err == ESP_OK && got == len) {
        return ESP_OK;
    }

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "nvs_get_blob('%s') failed: %s", key, esp_err_to_name(err));
        return err;
    }

    // Absent or the wrong size (an older layout) - mint a fresh one
    esp_fill_random(out, len);

    err = nvs_set_blob(h, key, out, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob('%s') failed: %s", key, esp_err_to_name(err));
        return err;
    }

    *created = true;
    return ESP_OK;
}

esp_err_t u2f_nvs_load_secrets(uint8_t *k_priv, uint8_t *k_mac)
{
    if (!k_priv || !k_mac) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(U2F_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u2f_nvs_load_secrets: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    bool created = false;

    err = load_or_create_random(h, U2F_NVS_K_PRIV, k_priv, U2F_SECRET_LEN, &created);
    if (err == ESP_OK) {
        err = load_or_create_random(h, U2F_NVS_K_MAC, k_mac, U2F_SECRET_LEN, &created);
    }

    if (err == ESP_OK && created) {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "u2f_nvs_load_secrets: NVS commit failed: %s", esp_err_to_name(err));
        }
#ifdef POLYCAST5_DEBUG
        else {
            ESP_LOGI(TAG, "Generated new U2F master secrets");
        }
#endif
    }

    // Never leave half-written secrets behind: a k_priv without its k_mac would
    // silently invalidate every credential minted before the failure
    if (err != ESP_OK && created) {
        nvs_erase_key(h, U2F_NVS_K_PRIV);
        nvs_erase_key(h, U2F_NVS_K_MAC);
        nvs_commit(h);
    }

    if (err != ESP_OK) {
        memset(k_priv, 0, U2F_SECRET_LEN);
        memset(k_mac, 0, U2F_SECRET_LEN);
    }

    nvs_close(h);
    return err;
}

esp_err_t u2f_nvs_counter_next(uint32_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(U2F_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u2f_nvs_counter_next: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t counter = 0;
    err = nvs_get_u32(h, U2F_NVS_COUNTER, &counter);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "u2f_nvs_counter_next: NVS read failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    ++counter;

    // Persist before signing: a counter that survives only in RAM would repeat
    // after a power cut and read as a cloned key to the relying party
    err = nvs_set_u32(h, U2F_NVS_COUNTER, counter);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u2f_nvs_counter_next: NVS write failed: %s", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    nvs_close(h);
    *out = counter;
    return ESP_OK;
}

esp_err_t u2f_nvs_load_ble_addr(uint8_t *addr)
{
    if (!addr) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(U2F_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u2f_nvs_load_ble_addr: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    bool created = false;
    err = load_or_create_random(h, U2F_NVS_BLE_ADDR, addr, 6, &created);

    if (err == ESP_OK && created) {
        /* A BLE static random address must have both MSBs of the top octet set,
         * and the remaining 46 bits must be neither all-zeros nor all-ones.
         * ble_hs_id_set_rnd() rejects the address outright otherwise. */
        addr[5] |= 0xC0;

        uint8_t all_or = addr[0] | addr[1] | addr[2] | addr[3] | addr[4] | (addr[5] & 0x3F);
        uint8_t all_and = addr[0] & addr[1] & addr[2] & addr[3] & addr[4]
                & (uint8_t)(addr[5] | 0xC0);

        if (all_or == 0x00 || all_and == 0xFF) {
            addr[0] ^= 0x01; // Break the degenerate pattern either way
        }

        err = nvs_set_blob(h, U2F_NVS_BLE_ADDR, addr, 6);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "u2f_nvs_load_ble_addr: NVS write failed: %s", esp_err_to_name(err));
        }
#ifdef POLYCAST5_DEBUG
        else {
            ESP_LOGI(TAG, "Generated U2F BLE address %02x:%02x:%02x:%02x:%02x:%02x",
                    addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        }
#endif
    }

    nvs_close(h);
    return err;
}

esp_err_t u2f_nvs_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(U2F_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u2f_nvs_reset: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "u2f_nvs_reset: NVS erase failed: %s", esp_err_to_name(err));
    }

    nvs_close(h);
    return err;
}
