#ifndef U2F_H
#define U2F_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* FIDO U2F second-factor authenticator over BLE.
 *
 * This is a second persona of the Bluetooth subsystem: it owns the NimBLE host
 * exactly the way ble_flood.c does, and is mutually exclusive with the HID
 * keyboard. Everything is pumped from bluetooth_task, so a single task owns the
 * radio. Transport is the FIDO GATT service 0xFFFD (U2F BT 1.2), which is
 * usable from Windows (via webauthn.dll) and Android (via Play Services).
 */

#define U2F_KEY_HANDLE_LEN 64 // nonce[32] || mac[32]
#define U2F_PUBKEY_LEN     65 // uncompressed SEC1 point: 0x04 || X[32] || Y[32]
#define U2F_NONCE_LEN      32
#define U2F_MAX_SIG_DER    72 // 0x30 len 0x02 len r[<=33] 0x02 len s[<=33]

/* A REGISTER request is 64 bytes and an AUTHENTICATE request is 129, so inbound
 * never needs much. Outbound is dominated by the attestation certificate. */
#define U2F_REQ_MAX_LEN 1024
#define U2F_RSP_MAX_LEN 2048

/* Batch attestation credentials (u2f_attest.c) */
extern const uint8_t u2f_attest_cert_der[];
extern const size_t u2f_attest_cert_der_len;
extern const uint8_t u2f_attest_key[32];

/* ===================== BLE persona (u2f_ble.c) ===================== */

/**
 * @brief Bring up the U2F BLE persona and start advertising the FIDO service
 *
 * Refuses if the HID stack or the BLE flood broadcaster owns the radio.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if the radio is busy
 */
esp_err_t u2f_ble_start(void);

/**
 * @brief Tear the U2F BLE persona down and release the radio
 */
void u2f_ble_stop(void);

/**
 * @brief Whether the U2F persona currently owns the radio
 */
bool u2f_ble_is_active(void);

/**
 * @brief Whether a host is connected and has bonded
 */
bool u2f_ble_is_connected(void);

/**
 * @brief Pump the U2F state machine, called from bluetooth_task every loop
 *
 * Never blocks: it picks up an assembled request, runs crypto, emits KEEPALIVE
 * while waiting on the user-presence test, and times the request out.
 */
void u2f_service(void);

/**
 * @brief Approve the pending user-presence test (user pressed SELECT)
 */
void u2f_presence_approve(void);

/**
 * @brief Deny the pending user-presence test (user pressed BACK)
 */
void u2f_presence_deny(void);

/**
 * @brief Whether a host is waiting on the user-presence test right now
 */
bool u2f_presence_pending(void);

/* ===================== Message layer (u2f_core.c) ===================== */

/**
 * @brief Whether this request needs a user-presence test before it can be answered
 *
 * @param [in] req APDU bytes
 * @param [in] req_len Length of req
 */
bool u2f_core_needs_presence(const uint8_t *req, size_t req_len);

/**
 * @brief Process a U2F APDU and build the response, status word included
 *
 * @param [in] req APDU bytes
 * @param [in] req_len Length of req
 * @param [out] rsp Response buffer
 * @param [in] rsp_cap Capacity of rsp
 * @param [in] presence True if the user approved the presence test
 *
 * @return Number of bytes written to rsp (always >= 2)
 */
size_t u2f_core_handle_apdu(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_cap,
        bool presence);

/* ===================== Crypto (u2f_crypto.c) ===================== */

/**
 * @brief Initialize PSA and load the per-device master secrets from NVS
 */
esp_err_t u2f_crypto_init(void);

/**
 * @brief Release the master secrets from the PSA key store
 */
void u2f_crypto_deinit(void);

/**
 * @brief SHA-256 a buffer
 *
 * @param [in] in Input bytes
 * @param [in] len Length of in
 * @param [out] out 32-byte digest
 */
esp_err_t u2f_crypto_sha256(const uint8_t *in, size_t len, uint8_t *out);

/**
 * @brief Mint a new credential for an application ID
 *
 * The private key is not stored: it is re-derived from the key handle on every
 * authentication, so credential storage stays O(1) no matter how many sites are
 * registered.
 *
 * @param [in] app Application parameter (SHA-256 of the origin)
 * @param [out] key_handle U2F_KEY_HANDLE_LEN bytes
 * @param [out] pubkey U2F_PUBKEY_LEN bytes, uncompressed
 */
esp_err_t u2f_crypto_make_credential(const uint8_t *app, uint8_t *key_handle, uint8_t *pubkey);

/**
 * @brief Check that a key handle was minted by this device for this application
 */
esp_err_t u2f_crypto_check_credential(const uint8_t *app, const uint8_t *key_handle,
        size_t key_handle_len);

/**
 * @brief Sign a message with the credential named by a key handle
 *
 * @param [out] sig_der DER-encoded ECDSA signature
 * @param [out] sig_len Bytes written to sig_der
 */
esp_err_t u2f_crypto_sign_credential(const uint8_t *app, const uint8_t *key_handle,
        size_t key_handle_len, const uint8_t *msg, size_t msg_len,
        uint8_t *sig_der, size_t sig_cap, size_t *sig_len);

/**
 * @brief Sign a message with the batch attestation key
 */
esp_err_t u2f_crypto_sign_attest(const uint8_t *msg, size_t msg_len,
        uint8_t *sig_der, size_t sig_cap, size_t *sig_len);

#endif // U2F_H
