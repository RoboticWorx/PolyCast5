// Meshtastic interop layer for PolyCast5 (phase 1: terminal only).
//
// Speaks the Meshtastic LongFast public-channel wire protocol directly on the
// SX1262: continuous RX with on-terminal decode, plus broadcast of text and
// NodeInfo. Hand-rolled minimal protobuf (no nanopb dependency). All wire
// constants are documented and sourced in lora_meshtastic.h.
//
// On-air packet:  [ 16-byte PacketHeader ][ AES-128-CTR( Data protobuf ) ]
//   header   = to(4 LE) from(4 LE) id(4 LE) flags(1) channel(1) next_hop(1) relay(1)
//   nonce    = packetId(8 LE) | fromNode(4 LE) | 0,0,0,0   (CTR counter starts 0)
//   key      = default LongFast PSK (AES-128)

#include "polycast5_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"     // esp_read_mac, ESP_MAC_WIFI_STA
#include "esp_random.h"  // esp_random

#include "psa/crypto.h"  // AES-128-CTR (mbedtls 4.x dropped the legacy mbedtls/aes.h)

#include "sx126x.h"

#include "lora_meshtastic.h"

static const char *TAG = "MESH";

// Commenting out POLYCAST5_MESHTASTIC_MODE in polycast5_macros.h means "off".
#ifndef POLYCAST5_MESHTASTIC_MODE
#define POLYCAST5_MESHTASTIC_MODE 0
#endif

// Runtime mode flag (see header). Default comes from polycast5_macros.h.
volatile bool g_meshtastic_mode = POLYCAST5_MESHTASTIC_MODE;

// ── Identity ───────────────────────────────────────────────────────────────
static uint32_t s_node_num = 0;
static char     s_node_id[10] = "!00000000"; // "!%08x"

// How we advertise ourselves (short_name kept <= 4 chars, Meshtastic convention)
#define MESH_LONG_NAME  "PolyCast5"
#define MESH_SHORT_NAME "PC5"

// Optional periodic self-test text TX (0 = disabled). Lets you confirm TX shows
// up on a nearby phone/node without any UI yet.
#ifndef MESHTASTIC_TEST_TX_PERIOD_MS
#define MESHTASTIC_TEST_TX_PERIOD_MS 60000 // 0 = off. >0 sends a test text every N ms (set back to 0 when done — it's the public channel)
#endif
// Re-broadcast our NodeInfo on this interval (others expire stale entries).
#define MESHTASTIC_NODEINFO_PERIOD_MS (15 * 60 * 1000)

// Default LongFast PSK — meshtastic/firmware Channels.h `defaultpsk`, the
// 16-byte AES-128 key expanded from the 1-byte PSK 0x01.
// base64 "1PG7OiApB1nwvP+rz05pAQ==".
static const uint8_t MESH_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

// PSA handle for the default PSK (imported once in lora_meshtastic_init).
static psa_key_id_t s_mesh_key_id = 0;

static void mesh_crypto_init(void); // imports the default PSK; defined below

// Serializes multi-command radio state transitions (the TX build sequence and
// the IRQ task's RX re-arm) so the higher-priority event task can't reconfigure
// the chip mid-TX-build. The per-call SX126x SPI mutex is finer-grained and does
// not cover whole sequences. Created in lora_meshtastic_init().
static SemaphoreHandle_t s_radio_mtx = NULL;

// Set while a TX is on air so we don't start a second one (half-duplex radio).
// Single-producer invariant: ONLY the lora_meshtastic_run task issues TX
// (send_nodeinfo / send_text), so the check-then-set in mesh_send_packet is
// race-free against other producers; the IRQ task only ever clears it. If a
// second TX caller is added (e.g. a phase-2 LCD send), make the claim atomic.
static volatile bool s_tx_busy = false;

// Largest application payload we will build for a single TX.
#define MESH_MAX_TX_PAYLOAD 200

// ── Radio configuration ────────────────────────────────────────────────────

void lora_meshtastic_get_radio_params(sx126x_mod_params_lora_t *mod,
                                      sx126x_pkt_params_lora_t *pkt,
                                      uint32_t *freq_hz, uint8_t *sync_word)
{
    // LONG_FAST (sub-GHz): BW 250 kHz, SF 11, CR 4/5. Symbol time
    // (1<<11)/250 = 8.192 ms < 16 ms → LDRO off, matching RadioLib auto-LDRO.
    mod->sf   = SX126X_LORA_SF11;
    mod->bw   = SX126X_LORA_BW_250;
    mod->cr   = SX126X_LORA_CR_4_5;
    mod->ldro = 0;

    pkt->preamble_len_in_symb = MESHTASTIC_PREAMBLE_SYMB;  // 16
    pkt->header_type          = SX126X_LORA_PKT_EXPLICIT;
    pkt->pld_len_in_bytes     = MESHTASTIC_MAX_PACKET_LEN; // RX max; real len set per-TX
    pkt->crc_is_on            = true;
    pkt->invert_iq_is_on      = false;

    *freq_hz   = MESHTASTIC_LORA_FREQ_HZ; // 906,875,000
    *sync_word = MESHTASTIC_SYNC_WORD;    // 0x2B → reg 0x24B4
}

// Put the radio into continuous RX (keeps listening after each packet).
// Caller MUST hold s_radio_mtx (it mutates radio mode vs the TX sequence).
static void mesh_enter_rx(void)
{
    sx126x_status_t status = sx126x_set_rx_with_timeout_in_rtc_step(NULL, SX126X_RX_CONTINUOUS);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to enter continuous RX");
    }
}

// ── Identity ───────────────────────────────────────────────────────────────

void lora_meshtastic_init(void)
{
    if (s_radio_mtx == NULL) {
        s_radio_mtx = xSemaphoreCreateMutex();
    }
    mesh_crypto_init(); // import the default PSK once

    if (s_node_num != 0) {
        return; // already initialized
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // Meshtastic derives nodeNum from the low 4 bytes of the MAC (big-endian).
    s_node_num = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                 ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
    if (s_node_num == 0) {
        s_node_num = 0x0a0b0c0d; // never allow a 0 node number
    }
    snprintf(s_node_id, sizeof(s_node_id), "!%08x", (unsigned)s_node_num);
}

uint32_t lora_meshtastic_node_num(void)
{
    return s_node_num;
}

// ── Crypto (AES-128-CTR) ───────────────────────────────────────────────────

static void mesh_build_nonce(uint8_t nonce[16], uint32_t from_node, uint32_t packet_id)
{
    memset(nonce, 0, 16);
    // bytes[0..7] = packetId as little-endian uint64 (32-bit id → bytes 4..7 = 0)
    nonce[0] = (uint8_t)packet_id;
    nonce[1] = (uint8_t)(packet_id >> 8);
    nonce[2] = (uint8_t)(packet_id >> 16);
    nonce[3] = (uint8_t)(packet_id >> 24);
    // bytes[8..11] = fromNode little-endian
    nonce[8]  = (uint8_t)from_node;
    nonce[9]  = (uint8_t)(from_node >> 8);
    nonce[10] = (uint8_t)(from_node >> 16);
    nonce[11] = (uint8_t)(from_node >> 24);
    // bytes[12..15] = CTR block counter, starts at 0
}

// Import the default PSK into PSA once. Idempotent.
static void mesh_crypto_init(void)
{
    if (s_mesh_key_id != 0) {
        return;
    }
    psa_status_t status = psa_crypto_init(); // idempotent
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_CTR);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 8 * sizeof(MESH_DEFAULT_PSK)); // 128

    status = psa_import_key(&attr, MESH_DEFAULT_PSK, sizeof(MESH_DEFAULT_PSK), &s_mesh_key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "mesh key import failed: %d", (int)status);
        s_mesh_key_id = 0;
    }
}

// AES-128-CTR over `buf` in place. Encrypt == decrypt (CTR XORs a keystream), so
// the same call serves both directions. PSA increments the full 16-byte counter
// block big-endian; for any payload under ~4 KB the carry never leaves byte 15,
// so this is byte-identical to Meshtastic's counterSize=4.
static void mesh_aes_ctr(uint8_t *buf, size_t len, uint32_t from_node, uint32_t packet_id)
{
    if (len == 0) {
        return;
    }
    if (s_mesh_key_id == 0) {
        mesh_crypto_init();
        if (s_mesh_key_id == 0) {
            return;
        }
    }

    uint8_t nonce[16];
    mesh_build_nonce(nonce, from_node, packet_id);

    // Use the encrypt path for both directions (CTR is symmetric) and supply our
    // own 16-byte counter block as the IV. PSA forbids in-place, so XOR via a temp.
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status = psa_cipher_encrypt_setup(&op, s_mesh_key_id, PSA_ALG_CTR);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "cipher setup failed: %d", (int)status);
        return;
    }
    status = psa_cipher_set_iv(&op, nonce, sizeof(nonce));
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "cipher set_iv failed: %d", (int)status);
        psa_cipher_abort(&op);
        return;
    }

    uint8_t out[MESHTASTIC_MAX_PACKET_LEN];
    size_t out_len = 0, fin_len = 0;
    status = psa_cipher_update(&op, buf, len, out, sizeof(out), &out_len);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&op, out + out_len, sizeof(out) - out_len, &fin_len);
    }
    psa_cipher_abort(&op);

    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "cipher update/finish failed: %d", (int)status);
        return;
    }
    memcpy(buf, out, out_len + fin_len); // == len for a CTR stream cipher
}

// ── Minimal protobuf ───────────────────────────────────────────────────────

// Writes a varint at out[off]. Writes at most 10 bytes (64-bit); callers must
// guarantee the buffer has room (all encoders here pre-size with that margin).
static size_t pb_put_varint(uint8_t *out, size_t off, uint64_t v)
{
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) {
            b |= 0x80;
        }
        out[off++] = b;
    } while (v);
    return off;
}

// Reads a varint, advancing *p (bounded by end). Returns false on truncation.
static bool pb_get_varint(const uint8_t **p, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (*p < end && shift < 64) {
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
        shift += 7;
    }
    return false;
}

static uint32_t mesh_get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mesh_put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Build a Data message: field 1 portnum (varint), field 2 payload (bytes).
static size_t mesh_encode_data(uint8_t *out, uint8_t portnum,
                               const uint8_t *payload, size_t payload_len)
{
    size_t off = 0;
    out[off++] = 0x08;                          // tag: field 1, varint
    off = pb_put_varint(out, off, portnum);
    out[off++] = 0x12;                          // tag: field 2, length-delimited
    off = pb_put_varint(out, off, payload_len);
    memcpy(out + off, payload, payload_len);
    off += payload_len;
    return off;
}

// Build a User message: id (f1), long_name (f2), short_name (f3), all strings.
// Bounded by out_cap and truncates per-field, so it stays safe if the names ever
// become runtime/user-configurable (phase-2 LCD).
static size_t mesh_encode_user(uint8_t *out, size_t out_cap, const char *id,
                               const char *long_name, const char *short_name)
{
    const char *fields[3] = { id, long_name, short_name };
    const uint8_t tags[3] = { 0x0A, 0x12, 0x1A }; // f1/f2/f3, length-delimited
    size_t off = 0;
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(fields[i]);
        if (n > 127) {
            n = 127; // keep the length varint single-byte
        }
        if (off + 2 + n > out_cap) {
            break; // out of room: emit what fits (1 tag + 1 len + n)
        }
        out[off++] = tags[i];
        off = pb_put_varint(out, off, n);
        memcpy(out + off, fields[i], n);
        off += n;
    }
    return off;
}

// ── PacketHeader + TX ──────────────────────────────────────────────────────

static void mesh_build_header(uint8_t *p, uint32_t to, uint32_t from, uint32_t id,
                              uint8_t flags, uint8_t channel)
{
    mesh_put_u32le(p + 0, to);
    mesh_put_u32le(p + 4, from);
    mesh_put_u32le(p + 8, id);
    p[12] = flags;
    p[13] = channel;
    p[14] = 0x00; // next_hop: no preference
    p[15] = 0x00; // relay_node: none
}

static uint32_t mesh_random_id(void)
{
    uint32_t id;
    do {
        id = esp_random();
    } while (id == 0);
    return id;
}

// Set Meshtastic packet params with the real length, then write + transmit.
// The whole set_pkt_params → write_buffer → set_tx sequence is held under
// s_radio_mtx so the IRQ task can't re-arm RX between the steps.
static bool mesh_radio_tx(const uint8_t *data, uint8_t len)
{
    sx126x_pkt_params_lora_t pkt = {
        .preamble_len_in_symb = MESHTASTIC_PREAMBLE_SYMB,
        .header_type          = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes     = len,
        .crc_is_on            = true,
        .invert_iq_is_on      = false,
    };

    bool ok = true;
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    if (sx126x_set_lora_pkt_params(NULL, &pkt) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "TX: set pkt params failed");
        ok = false;
    }
    if (ok && sx126x_write_buffer(NULL, 0, data, len) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "TX: write buffer failed");
        ok = false;
    }
    if (ok) {
        s_tx_busy = true; // set before set_tx so a preempting IRQ sees it
        if (sx126x_set_tx(NULL, SX126X_MAX_TIMEOUT_IN_MS) != SX126X_STATUS_OK) {
            s_tx_busy = false;
            ESP_LOGE(TAG, "TX: set_tx failed");
            ok = false;
        }
    }
    xSemaphoreGive(s_radio_mtx);
    return ok;
}

// Encrypt an application payload into a full Meshtastic broadcast frame and send.
static bool mesh_send_packet(uint8_t portnum, const uint8_t *payload, size_t payload_len)
{
    if (s_node_num == 0) {
        lora_meshtastic_init();
    }
    if (s_tx_busy) {
        ESP_LOGW(TAG, "TX still busy, dropping packet (port %u)", portnum);
        return false;
    }
    if (payload_len > MESH_MAX_TX_PAYLOAD) {
        ESP_LOGW(TAG, "payload too long (%u), truncating to %u",
                 (unsigned)payload_len, MESH_MAX_TX_PAYLOAD);
        payload_len = MESH_MAX_TX_PAYLOAD;
    }

    // 8-byte margin covers the Data framing (tag+varint portnum, tag+varint len)
    // for portnum < 16384 and payload_len <= MESH_MAX_TX_PAYLOAD (<= 6 bytes).
    uint8_t data[8 + MESH_MAX_TX_PAYLOAD];
    size_t data_len = mesh_encode_data(data, portnum, payload, payload_len);

    uint32_t packet_id = mesh_random_id();
    mesh_aes_ctr(data, data_len, s_node_num, packet_id); // encrypt in place

    uint8_t pkt[16 + sizeof(data)];
    // flags 0x63 = hop_limit 3 | hop_start 3 (want_ack 0, via_mqtt 0)
    mesh_build_header(pkt, 0xFFFFFFFFu, s_node_num, packet_id, 0x63, MESHTASTIC_CHANNEL_HASH);
    memcpy(pkt + 16, data, data_len);

    return mesh_radio_tx(pkt, (uint8_t)(16 + data_len));
}

bool lora_meshtastic_send_text(const char *text)
{
    if (s_node_num == 0) {
        lora_meshtastic_init();
    }
    ESP_LOGI(TAG, "TX text → mesh: \"%s\"", text);
    return mesh_send_packet(MESHTASTIC_PORT_TEXT, (const uint8_t *)text, strlen(text));
}

bool lora_meshtastic_send_nodeinfo(void)
{
    if (s_node_num == 0) {
        lora_meshtastic_init();
    }
    // We emit only id/long_name/short_name. is_licensed is omitted (proto3 default
    // false), so normal receivers list us; by Meshtastic design a ham-licensed
    // receiver (owner.is_licensed=true) drops NodeInfo on an is_licensed mismatch,
    // so we are simply invisible to those nodes — claiming licensed would be wrong.
    uint8_t user[96];
    size_t user_len = mesh_encode_user(user, sizeof(user), s_node_id, MESH_LONG_NAME, MESH_SHORT_NAME);
    ESP_LOGI(TAG, "TX nodeinfo → mesh: %s (%s / %s)", s_node_id, MESH_LONG_NAME, MESH_SHORT_NAME);
    return mesh_send_packet(MESHTASTIC_PORT_NODEINFO, user, user_len);
}

// ── RX decode + print ──────────────────────────────────────────────────────

// Parse a decrypted Data message: extract portnum (f1) and payload (f2).
static bool mesh_decode_data(const uint8_t *buf, size_t len, uint32_t *portnum,
                             const uint8_t **payload, size_t *payload_len)
{
    *portnum = 0;
    *payload = NULL;
    *payload_len = 0;

    const uint8_t *p = buf, *end = buf + len;
    while (p < end) {
        uint64_t tag;
        if (!pb_get_varint(&p, end, &tag)) {
            return false;
        }
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wire  = (uint32_t)(tag & 0x07);

        if (wire == 0) {            // varint
            uint64_t v;
            if (!pb_get_varint(&p, end, &v)) {
                return false;
            }
            if (field == 1) {
                *portnum = (uint32_t)v;
            }
        } else if (wire == 2) {     // length-delimited
            uint64_t l;
            if (!pb_get_varint(&p, end, &l) || (uint64_t)(end - p) < l) {
                return false;
            }
            if (field == 2) {
                *payload = p;
                *payload_len = (size_t)l;
            }
            p += l;
        } else if (wire == 5) {     // 32-bit
            if ((end - p) < 4) {
                return false;
            }
            p += 4;
        } else if (wire == 1) {     // 64-bit
            if ((end - p) < 8) {
                return false;
            }
            p += 8;
        } else {                    // unknown wire type
            return false;
        }
    }
    return true;
}

// Decode a User payload and print id / long_name / short_name.
static void mesh_print_user(const uint8_t *buf, size_t len)
{
    char id[24] = {0}, ln[40] = {0}, sn[8] = {0};
    const uint8_t *p = buf, *end = buf + len;
    while (p < end) {
        uint64_t tag;
        if (!pb_get_varint(&p, end, &tag)) {
            break;
        }
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wire  = (uint32_t)(tag & 0x07);

        if (wire == 2) {
            uint64_t l;
            if (!pb_get_varint(&p, end, &l) || (uint64_t)(end - p) < l) {
                break;
            }
            if (field == 1 && l < sizeof(id)) {
                memcpy(id, p, l);
                id[l] = 0;
            } else if (field == 2 && l < sizeof(ln)) {
                memcpy(ln, p, l);
                ln[l] = 0;
            } else if (field == 3 && l < sizeof(sn)) {
                memcpy(sn, p, l);
                sn[l] = 0;
            }
            p += l;
        } else if (wire == 0) {
            uint64_t v;
            if (!pb_get_varint(&p, end, &v)) {
                break;
            }
        } else if (wire == 5) {
            if ((end - p) < 4) {
                break;
            }
            p += 4;
        } else if (wire == 1) {
            if ((end - p) < 8) {
                break;
            }
            p += 8;
        } else {
            break;
        }
    }
    ESP_LOGI(TAG, "   NodeInfo: id=%s long=\"%s\" short=\"%s\"", id, ln, sn);
}

// Decode a Position payload and print lat / lon / alt (no floating point).
static void mesh_print_position(const uint8_t *buf, size_t len)
{
    int32_t lat_i = 0, lon_i = 0, alt = 0;
    uint32_t t = 0;
    bool have_lat = false, have_lon = false;

    const uint8_t *p = buf, *end = buf + len;
    while (p < end) {
        uint64_t tag;
        if (!pb_get_varint(&p, end, &tag)) {
            break;
        }
        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wire  = (uint32_t)(tag & 0x07);

        if (wire == 5) {            // sfixed32 / fixed32
            if ((end - p) < 4) {
                break;
            }
            int32_t v = (int32_t)mesh_get_u32le(p);
            if (field == 1) { lat_i = v; have_lat = true; }
            else if (field == 2) { lon_i = v; have_lon = true; }
            else if (field == 4) { t = (uint32_t)v; }
            p += 4;
        } else if (wire == 0) {     // varint
            uint64_t v;
            if (!pb_get_varint(&p, end, &v)) {
                break;
            }
            if (field == 3) {
                alt = (int32_t)v;
            }
        } else if (wire == 2) {
            uint64_t l;
            if (!pb_get_varint(&p, end, &l) || (uint64_t)(end - p) < l) {
                break;
            }
            p += l;
        } else if (wire == 1) {
            if ((end - p) < 8) {
                break;
            }
            p += 8;
        } else {
            break;
        }
    }

    if (have_lat || have_lon) {
        // Print scaled integer degrees (×1e7) as D.DDDDDDD without float printf.
        // Take magnitudes in uint32 (0u - x is well-defined and handles INT32_MIN,
        // which abs() would hit as undefined behavior on hostile input).
        uint32_t lat_mag = (lat_i < 0) ? (0u - (uint32_t)lat_i) : (uint32_t)lat_i;
        uint32_t lon_mag = (lon_i < 0) ? (0u - (uint32_t)lon_i) : (uint32_t)lon_i;
        ESP_LOGI(TAG, "   Position: lat=%s%u.%07u lon=%s%u.%07u alt=%dm time=%u",
                 (lat_i < 0) ? "-" : "", lat_mag / 10000000u, lat_mag % 10000000u,
                 (lon_i < 0) ? "-" : "", lon_mag / 10000000u, lon_mag % 10000000u,
                 (int)alt, (unsigned)t);
    } else {
        ESP_LOGI(TAG, "   Position: (no fix) alt=%dm time=%u", (int)alt, (unsigned)t);
    }
}

void lora_meshtastic_process_rx(const uint8_t *buf, size_t len, int8_t rssi, int8_t snr)
{
    static uint8_t payload[MESHTASTIC_MAX_PACKET_LEN];

    if (len < 16) {
        ESP_LOGW(TAG, "RX frame too short (%u bytes)", (unsigned)len);
        return;
    }

    uint32_t to    = mesh_get_u32le(buf + 0);
    uint32_t from  = mesh_get_u32le(buf + 4);
    uint32_t id    = mesh_get_u32le(buf + 8);
    uint8_t  flags = buf[12];
    uint8_t  chan  = buf[13];
    uint8_t  hop_limit = flags & 0x07;
    uint8_t  hop_start = (flags & 0xE0) >> 5;

    ESP_LOGI(TAG, "RX !%08x → %s id=0x%08x ch=0x%02x hops=%u/%u rssi=%d snr=%d len=%u%s",
             (unsigned)from, (to == 0xFFFFFFFFu) ? "ALL" : "node",
             (unsigned)id, chan, hop_limit, hop_start, rssi, snr, (unsigned)len,
             (from == s_node_num) ? " (self)" : "");

    // We only hold the default LongFast key, so only ch 0x08 packets decrypt.
    if (chan != MESHTASTIC_CHANNEL_HASH) {
        ESP_LOGI(TAG, "   (channel 0x%02x not ours [0x%02x] — skipping decrypt)",
                 chan, MESHTASTIC_CHANNEL_HASH);
        return;
    }

    size_t pl_len = len - 16;
    memcpy(payload, buf + 16, pl_len);
    mesh_aes_ctr(payload, pl_len, from, id); // decrypt in place

    uint32_t portnum;
    const uint8_t *inner;
    size_t inner_len;
    if (!mesh_decode_data(payload, pl_len, &portnum, &inner, &inner_len)) {
        ESP_LOGW(TAG, "   (could not parse Data — wrong key or channel-hash collision)");
        return;
    }

    switch (portnum) {
    case MESHTASTIC_PORT_TEXT:
        ESP_LOGI(TAG, "   TEXT: \"%.*s\"", (int)inner_len, (const char *)inner);
        break;
    case MESHTASTIC_PORT_NODEINFO:
        mesh_print_user(inner, inner_len);
        break;
    case MESHTASTIC_PORT_POSITION:
        mesh_print_position(inner, inner_len);
        break;
    default:
        ESP_LOGI(TAG, "   port=%u payload=%u bytes", (unsigned)portnum, (unsigned)inner_len);
        break;
    }
}

// ── IRQ handling + main loop ───────────────────────────────────────────────

void lora_meshtastic_handle_irq(uint16_t irq_flags)
{
    // Only the event-handler task touches rx_buf, so it needs no lock of its own.
    static uint8_t rx_buf[MESHTASTIC_MAX_PACKET_LEN];
    // rx_size (from the radio) is uint8_t, so any value fits as long as rx_buf is
    // >= 255 — enforced here so the read below needs no (always-true) bound check.
    _Static_assert(MESHTASTIC_MAX_PACKET_LEN >= 255, "rx_buf must hold any uint8_t LoRa length");

    if (s_radio_mtx == NULL) {
        return; // a stray IRQ before init: radio isn't ours yet
    }

    uint8_t rx_size = 0;
    int8_t  rssi = 0, snr = 0;
    bool    have_pkt = false;

    // The DIO1 ISR is edge-triggered, so we must consume every latched event in
    // one pass (TX_DONE and RX_DONE can co-latch) and clear IRQ_ALL — otherwise
    // a leftover flag keeps DIO1 high and no future edge is generated.
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);

    if (irq_flags & SX126X_IRQ_RX_DONE) {
        sx126x_rx_buffer_status_t rx_status = {0};
        if (sx126x_get_rx_buffer_status(NULL, &rx_status) == SX126X_STATUS_OK) {
            rx_size = rx_status.pld_len_in_bytes;
            // RSSI/SNR are valid even for a CRC-failed packet (it was still
            // demodulated), so capture them up front to diagnose dropped frames.
            sx126x_pkt_status_lora_t st = {0};
            sx126x_get_lora_pkt_status(NULL, &st);
            rssi = st.rssi_pkt_in_dbm;
            snr  = st.snr_pkt_in_db;
            // CRC errors co-latch with RX_DONE; reject those and empty frames.
            if (!(irq_flags & SX126X_IRQ_CRC_ERROR) && rx_size > 0 &&
                sx126x_read_buffer(NULL, rx_status.buffer_start_pointer, rx_buf, rx_size) == SX126X_STATUS_OK) {
                have_pkt = true;
            }
        }
    }

    // A TX ends on completion OR its watchdog timeout. Continuous RX never raises
    // TIMEOUT, so this only fires for the TX path — and clearing s_tx_busy here is
    // what lets RX re-arm again after a wedged/aborted TX (otherwise the node would
    // go permanently deaf and mute, since !s_tx_busy gates every RX re-arm).
    if (irq_flags & (SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT)) {
        s_tx_busy = false;
    }

    sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL);

    // Re-arm continuous RX, but never while a TX we started is still on air.
    if (!s_tx_busy) {
        mesh_enter_rx();
    }
    xSemaphoreGive(s_radio_mtx);

    // Decrypt + log outside the radio lock (no radio access, may be slow).
    if (irq_flags & SX126X_IRQ_RX_DONE) {
        if (have_pkt) {
            lora_meshtastic_process_rx(rx_buf, rx_size, rssi, snr);
        } else {
            ESP_LOGW(TAG, "RX dropped (size=%u crc_err=%d rssi=%d snr=%d)", rx_size,
                     (irq_flags & SX126X_IRQ_CRC_ERROR) ? 1 : 0, rssi, snr);
        }
    }
}

void lora_meshtastic_run(void)
{
    lora_meshtastic_init();

    ESP_LOGI(TAG, "=== Meshtastic mode ACTIVE ===");
    ESP_LOGI(TAG, "Node %s (0x%08x)  LongFast/US  %u.%03u MHz  ch-hash 0x%02x",
             s_node_id, (unsigned)s_node_num,
             (unsigned)(MESHTASTIC_LORA_FREQ_HZ / 1000000UL),
             (unsigned)((MESHTASTIC_LORA_FREQ_HZ / 1000UL) % 1000UL),
             MESHTASTIC_CHANNEL_HASH);

    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    mesh_enter_rx();
    xSemaphoreGive(s_radio_mtx);

    // Announce ourselves so we appear in other nodes' lists.
    vTaskDelay(pdMS_TO_TICKS(500));
    lora_meshtastic_send_nodeinfo();

    TickType_t last_nodeinfo = xTaskGetTickCount();
#if MESHTASTIC_TEST_TX_PERIOD_MS > 0
    TickType_t last_test = xTaskGetTickCount();
    uint32_t   test_seq = 0;
#endif

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if ((now - last_nodeinfo) >= pdMS_TO_TICKS(MESHTASTIC_NODEINFO_PERIOD_MS)) {
            lora_meshtastic_send_nodeinfo();
            last_nodeinfo = now;
        }

#if MESHTASTIC_TEST_TX_PERIOD_MS > 0
        if ((now - last_test) >= pdMS_TO_TICKS(MESHTASTIC_TEST_TX_PERIOD_MS)) {
            char msg[48];
            snprintf(msg, sizeof(msg), "PolyCast5 test #%u", (unsigned)(++test_seq));
            lora_meshtastic_send_text(msg);
            last_test = now;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
