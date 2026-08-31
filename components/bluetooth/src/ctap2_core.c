#include "polycast5_macros.h"

#include <string.h>

#include "esp_log.h"

#include "ctap2.h"
#include "ctap2_cbor.h"
#include "u2f.h"
#include "u2f_nvs.h"

#define TAG "CTAP2_CORE"

#define CTAP2_ES256_ALG (-7) // COSE identifier for ECDSA w/ SHA-256 on P-256

#define CTAP2_RPID_HASH_LEN   32
#define CTAP2_CLIENT_DATA_LEN 32

/* rpIdHash | flags | signCount | aaguid | credIdLen | credId | COSE key.
 * The COSE key for P-256 is a fixed 77 bytes. Sized with room to append the
 * client data hash, because the signature covers authData || clientDataHash and
 * building it in place avoids a second copy of the whole structure. */
#define CTAP2_AUTHDATA_MAX (32 + 1 + 4 + CTAP2_AAGUID_LEN + 2 + U2F_KEY_HANDLE_LEN + 96)
#define CTAP2_SIGN_BUF_MAX (CTAP2_AUTHDATA_MAX + CTAP2_CLIENT_DATA_LEN)

/**
 * Emit a bare status byte, which is the whole response for every error.
 */
static size_t emit_status(uint8_t *rsp, size_t rsp_cap, uint8_t status)
{
    if (rsp_cap < 1) {
        return 0;
    }
    rsp[0] = status;
    return 1;
}

/**
 * Compare a borrowed CBOR text string, which is not NUL-terminated, to a literal.
 */
static bool text_is(const char *p, size_t n, const char *lit)
{
    size_t l = strlen(lit);
    return n == l && memcmp(p, lit, l) == 0;
}

/**
 * Append the COSE_Key encoding of an uncompressed P-256 point.
 *
 * {1: 2 (EC2), 3: -7 (ES256), -1: 1 (P-256), -2: x, -3: y}, keys in the
 * canonical order CTAP2 requires: non-negative ascending, then negative.
 */
static void write_cose_key(cbor_writer_t *w, const uint8_t *pubkey)
{
    cbor_w_map(w, 5);
    cbor_w_int(w, 1);  cbor_w_int(w, 2);
    cbor_w_int(w, 3);  cbor_w_int(w, CTAP2_ES256_ALG);
    cbor_w_int(w, -1); cbor_w_int(w, 1);
    cbor_w_int(w, -2); cbor_w_bytes(w, pubkey + 1, 32);      // x
    cbor_w_int(w, -3); cbor_w_bytes(w, pubkey + 1 + 32, 32); // y
}

/* ===================== authenticatorGetInfo ===================== */

static size_t handle_get_info(uint8_t *rsp, size_t rsp_cap)
{
    cbor_writer_t w;
    cbor_w_init(&w, rsp + 1, rsp_cap - 1);

    cbor_w_map(&w, 6);

    // 0x01 versions: CTAP1 is still offered, so a client may pick either
    cbor_w_int(&w, 1);
    cbor_w_array(&w, 2);
    cbor_w_text(&w, "U2F_V2");
    cbor_w_text(&w, "FIDO_2_0");

    // 0x03 aaguid
    cbor_w_int(&w, 3);
    cbor_w_bytes(&w, ctap2_aaguid, CTAP2_AAGUID_LEN);

    /* 0x04 options. "uv" is omitted entirely, which tells the client this
     * authenticator has no user verification rather than having one that is
     * unconfigured. Text keys sort by length first, so rk and up precede plat. */
    cbor_w_int(&w, 4);
    cbor_w_map(&w, 3);
    cbor_w_text(&w, "rk");   cbor_w_bool(&w, false); // No discoverable credentials
    cbor_w_text(&w, "up");   cbor_w_bool(&w, true);  // User presence via the button
    cbor_w_text(&w, "plat"); cbor_w_bool(&w, false); // Roaming, not platform-bound

    // 0x05 maxMsgSize, bounded by the reassembly buffer
    cbor_w_int(&w, 5);
    cbor_w_int(&w, (int64_t)U2F_REQ_MAX_LEN);

    // 0x09 transports
    cbor_w_int(&w, 9);
    cbor_w_array(&w, 1);
    cbor_w_text(&w, "ble");

    // 0x0A algorithms
    cbor_w_int(&w, 10);
    cbor_w_array(&w, 1);
    cbor_w_map(&w, 2);
    cbor_w_text(&w, "alg");  cbor_w_int(&w, CTAP2_ES256_ALG);
    cbor_w_text(&w, "type"); cbor_w_text(&w, "public-key");

    if (!cbor_w_ok(&w)) {
        ESP_LOGE(TAG, "getInfo response did not fit");
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    rsp[0] = CTAP2_OK;
    return 1 + w.len;
}

/* ===================== Shared request pieces ===================== */

/**
 * Read a PublicKeyCredentialDescriptor and hand back its id.
 * Entries whose type is not public-key are reported so the caller can skip them.
 */
static int read_cred_descriptor(cbor_reader_t *r, const uint8_t **id, size_t *id_len,
        bool *is_public_key)
{
    size_t n;
    int rc = cbor_r_map(r, &n);
    if (rc != CBOR_OK) {
        return rc;
    }

    *id = NULL;
    *id_len = 0;
    *is_public_key = false;

    for (size_t i = 0; i < n; i++) {
        const char *k;
        size_t klen;
        if (cbor_r_text(r, &k, &klen) != CBOR_OK) {
            // Non-text key: not a descriptor we understand, step over the pair
            if (cbor_r_skip(r) != CBOR_OK || cbor_r_skip(r) != CBOR_OK) {
                return CBOR_ERR_BAD;
            }
            continue;
        }

        if (text_is(k, klen, "id")) {
            rc = cbor_r_bytes(r, id, id_len);
            if (rc != CBOR_OK) {
                return rc;
            }
        } else if (text_is(k, klen, "type")) {
            const char *t;
            size_t tlen;
            rc = cbor_r_text(r, &t, &tlen);
            if (rc != CBOR_OK) {
                return rc;
            }
            *is_public_key = text_is(t, tlen, "public-key");
        } else if (cbor_r_skip(r) != CBOR_OK) {
            return CBOR_ERR_BAD;
        }
    }

    return CBOR_OK;
}

/**
 * Read an options map, recording only the flags this build acts on.
 */
static int read_options(cbor_reader_t *r, bool *rk, bool *uv, bool *up, bool *up_present)
{
    size_t n;
    int rc = cbor_r_map(r, &n);
    if (rc != CBOR_OK) {
        return rc;
    }

    for (size_t i = 0; i < n; i++) {
        const char *k;
        size_t klen;
        if (cbor_r_text(r, &k, &klen) != CBOR_OK) {
            if (cbor_r_skip(r) != CBOR_OK || cbor_r_skip(r) != CBOR_OK) {
                return CBOR_ERR_BAD;
            }
            continue;
        }

        bool v;
        if (text_is(k, klen, "rk") && rk) {
            if (cbor_r_bool(r, &v) != CBOR_OK) return CBOR_ERR_TYPE;
            *rk = v;
        } else if (text_is(k, klen, "uv") && uv) {
            if (cbor_r_bool(r, &v) != CBOR_OK) return CBOR_ERR_TYPE;
            *uv = v;
        } else if (text_is(k, klen, "up") && up) {
            if (cbor_r_bool(r, &v) != CBOR_OK) return CBOR_ERR_TYPE;
            *up = v;
            if (up_present) *up_present = true;
        } else if (cbor_r_skip(r) != CBOR_OK) {
            return CBOR_ERR_BAD;
        }
    }

    return CBOR_OK;
}

/**
 * Build the fixed head of authenticatorData: rpIdHash, flags, signature counter.
 * Returns bytes written, or 0 if the counter could not be advanced.
 */
static size_t write_authdata_head(uint8_t *out, const uint8_t *rp_id_hash, uint8_t flags)
{
    uint32_t counter = 0;
    if (u2f_nvs_counter_next(&counter) != ESP_OK) {
        ESP_LOGE(TAG, "Counter update failed, refusing to sign");
        return 0;
    }

    size_t n = 0;
    memcpy(out + n, rp_id_hash, CTAP2_RPID_HASH_LEN);
    n += CTAP2_RPID_HASH_LEN;
    out[n++] = flags;
    out[n++] = (uint8_t)(counter >> 24);
    out[n++] = (uint8_t)(counter >> 16);
    out[n++] = (uint8_t)(counter >> 8);
    out[n++] = (uint8_t)counter;

    return n;
}

/* ===================== authenticatorMakeCredential ===================== */

static size_t handle_make_credential(const uint8_t *params, size_t params_len,
        uint8_t *rsp, size_t rsp_cap, bool presence)
{
    cbor_reader_t r;
    cbor_r_init(&r, params, params_len);

    size_t nkeys;
    if (cbor_r_map(&r, &nkeys) != CBOR_OK) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
    }

    const uint8_t *client_data = NULL;
    size_t client_data_len = 0;
    const char *rp_id = NULL;
    size_t rp_id_len = 0;
    bool have_user = false;
    bool es256_offered = false;
    bool opt_rk = false, opt_uv = false;
    bool pin_auth_present = false;

    /* The exclude list has to be checked against the RP, which may be parsed
     * after it, so remember where it starts and walk it once the id is known. */
    const uint8_t *exclude_at = NULL;
    size_t exclude_len = 0;

    for (size_t i = 0; i < nkeys; i++) {
        uint64_t key;
        if (cbor_r_uint(&r, &key) != CBOR_OK) {
            return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
        }

        switch (key) {
        case 1:
            if (cbor_r_bytes(&r, &client_data, &client_data_len) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
            }
            break;

        case 2: { // rp
            size_t n;
            if (cbor_r_map(&r, &n) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
            }
            for (size_t j = 0; j < n; j++) {
                const char *k;
                size_t klen;
                if (cbor_r_text(&r, &k, &klen) != CBOR_OK) {
                    if (cbor_r_skip(&r) != CBOR_OK || cbor_r_skip(&r) != CBOR_OK) {
                        return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
                    }
                    continue;
                }
                if (text_is(k, klen, "id")) {
                    if (cbor_r_text(&r, &rp_id, &rp_id_len) != CBOR_OK) {
                        return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
                    }
                } else if (cbor_r_skip(&r) != CBOR_OK) {
                    return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
                }
            }
            break;
        }

        case 3: // user, required to be present but nothing in it is stored
            if (cbor_r_skip(&r) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
            }
            have_user = true;
            break;

        case 4: { // pubKeyCredParams
            size_t n;
            if (cbor_r_array(&r, &n) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
            }
            for (size_t j = 0; j < n; j++) {
                size_t m;
                if (cbor_r_map(&r, &m) != CBOR_OK) {
                    return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
                }
                int64_t alg = 0;
                bool is_pk = false;
                for (size_t q = 0; q < m; q++) {
                    const char *k;
                    size_t klen;
                    if (cbor_r_text(&r, &k, &klen) != CBOR_OK) {
                        if (cbor_r_skip(&r) != CBOR_OK || cbor_r_skip(&r) != CBOR_OK) {
                            return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
                        }
                        continue;
                    }
                    if (text_is(k, klen, "alg")) {
                        if (cbor_r_int(&r, &alg) != CBOR_OK) {
                            return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
                        }
                    } else if (text_is(k, klen, "type")) {
                        const char *t;
                        size_t tlen;
                        if (cbor_r_text(&r, &t, &tlen) != CBOR_OK) {
                            return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
                        }
                        is_pk = text_is(t, tlen, "public-key");
                    } else if (cbor_r_skip(&r) != CBOR_OK) {
                        return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
                    }
                }
                if (is_pk && alg == CTAP2_ES256_ALG) {
                    es256_offered = true;
                }
            }
            break;
        }

        case 5: // excludeList, revisited once rp.id is known
            exclude_at = params + r.pos;
            if (cbor_r_skip(&r) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
            }
            exclude_len = (size_t)(params + r.pos - exclude_at);
            break;

        case 7:
            if (read_options(&r, &opt_rk, &opt_uv, NULL, NULL) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
            }
            break;

        case 8: // pinAuth
            pin_auth_present = true;
            if (cbor_r_skip(&r) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
            }
            break;

        default: // extensions and anything newer than this build
            if (cbor_r_skip(&r) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
            }
            break;
        }
    }

    if (!client_data || client_data_len != CTAP2_CLIENT_DATA_LEN || !rp_id || !have_user) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_MISSING_PARAMETER);
    }

    // No PIN is supported, so any pinAuth means the client believes one is set
    if (pin_auth_present) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_PIN_NOT_SET);
    }
    if (opt_rk || opt_uv) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_UNSUPPORTED_OPTION);
    }
    if (!es256_offered) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_UNSUPPORTED_ALGORITHM);
    }

    uint8_t rp_id_hash[CTAP2_RPID_HASH_LEN];
    if (u2f_crypto_sha256((const uint8_t *)rp_id, rp_id_len, rp_id_hash) != ESP_OK) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    /* An excluded credential means this RP already has one on this device. The
     * spec still wants the presence test first, which the caller has done. */
    if (exclude_at && exclude_len) {
        cbor_reader_t er;
        cbor_r_init(&er, exclude_at, exclude_len);
        size_t n;
        if (cbor_r_array(&er, &n) == CBOR_OK) {
            for (size_t i = 0; i < n; i++) {
                const uint8_t *id;
                size_t id_len;
                bool is_pk;
                if (read_cred_descriptor(&er, &id, &id_len, &is_pk) != CBOR_OK) {
                    break;
                }
                if (is_pk && id
                        && u2f_crypto_check_credential(rp_id_hash, id, id_len) == ESP_OK) {
                    return emit_status(rsp, rsp_cap, CTAP2_ERR_CREDENTIAL_EXCLUDED);
                }
            }
        }
    }

    if (!presence) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_OPERATION_DENIED);
    }

    uint8_t key_handle[U2F_KEY_HANDLE_LEN];
    uint8_t pubkey[U2F_PUBKEY_LEN];
    if (u2f_crypto_make_credential(rp_id_hash, key_handle, pubkey) != ESP_OK) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    // authData, with room to append the client data hash for signing
    uint8_t authdata[CTAP2_SIGN_BUF_MAX];
    size_t n = write_authdata_head(authdata, rp_id_hash, CTAP2_FLAG_UP | CTAP2_FLAG_AT);
    if (n == 0) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    memcpy(authdata + n, ctap2_aaguid, CTAP2_AAGUID_LEN);
    n += CTAP2_AAGUID_LEN;
    authdata[n++] = (uint8_t)(U2F_KEY_HANDLE_LEN >> 8);
    authdata[n++] = (uint8_t)(U2F_KEY_HANDLE_LEN & 0xFF);
    memcpy(authdata + n, key_handle, U2F_KEY_HANDLE_LEN);
    n += U2F_KEY_HANDLE_LEN;

    cbor_writer_t kw;
    cbor_w_init(&kw, authdata + n, sizeof(authdata) - n - CTAP2_CLIENT_DATA_LEN);
    write_cose_key(&kw, pubkey);
    if (!cbor_w_ok(&kw)) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }
    n += kw.len;

    size_t authdata_len = n;

    // Packed attestation signs authData || clientDataHash with the batch key
    memcpy(authdata + authdata_len, client_data, CTAP2_CLIENT_DATA_LEN);

    uint8_t sig[U2F_MAX_SIG_DER];
    size_t sig_len = 0;
    if (u2f_crypto_sign_attest(authdata, authdata_len + CTAP2_CLIENT_DATA_LEN,
            sig, sizeof(sig), &sig_len) != ESP_OK) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    cbor_writer_t w;
    cbor_w_init(&w, rsp + 1, rsp_cap - 1);

    cbor_w_map(&w, 3);
    cbor_w_int(&w, 1); cbor_w_text(&w, "packed");
    cbor_w_int(&w, 2); cbor_w_bytes(&w, authdata, authdata_len);
    cbor_w_int(&w, 3);
    cbor_w_map(&w, 3);
    cbor_w_text(&w, "alg"); cbor_w_int(&w, CTAP2_ES256_ALG);
    cbor_w_text(&w, "sig"); cbor_w_bytes(&w, sig, sig_len);
    cbor_w_text(&w, "x5c");
    cbor_w_array(&w, 1);
    cbor_w_bytes(&w, u2f_attest_cert_der, u2f_attest_cert_der_len);

    if (!cbor_w_ok(&w)) {
        ESP_LOGE(TAG, "makeCredential response did not fit");
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Made a credential for %.*s (%u byte response)",
            (int)rp_id_len, rp_id, (unsigned)(1 + w.len));
#endif

    rsp[0] = CTAP2_OK;
    return 1 + w.len;
}

/* ===================== authenticatorGetAssertion ===================== */

static size_t handle_get_assertion(const uint8_t *params, size_t params_len,
        uint8_t *rsp, size_t rsp_cap, bool presence)
{
    cbor_reader_t r;
    cbor_r_init(&r, params, params_len);

    size_t nkeys;
    if (cbor_r_map(&r, &nkeys) != CBOR_OK) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
    }

    const char *rp_id = NULL;
    size_t rp_id_len = 0;
    const uint8_t *client_data = NULL;
    size_t client_data_len = 0;
    const uint8_t *allow_at = NULL;
    size_t allow_len = 0;
    bool opt_uv = false, opt_up = true, up_present = false;
    bool pin_auth_present = false;

    for (size_t i = 0; i < nkeys; i++) {
        uint64_t key;
        if (cbor_r_uint(&r, &key) != CBOR_OK) {
            return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
        }

        switch (key) {
        case 1:
            if (cbor_r_text(&r, &rp_id, &rp_id_len) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
            }
            break;

        case 2:
            if (cbor_r_bytes(&r, &client_data, &client_data_len) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
            }
            break;

        case 3: // allowList
            allow_at = params + r.pos;
            if (cbor_r_skip(&r) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
            }
            allow_len = (size_t)(params + r.pos - allow_at);
            break;

        case 5:
            if (read_options(&r, NULL, &opt_uv, &opt_up, &up_present) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
            }
            break;

        case 6:
            pin_auth_present = true;
            if (cbor_r_skip(&r) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
            }
            break;

        default:
            if (cbor_r_skip(&r) != CBOR_OK) {
                return emit_status(rsp, rsp_cap, CTAP2_ERR_INVALID_CBOR);
            }
            break;
        }
    }

    if (!rp_id || !client_data || client_data_len != CTAP2_CLIENT_DATA_LEN) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_MISSING_PARAMETER);
    }
    if (pin_auth_present) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_PIN_NOT_SET);
    }
    if (opt_uv) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_UNSUPPORTED_OPTION);
    }

    /* up:false asks for a signature with no user interaction. Producing one
     * would let a bonded host harvest assertions silently, which is the whole
     * thing a physical key exists to prevent. */
    if (up_present && !opt_up) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_UNSUPPORTED_OPTION);
    }

    // Without discoverable credentials there is nothing to find without a list
    if (!allow_at || !allow_len) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_NO_CREDENTIALS);
    }

    uint8_t rp_id_hash[CTAP2_RPID_HASH_LEN];
    if (u2f_crypto_sha256((const uint8_t *)rp_id, rp_id_len, rp_id_hash) != ESP_OK) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    // First entry in the list that this device actually minted for this RP wins
    const uint8_t *cred_id = NULL;
    size_t cred_id_len = 0;
    {
        cbor_reader_t ar;
        cbor_r_init(&ar, allow_at, allow_len);
        size_t n;
        if (cbor_r_array(&ar, &n) != CBOR_OK) {
            return emit_status(rsp, rsp_cap, CTAP2_ERR_CBOR_UNEXPECTED_TYPE);
        }
        for (size_t i = 0; i < n; i++) {
            const uint8_t *id;
            size_t id_len;
            bool is_pk;
            if (read_cred_descriptor(&ar, &id, &id_len, &is_pk) != CBOR_OK) {
                break;
            }
            if (is_pk && id && u2f_crypto_check_credential(rp_id_hash, id, id_len) == ESP_OK) {
                cred_id = id;
                cred_id_len = id_len;
                break;
            }
        }
    }

    if (!cred_id) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_NO_CREDENTIALS);
    }

    if (!presence) {
        return emit_status(rsp, rsp_cap, CTAP2_ERR_OPERATION_DENIED);
    }

    // Assertion authData carries no attested credential data, so no AT flag
    uint8_t authdata[CTAP2_SIGN_BUF_MAX];
    size_t authdata_len = write_authdata_head(authdata, rp_id_hash, CTAP2_FLAG_UP);
    if (authdata_len == 0) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    memcpy(authdata + authdata_len, client_data, CTAP2_CLIENT_DATA_LEN);

    uint8_t sig[U2F_MAX_SIG_DER];
    size_t sig_len = 0;
    if (u2f_crypto_sign_credential(rp_id_hash, cred_id, cred_id_len,
            authdata, authdata_len + CTAP2_CLIENT_DATA_LEN,
            sig, sizeof(sig), &sig_len) != ESP_OK) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

    cbor_writer_t w;
    cbor_w_init(&w, rsp + 1, rsp_cap - 1);

    cbor_w_map(&w, 3);
    cbor_w_int(&w, 1);
    cbor_w_map(&w, 2);
    cbor_w_text(&w, "id");   cbor_w_bytes(&w, cred_id, cred_id_len);
    cbor_w_text(&w, "type"); cbor_w_text(&w, "public-key");
    cbor_w_int(&w, 2); cbor_w_bytes(&w, authdata, authdata_len);
    cbor_w_int(&w, 3); cbor_w_bytes(&w, sig, sig_len);

    if (!cbor_w_ok(&w)) {
        ESP_LOGE(TAG, "getAssertion response did not fit");
        return emit_status(rsp, rsp_cap, CTAP1_ERR_OTHER);
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Signed an assertion for %.*s", (int)rp_id_len, rp_id);
#endif

    rsp[0] = CTAP2_OK;
    return 1 + w.len;
}

/* ===================== Entry points ===================== */

bool ctap2_core_needs_presence(const uint8_t *req, size_t req_len)
{
    if (req_len < 1) {
        return false;
    }

    if (req[0] == CTAP2_CMD_MAKE_CREDENTIAL) {
        return true;
    }

    if (req[0] == CTAP2_CMD_GET_ASSERTION) {
        /* A request that will be refused anyway must not park on the button.
         * up:false is declined outright, and without an allowList there is no
         * credential to assert, so neither case should prompt. */
        cbor_reader_t r;
        cbor_r_init(&r, req + 1, req_len - 1);

        size_t nkeys;
        if (cbor_r_map(&r, &nkeys) != CBOR_OK) {
            return false;
        }

        bool has_allow_list = false;
        bool opt_up = true, up_present = false, opt_uv = false;

        for (size_t i = 0; i < nkeys; i++) {
            uint64_t key;
            if (cbor_r_uint(&r, &key) != CBOR_OK) {
                return false;
            }
            if (key == 3) {
                size_t n;
                size_t save = r.pos;
                if (cbor_r_array(&r, &n) == CBOR_OK && n > 0) {
                    has_allow_list = true;
                }
                r.pos = save;
                if (cbor_r_skip(&r) != CBOR_OK) {
                    return false;
                }
            } else if (key == 5) {
                if (read_options(&r, NULL, &opt_uv, &opt_up, &up_present) != CBOR_OK) {
                    return false;
                }
            } else if (cbor_r_skip(&r) != CBOR_OK) {
                return false;
            }
        }

        if (opt_uv || (up_present && !opt_up)) {
            return false;
        }
        return has_allow_list;
    }

    return false;
}

size_t ctap2_core_handle(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_cap,
        bool presence)
{
    /* Every handler carves the CBOR writer out of rsp + 1, so a zero capacity
     * would underflow that length to SIZE_MAX and write past the buffer. No
     * caller does this today; the guard keeps it that way. */
    if (rsp_cap < 1) {
        return 0;
    }

    if (req_len < 1) {
        return emit_status(rsp, rsp_cap, CTAP1_ERR_INVALID_LENGTH);
    }

    const uint8_t *params = req + 1;
    size_t params_len = req_len - 1;

    switch (req[0]) {
    case CTAP2_CMD_GET_INFO:
        return handle_get_info(rsp, rsp_cap);

    case CTAP2_CMD_MAKE_CREDENTIAL:
        return handle_make_credential(params, params_len, rsp, rsp_cap, presence);

    case CTAP2_CMD_GET_ASSERTION:
        return handle_get_assertion(params, params_len, rsp, rsp_cap, presence);

    case CTAP2_CMD_CLIENT_PIN:
        // No PIN support, so there is never a PIN to agree on
        return emit_status(rsp, rsp_cap, CTAP2_ERR_PIN_NOT_SET);

    case CTAP2_CMD_GET_NEXT_ASSERT:
        // Only ever one credential is returned, so there is never a next one
        return emit_status(rsp, rsp_cap, CTAP2_ERR_NO_CREDENTIALS);

    default:
        ESP_LOGW(TAG, "Unsupported CTAP2 command 0x%02x", req[0]);
        return emit_status(rsp, rsp_cap, CTAP1_ERR_INVALID_COMMAND);
    }
}
