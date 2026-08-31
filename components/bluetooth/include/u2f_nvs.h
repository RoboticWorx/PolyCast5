#ifndef U2F_NVS_H
#define U2F_NVS_H

#include <stdint.h>

#include "esp_err.h"

#define U2F_SECRET_LEN 32

/**
 * @brief Load the per-device master secrets, generating them on first use
 *
 * These are the only values that actually protect credentials: every U2F private
 * key is derived from k_priv, and every key handle is authenticated with k_mac.
 * Stored in encrypted NVS (CONFIG_NVS_ENCRYPTION=y).
 *
 * @param [out] k_priv U2F_SECRET_LEN bytes, key-derivation secret
 * @param [out] k_mac U2F_SECRET_LEN bytes, key-handle MAC secret
 */
esp_err_t u2f_nvs_load_secrets(uint8_t *k_priv, uint8_t *k_mac);

/**
 * @brief Increment and persist the signature counter, then return the new value
 *
 * Relying parties treat a counter that fails to advance as a cloned
 * authenticator, so this is persisted on every authentication.
 *
 * @param [out] out New counter value
 */
esp_err_t u2f_nvs_counter_next(uint32_t *out);

/**
 * @brief Load the persona's BLE static random address, generating it on first use
 *
 * The U2F persona advertises under its own address so hosts see it as a device
 * distinct from the HID keyboard: the same address presenting two different GATT
 * tables makes hosts serve a stale cached table.
 *
 * @param [out] addr 6 bytes, little-endian as NimBLE expects
 */
esp_err_t u2f_nvs_load_ble_addr(uint8_t *addr);

/**
 * @brief Erase all U2F state, invalidating every registration made with this device
 */
esp_err_t u2f_nvs_reset(void);

#endif // U2F_NVS_H
