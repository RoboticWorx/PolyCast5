#include "polycast5_macros.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"

#include "u2f.h"
#include "u2f_nvs.h"

#include "psa/crypto.h"

#define TAG "U2F_CRYPTO"

#define U2F_HMAC_ALG PSA_ALG_HMAC(PSA_ALG_SHA_256)

/* RFC 6979. A repeated ECDSA nonce leaks the private key outright, so an
 * authenticator should never depend on RNG quality at signing time.
 * CONFIG_MBEDTLS_ECDSA_DETERMINISTIC=y provides this. */
#define U2F_ECDSA_ALG PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256)

#define U2F_P256_SCALAR_LEN 32
#define U2F_RAW_SIG_LEN     64 // r[32] || s[32]

// HMAC keys for the two derivation domains, imported once per persona session
static psa_key_id_t s_kid_priv = 0;
static psa_key_id_t s_kid_mac = 0;
static bool s_ready = false;

/**
 * Import a raw 32-byte secret as an HMAC-SHA256 key.
 */
static esp_err_t import_hmac_key(const uint8_t *secret, psa_key_id_t *out_kid)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, U2F_HMAC_ALG);
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, U2F_SECRET_LEN * 8);

    psa_status_t status = psa_import_key(&attr, secret, U2F_SECRET_LEN, out_kid);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key(HMAC) failed: %d", (int)status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * HMAC-SHA256(key, application || nonce) -> 32 bytes.
 *
 * Both the private key and the key-handle MAC are derived this way, from
 * different keys, which is what lets the device re-create a credential from the
 * key handle alone instead of storing one record per registration.
 */
static esp_err_t derive(psa_key_id_t kid, const uint8_t *app, const uint8_t *nonce, uint8_t *out)
{
    uint8_t input[32 + U2F_NONCE_LEN];
    memcpy(input, app, 32);
    memcpy(input + 32, nonce, U2F_NONCE_LEN);

    size_t out_len = 0;
    psa_status_t status = psa_mac_compute(kid, U2F_HMAC_ALG, input, sizeof(input),
            out, 32, &out_len);

    memset(input, 0, sizeof(input));

    if (status != PSA_SUCCESS || out_len != 32) {
        ESP_LOGE(TAG, "psa_mac_compute failed: %d", (int)status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * Constant-time comparison, so a forged key handle cannot be brute-forced one
 * byte at a time by timing the rejection.
 */
static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/**
 * Import a raw private scalar as a SECP256R1 signing key.
 * Fails if the scalar is outside [1, n-1], which the caller treats as a signal
 * to pick a new nonce and derive again.
 */
static esp_err_t import_p256_key(const uint8_t *d, psa_key_id_t *out_kid)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, U2F_ECDSA_ALG);
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);

    psa_status_t status = psa_import_key(&attr, d, U2F_P256_SCALAR_LEN, out_kid);
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * Encode one raw big-endian integer as a minimal unsigned DER INTEGER.
 * Returns bytes written.
 */
static size_t der_encode_int(const uint8_t *val, size_t len, uint8_t *out)
{
    // DER requires the minimal encoding, so drop leading zero bytes
    size_t off = 0;
    while (off < len - 1 && val[off] == 0x00) {
        off++;
    }

    size_t body = len - off;

    // A leading bit of 1 would read as negative, so pad with a zero byte
    bool pad = (val[off] & 0x80) != 0;

    size_t i = 0;
    out[i++] = 0x02;
    out[i++] = (uint8_t)(body + (pad ? 1 : 0));
    if (pad) {
        out[i++] = 0x00;
    }
    memcpy(out + i, val + off, body);

    return i + body;
}

/**
 * Wrap a raw r||s signature as DER SEQUENCE { INTEGER r, INTEGER s }.
 * PSA hands back the raw form; U2F requires the DER form.
 */
static esp_err_t raw_sig_to_der(const uint8_t *raw, uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t body[U2F_MAX_SIG_DER];
    size_t n = 0;

    n += der_encode_int(raw, 32, body + n);
    n += der_encode_int(raw + 32, 32, body + n);

    // Both integers are at most 35 bytes encoded, so the sequence body never
    // reaches the 128-byte long-form threshold and the length fits one byte
    if (out_cap < n + 2) {
        return ESP_ERR_INVALID_SIZE;
    }

    out[0] = 0x30;
    out[1] = (uint8_t)n;
    memcpy(out + 2, body, n);

    *out_len = n + 2;
    return ESP_OK;
}

/**
 * Sign a message with an already-imported P-256 key and DER-encode the result.
 */
static esp_err_t sign_der(psa_key_id_t kid, const uint8_t *msg, size_t msg_len,
        uint8_t *sig_der, size_t sig_cap, size_t *sig_len)
{
    uint8_t raw[U2F_RAW_SIG_LEN];
    size_t raw_len = 0;

    psa_status_t status = psa_sign_message(kid, U2F_ECDSA_ALG, msg, msg_len,
            raw, sizeof(raw), &raw_len);
    if (status != PSA_SUCCESS || raw_len != U2F_RAW_SIG_LEN) {
        ESP_LOGE(TAG, "psa_sign_message failed: %d", (int)status);
        return ESP_FAIL;
    }

    esp_err_t err = raw_sig_to_der(raw, sig_der, sig_cap, sig_len);
    memset(raw, 0, sizeof(raw));
    return err;
}

esp_err_t u2f_crypto_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return ESP_FAIL;
    }

    uint8_t k_priv[U2F_SECRET_LEN];
    uint8_t k_mac[U2F_SECRET_LEN];

    esp_err_t err = u2f_nvs_load_secrets(k_priv, k_mac);
    if (err != ESP_OK) {
        return err;
    }

    err = import_hmac_key(k_priv, &s_kid_priv);
    if (err == ESP_OK) {
        err = import_hmac_key(k_mac, &s_kid_mac);
    }

    // The PSA key store holds the only copies we need from here on
    memset(k_priv, 0, sizeof(k_priv));
    memset(k_mac, 0, sizeof(k_mac));

    if (err != ESP_OK) {
        u2f_crypto_deinit();
        return err;
    }

    s_ready = true;
    return ESP_OK;
}

void u2f_crypto_deinit(void)
{
    if (s_kid_priv != 0) {
        psa_destroy_key(s_kid_priv);
        s_kid_priv = 0;
    }
    if (s_kid_mac != 0) {
        psa_destroy_key(s_kid_mac);
        s_kid_mac = 0;
    }
    s_ready = false;
}

esp_err_t u2f_crypto_sha256(const uint8_t *in, size_t len, uint8_t *out)
{
    size_t out_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, in, len, out, 32, &out_len);

    if (status != PSA_SUCCESS || out_len != 32) {
        ESP_LOGE(TAG, "psa_hash_compute failed: %d", (int)status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t u2f_crypto_make_credential(const uint8_t *app, uint8_t *key_handle, uint8_t *pubkey)
{
    if (!s_ready || !app || !key_handle || !pubkey) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t nonce[U2F_NONCE_LEN];
    uint8_t d[U2F_P256_SCALAR_LEN];
    psa_key_id_t kid = 0;
    esp_err_t err = ESP_FAIL;

    /* A derived scalar outside [1, n-1] is roughly a 2^-32 event, but it still
     * has to be handled. Re-rolling the nonce keeps the credential derivable
     * from the key handle alone, which a retry counter would not. */
    for (int attempt = 0; attempt < 8; attempt++) {
        esp_fill_random(nonce, sizeof(nonce));

        err = derive(s_kid_priv, app, nonce, d);
        if (err != ESP_OK) {
            break;
        }

        if (import_p256_key(d, &kid) == ESP_OK) {
            err = ESP_OK;
            break;
        }

        ESP_LOGW(TAG, "Derived scalar out of range, re-rolling nonce");
        err = ESP_FAIL;
    }

    memset(d, 0, sizeof(d));

    if (err != ESP_OK || kid == 0) {
        memset(nonce, 0, sizeof(nonce));
        return ESP_FAIL;
    }

    size_t pub_len = 0;
    psa_status_t status = psa_export_public_key(kid, pubkey, U2F_PUBKEY_LEN, &pub_len);
    psa_destroy_key(kid);

    if (status != PSA_SUCCESS || pub_len != U2F_PUBKEY_LEN) {
        ESP_LOGE(TAG, "psa_export_public_key failed: %d (len %u)", (int)status, (unsigned)pub_len);
        memset(nonce, 0, sizeof(nonce));
        return ESP_FAIL;
    }

    // key handle = nonce || HMAC(k_mac, app || nonce)
    memcpy(key_handle, nonce, U2F_NONCE_LEN);
    err = derive(s_kid_mac, app, nonce, key_handle + U2F_NONCE_LEN);

    memset(nonce, 0, sizeof(nonce));

    if (err != ESP_OK) {
        memset(key_handle, 0, U2F_KEY_HANDLE_LEN);
        return err;
    }

    return ESP_OK;
}

esp_err_t u2f_crypto_check_credential(const uint8_t *app, const uint8_t *key_handle,
        size_t key_handle_len)
{
    if (!s_ready || !app || !key_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key_handle_len != U2F_KEY_HANDLE_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t expect[32];
    esp_err_t err = derive(s_kid_mac, app, key_handle, expect);
    if (err != ESP_OK) {
        return err;
    }

    // Binding the MAC to the application ID is what stops one site from asking
    // the key to authenticate with a credential minted for a different site
    bool ok = ct_equal(expect, key_handle + U2F_NONCE_LEN, 32);
    memset(expect, 0, sizeof(expect));

    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t u2f_crypto_sign_credential(const uint8_t *app, const uint8_t *key_handle,
        size_t key_handle_len, const uint8_t *msg, size_t msg_len,
        uint8_t *sig_der, size_t sig_cap, size_t *sig_len)
{
    esp_err_t err = u2f_crypto_check_credential(app, key_handle, key_handle_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t d[U2F_P256_SCALAR_LEN];
    err = derive(s_kid_priv, app, key_handle, d);
    if (err != ESP_OK) {
        return err;
    }

    psa_key_id_t kid = 0;
    err = import_p256_key(d, &kid);
    memset(d, 0, sizeof(d));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Re-derived scalar rejected for a handle this device issued");
        return err;
    }

    err = sign_der(kid, msg, msg_len, sig_der, sig_cap, sig_len);
    psa_destroy_key(kid);

    return err;
}

esp_err_t u2f_crypto_sign_attest(const uint8_t *msg, size_t msg_len,
        uint8_t *sig_der, size_t sig_cap, size_t *sig_len)
{
    psa_key_id_t kid = 0;
    esp_err_t err = import_p256_key(u2f_attest_key, &kid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to import attestation key");
        return err;
    }

    err = sign_der(kid, msg, msg_len, sig_der, sig_cap, sig_len);
    psa_destroy_key(kid);

    return err;
}
