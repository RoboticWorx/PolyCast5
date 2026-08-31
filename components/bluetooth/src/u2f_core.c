#include "polycast5_macros.h"

#include <string.h>

#include "esp_log.h"

#include "u2f.h"
#include "u2f_nvs.h"

#define TAG "U2F_CORE"

/* U2F raw message instructions */
#define U2F_INS_REGISTER     0x01
#define U2F_INS_AUTHENTICATE 0x02
#define U2F_INS_VERSION      0x03

/* AUTHENTICATE control byte */
#define U2F_AUTH_ENFORCE     0x03 // enforce-user-presence-and-sign
#define U2F_AUTH_CHECK_ONLY  0x07 // check-only
#define U2F_AUTH_NO_ENFORCE  0x08 // dont-enforce-user-presence-and-sign

/* ISO 7816-4 status words */
#define SW_NO_ERROR                 0x9000
#define SW_CONDITIONS_NOT_SATISFIED 0x6985
#define SW_WRONG_DATA               0x6A80
#define SW_WRONG_LENGTH             0x6700
#define SW_CLA_NOT_SUPPORTED        0x6E00
#define SW_INS_NOT_SUPPORTED        0x6D00

#define U2F_REGISTER_ID    0x05 // Legacy magic byte that opens a registration response
#define U2F_AUTH_FLAG_TUP  0x01 // User presence verified
#define U2F_CHALLENGE_LEN  32
#define U2F_APPLICATION_LEN 32

#define U2F_VERSION_STR "U2F_V2"

/**
 * A parsed command APDU. Data points into the caller buffer, never copied.
 */
typedef struct {
    uint8_t cla;
    uint8_t ins;
    uint8_t p1;
    uint8_t p2;
    const uint8_t *data;
    size_t data_len;
} u2f_apdu_t;

/**
 * Parse a command APDU in either the short or extended length encoding.
 *
 * The BLE transport mandates extended length, but accepting the short form too
 * costs nothing and keeps bring-up tools (nRF Connect, ad-hoc scripts) usable.
 */
static bool parse_apdu(const uint8_t *req, size_t req_len, u2f_apdu_t *out)
{
    if (req_len < 4) {
        return false;
    }

    out->cla = req[0];
    out->ins = req[1];
    out->p1 = req[2];
    out->p2 = req[3];
    out->data = NULL;
    out->data_len = 0;

    if (req_len == 4) {
        return true; // No Lc, no Le
    }
    if (req_len == 5) {
        return true; // Short Le only, no payload
    }

    if (req[4] != 0x00) {
        // Short form: Lc is one byte
        size_t lc = req[4];
        if (req_len < 5 + lc) {
            return false;
        }
        out->data = req + 5;
        out->data_len = lc;
        return true;
    }

    // Extended form: a zero byte then two more length bytes
    if (req_len < 7) {
        return false;
    }

    /* With no payload those two bytes are Le, not Lc. Exactly seven bytes is the
     * only way that case can arise, so length alone disambiguates it. Reading
     * them as Lc would reject a bare U2F_VERSION, which clients send this way. */
    if (req_len == 7) {
        return true;
    }

    size_t lc = ((size_t)req[5] << 8) | req[6];
    if (req_len < 7 + lc) {
        return false;
    }

    out->data = req + 7;
    out->data_len = lc;
    return true;
}

/**
 * Write a bare status word, the shortest possible response.
 */
static size_t emit_sw(uint8_t *rsp, size_t rsp_cap, uint16_t sw)
{
    if (rsp_cap < 2) {
        return 0;
    }

    rsp[0] = (uint8_t)(sw >> 8);
    rsp[1] = (uint8_t)(sw & 0xFF);
    return 2;
}

bool u2f_core_needs_presence(const uint8_t *req, size_t req_len)
{
    u2f_apdu_t apdu;
    if (!parse_apdu(req, req_len, &apdu) || apdu.cla != 0x00) {
        return false;
    }

    if (apdu.ins == U2F_INS_REGISTER) {
        return true;
    }

    if (apdu.ins == U2F_INS_AUTHENTICATE) {
        /* check-only is a probe that must answer immediately, and this device
         * declines silent signing outright, so only the enforcing variant ever
         * prompts the user. */
        return apdu.p1 == U2F_AUTH_ENFORCE;
    }

    return false;
}

/**
 * U2F_REGISTER: mint a credential for the application and attest to it.
 */
static size_t handle_register(const u2f_apdu_t *apdu, uint8_t *rsp, size_t rsp_cap, bool presence)
{
    if (apdu->data_len != U2F_CHALLENGE_LEN + U2F_APPLICATION_LEN) {
        return emit_sw(rsp, rsp_cap, SW_WRONG_LENGTH);
    }

    if (!presence) {
        return emit_sw(rsp, rsp_cap, SW_CONDITIONS_NOT_SATISFIED);
    }

    const uint8_t *challenge = apdu->data;
    const uint8_t *application = apdu->data + U2F_CHALLENGE_LEN;

    uint8_t key_handle[U2F_KEY_HANDLE_LEN];
    uint8_t pubkey[U2F_PUBKEY_LEN];

    if (u2f_crypto_make_credential(application, key_handle, pubkey) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mint credential");
        return emit_sw(rsp, rsp_cap, SW_CONDITIONS_NOT_SATISFIED);
    }

    /* Attestation signature covers a leading 0x00 reserved byte, then the
     * application, challenge, key handle and public key. */
    uint8_t to_sign[1 + U2F_APPLICATION_LEN + U2F_CHALLENGE_LEN + U2F_KEY_HANDLE_LEN
            + U2F_PUBKEY_LEN];
    size_t n = 0;

    to_sign[n++] = 0x00;
    memcpy(to_sign + n, application, U2F_APPLICATION_LEN);
    n += U2F_APPLICATION_LEN;
    memcpy(to_sign + n, challenge, U2F_CHALLENGE_LEN);
    n += U2F_CHALLENGE_LEN;
    memcpy(to_sign + n, key_handle, U2F_KEY_HANDLE_LEN);
    n += U2F_KEY_HANDLE_LEN;
    memcpy(to_sign + n, pubkey, U2F_PUBKEY_LEN);
    n += U2F_PUBKEY_LEN;

    uint8_t sig[U2F_MAX_SIG_DER];
    size_t sig_len = 0;

    if (u2f_crypto_sign_attest(to_sign, n, sig, sizeof(sig), &sig_len) != ESP_OK) {
        ESP_LOGE(TAG, "Attestation signature failed");
        return emit_sw(rsp, rsp_cap, SW_CONDITIONS_NOT_SATISFIED);
    }

    size_t total = 1 + U2F_PUBKEY_LEN + 1 + U2F_KEY_HANDLE_LEN + u2f_attest_cert_der_len
            + sig_len + 2;
    if (rsp_cap < total) {
        ESP_LOGE(TAG, "Registration response needs %u bytes, have %u",
                (unsigned)total, (unsigned)rsp_cap);
        return emit_sw(rsp, rsp_cap, SW_WRONG_LENGTH);
    }

    size_t o = 0;
    rsp[o++] = U2F_REGISTER_ID;
    memcpy(rsp + o, pubkey, U2F_PUBKEY_LEN);
    o += U2F_PUBKEY_LEN;
    rsp[o++] = (uint8_t)U2F_KEY_HANDLE_LEN;
    memcpy(rsp + o, key_handle, U2F_KEY_HANDLE_LEN);
    o += U2F_KEY_HANDLE_LEN;
    memcpy(rsp + o, u2f_attest_cert_der, u2f_attest_cert_der_len);
    o += u2f_attest_cert_der_len;
    memcpy(rsp + o, sig, sig_len);
    o += sig_len;
    o += emit_sw(rsp + o, rsp_cap - o, SW_NO_ERROR);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Registered a new credential (%u byte response)", (unsigned)o);
#endif

    return o;
}

/**
 * U2F_AUTHENTICATE: prove possession of the credential named by the key handle.
 */
static size_t handle_authenticate(const u2f_apdu_t *apdu, uint8_t *rsp, size_t rsp_cap,
        bool presence)
{
    // challenge[32] || application[32] || khLen[1] || keyHandle
    if (apdu->data_len < U2F_CHALLENGE_LEN + U2F_APPLICATION_LEN + 1) {
        return emit_sw(rsp, rsp_cap, SW_WRONG_LENGTH);
    }

    const uint8_t *challenge = apdu->data;
    const uint8_t *application = apdu->data + U2F_CHALLENGE_LEN;
    size_t kh_len = apdu->data[U2F_CHALLENGE_LEN + U2F_APPLICATION_LEN];
    const uint8_t *key_handle = apdu->data + U2F_CHALLENGE_LEN + U2F_APPLICATION_LEN + 1;

    if (apdu->data_len != U2F_CHALLENGE_LEN + U2F_APPLICATION_LEN + 1 + kh_len) {
        return emit_sw(rsp, rsp_cap, SW_WRONG_LENGTH);
    }

    bool known = u2f_crypto_check_credential(application, key_handle, kh_len) == ESP_OK;

    /* check-only is how a client asks whether this key holds the credential.
     * A known handle answers 0x6985, which is the spec way of saying yes. */
    if (apdu->p1 == U2F_AUTH_CHECK_ONLY) {
        return emit_sw(rsp, rsp_cap, known ? SW_CONDITIONS_NOT_SATISFIED : SW_WRONG_DATA);
    }

    if (!known) {
        return emit_sw(rsp, rsp_cap, SW_WRONG_DATA);
    }

    /* Signing without a presence test would let a bonded host harvest
     * assertions silently, which defeats the point of a physical key. Decline
     * rather than sign, which is what clients expect from a key that has no
     * silent-authentication support. */
    if (apdu->p1 == U2F_AUTH_NO_ENFORCE) {
        return emit_sw(rsp, rsp_cap, SW_CONDITIONS_NOT_SATISFIED);
    }

    if (apdu->p1 != U2F_AUTH_ENFORCE) {
        return emit_sw(rsp, rsp_cap, SW_WRONG_DATA);
    }

    if (!presence) {
        return emit_sw(rsp, rsp_cap, SW_CONDITIONS_NOT_SATISFIED);
    }

    uint32_t counter = 0;
    if (u2f_nvs_counter_next(&counter) != ESP_OK) {
        ESP_LOGE(TAG, "Counter update failed, refusing to sign");
        return emit_sw(rsp, rsp_cap, SW_CONDITIONS_NOT_SATISFIED);
    }

    // application || flags || counter || challenge
    uint8_t to_sign[U2F_APPLICATION_LEN + 1 + 4 + U2F_CHALLENGE_LEN];
    size_t n = 0;

    memcpy(to_sign + n, application, U2F_APPLICATION_LEN);
    n += U2F_APPLICATION_LEN;
    to_sign[n++] = U2F_AUTH_FLAG_TUP;
    to_sign[n++] = (uint8_t)(counter >> 24);
    to_sign[n++] = (uint8_t)(counter >> 16);
    to_sign[n++] = (uint8_t)(counter >> 8);
    to_sign[n++] = (uint8_t)counter;
    memcpy(to_sign + n, challenge, U2F_CHALLENGE_LEN);
    n += U2F_CHALLENGE_LEN;

    uint8_t sig[U2F_MAX_SIG_DER];
    size_t sig_len = 0;

    if (u2f_crypto_sign_credential(application, key_handle, kh_len, to_sign, n,
            sig, sizeof(sig), &sig_len) != ESP_OK) {
        ESP_LOGE(TAG, "Assertion signature failed");
        return emit_sw(rsp, rsp_cap, SW_CONDITIONS_NOT_SATISFIED);
    }

    if (rsp_cap < 1 + 4 + sig_len + 2) {
        return emit_sw(rsp, rsp_cap, SW_WRONG_LENGTH);
    }

    // The response repeats the same flags and counter the signature commits to
    size_t o = 0;
    rsp[o++] = U2F_AUTH_FLAG_TUP;
    rsp[o++] = (uint8_t)(counter >> 24);
    rsp[o++] = (uint8_t)(counter >> 16);
    rsp[o++] = (uint8_t)(counter >> 8);
    rsp[o++] = (uint8_t)counter;
    memcpy(rsp + o, sig, sig_len);
    o += sig_len;
    o += emit_sw(rsp + o, rsp_cap - o, SW_NO_ERROR);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Signed assertion, counter=%u", (unsigned)counter);
#endif

    return o;
}

size_t u2f_core_handle_apdu(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_cap,
        bool presence)
{
    u2f_apdu_t apdu;

    if (!parse_apdu(req, req_len, &apdu)) {
        return emit_sw(rsp, rsp_cap, SW_WRONG_LENGTH);
    }

    if (apdu.cla != 0x00) {
        return emit_sw(rsp, rsp_cap, SW_CLA_NOT_SUPPORTED);
    }

    switch (apdu.ins) {
    case U2F_INS_REGISTER:
        return handle_register(&apdu, rsp, rsp_cap, presence);

    case U2F_INS_AUTHENTICATE:
        return handle_authenticate(&apdu, rsp, rsp_cap, presence);

    case U2F_INS_VERSION:
        if (rsp_cap < sizeof(U2F_VERSION_STR) - 1 + 2) {
            return emit_sw(rsp, rsp_cap, SW_WRONG_LENGTH);
        }
        memcpy(rsp, U2F_VERSION_STR, sizeof(U2F_VERSION_STR) - 1);
        return (sizeof(U2F_VERSION_STR) - 1)
                + emit_sw(rsp + sizeof(U2F_VERSION_STR) - 1,
                        rsp_cap - (sizeof(U2F_VERSION_STR) - 1), SW_NO_ERROR);

    default:
        return emit_sw(rsp, rsp_cap, SW_INS_NOT_SUPPORTED);
    }
}
