// Meshtastic interop layer for PolyCast5.
//
// Speaks the Meshtastic LongFast public-channel wire protocol directly on the
// SX1262: continuous RX with decode, plus broadcast of text and NodeInfo. RX
// text and our own sent text are kept in a small ring buffer that the Wi-Fi
// config portal polls, and portal text is queued back here for transmit - so
// the portal is a text I/O surface while all radio framing happens here.
// Hand-rolled minimal protobuf (no nanopb dependency). All wire constants are
// documented and sourced in lora_meshtastic.h.
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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"     // esp_read_mac, ESP_MAC_WIFI_STA
#include "esp_random.h"  // esp_random

#include "psa/crypto.h"  // AES-128-CTR (mbedtls 4.x dropped the legacy mbedtls/aes.h)

#include "sx126x.h"

#include "lora_meshtastic.h"

static const char *TAG = "MESH";

// lora_task overwrites this at startup from lora_meshtastic_portal_enabled_load_nvs()
volatile bool g_meshtastic_mode = false;

static uint32_t s_node_num = 0;
static char     s_node_id[10] = "!00000000"; // "!%08x"

// How we advertise ourselves (short_name kept <= 4 chars, Meshtastic convention)
#define MESH_LONG_NAME  "PolyCast5"
#define MESH_SHORT_NAME "PC5"

// Re-broadcast our NodeInfo on this interval (others expire stale entries)
#define MESHTASTIC_NODEINFO_PERIOD_MS (15 * 60 * 1000) // Every 15 minutes

// Depth of the web-portal -> radio text send queue
#define MESH_TX_QUEUE_LEN 4

// Default LongFast PSK - meshtastic/firmware Channels.h `defaultpsk`, the
// 16-byte AES-128 key expanded from the 1-byte PSK 0x01.
// base64 "1PG7OiApB1nwvP+rz05pAQ==".
static const uint8_t MESH_DEFAULT_PSK[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

// PSA handle for the default PSK (imported once in lora_meshtastic_init).
static psa_key_id_t s_mesh_key_id = 0;

static void mesh_crypto_init(void); // Imports the default PSK; defined below

// Serializes multi-command radio state transitions so the higher-priority event task can't reconfigure mid-TX-build
static SemaphoreHandle_t s_radio_mtx = NULL;

// Set while a TX is on air so we don't start a second one (half-duplex radio)
static volatile bool s_tx_busy = false;

// True only while the Meshtastic portal page is open
// Continuous RX (the radio's main current draw) and all TX are gated on this
static volatile bool s_listening = false;

// Set by listen_start so the run task broadcasts a NodeInfo at session start
static volatile bool s_announce_pending = false;

// Largest application payload we will build for a single TX
#define MESH_MAX_TX_PAYLOAD 200

// Web-portal data plane (message log + TX queue)

// Ring buffer of recent text messages (inbound + our own outbound) exposed to the web portal
// Written by the event task (RX) and the run task (TX echo), read by the HTTP server task
POLYCAST5_USE_PSRAM_BSS static lora_meshtastic_msg_t s_log[MESHTASTIC_MSG_LOG_CAP];
static uint8_t  s_log_head  = 0; // Index of the oldest stored entry
static uint8_t  s_log_count = 0; // Number of valid entries (<= CAP)
static uint32_t s_log_seq   = 0; // Last assigned seq (monotonic, 0 = none yet)
static SemaphoreHandle_t s_log_mtx = NULL;

// Recently stored inbound packet ids, to drop mesh-relayed duplicates of the same message
static uint32_t s_seen_ids[MESHTASTIC_MSG_LOG_CAP] = {0};
static uint8_t  s_seen_pos = 0;

// Text queued by the web portal for the run task to broadcast (single-producer TX)
typedef struct { char text[MESHTASTIC_RX_TEXT_MAX + 1]; } mesh_tx_item_t;
static QueueHandle_t s_tx_queue = NULL;

// Nodes-heard roster
// Written by the RX event task (mesh_node_seen/set_names), read by the HTTP task
typedef struct {
    uint32_t node_num;
    char     long_name[MESHTASTIC_NODE_LONG_MAX + 1];
    char     short_name[MESHTASTIC_NODE_SHORT_MAX + 1];
    int8_t   rssi;
    int8_t   snr;
    uint8_t  hops;
    uint32_t last_heard_tick;
} mesh_node_t;
POLYCAST5_USE_PSRAM_BSS static mesh_node_t s_nodes[MESHTASTIC_NODE_MAX];
static SemaphoreHandle_t s_nodes_mtx = NULL;

// Largest length <= max that doesn't split a UTF-8 multi-byte sequence. Backs up
// off any trailing continuation bytes (0b10xxxxxx) so truncated text stays valid
// UTF-8 - no stray replacement char goes on the mesh or into the portal.
static size_t mesh_utf8_trunc_len(const uint8_t *s, size_t len, size_t max)
{
    if (len <= max) {
        return len;
    }
    size_t i = max;
    while (i > 0 && (s[i] & 0xC0) == 0x80) {
        i--; // s[i] is a continuation byte: the char that started earlier straddles max
    }
    return i;
}

// Append one message to the ring (overwriting the oldest when full)
static void mesh_log_append(uint32_t from_node, bool outbound, int8_t rssi,
                            int8_t snr, uint8_t hops, const uint8_t *text, size_t len,
                            uint32_t id, bool failed)
{
    if (s_log_mtx == NULL) {
        return; // Not initialized yet
    }
    len = mesh_utf8_trunc_len(text, len, MESHTASTIC_RX_TEXT_MAX);

    xSemaphoreTake(s_log_mtx, portMAX_DELAY);

    uint8_t slot;
    if (s_log_count < MESHTASTIC_MSG_LOG_CAP) {
        slot = (uint8_t)((s_log_head + s_log_count) % MESHTASTIC_MSG_LOG_CAP);
        s_log_count++;
    } else {
        slot = s_log_head; // Full: overwrite oldest and advance head
        s_log_head = (uint8_t)((s_log_head + 1) % MESHTASTIC_MSG_LOG_CAP);
    }

    lora_meshtastic_msg_t *m = &s_log[slot];
    m->seq       = ++s_log_seq;
    m->id        = id;
    m->outbound  = outbound;
    m->acked     = false;
    m->failed    = failed;
    m->from_node = from_node;
    m->rssi      = rssi;
    m->snr       = snr;
    m->hops      = hops;
    memcpy(m->text, text, len);
    m->text[len] = '\0';

    xSemaphoreGive(s_log_mtx);
}

// Mark our outbound message with packet id `id` as delivered:
// We heard a neighbour relay our own broadcast back, which is Meshtastic's implicit ACK for a broadcast
static void mesh_log_mark_acked(uint32_t id)
{
    if (s_log_mtx == NULL || id == 0) {
        return;
    }

    // Walk the log
    xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    for (uint8_t i = 0; i < s_log_count; i++) {
        lora_meshtastic_msg_t *m = &s_log[(s_log_head + i) % MESHTASTIC_MSG_LOG_CAP];
        if (m->outbound && m->id == id && !m->acked) {
            m->acked = true;
            m->seq   = ++s_log_seq; // Bump so the portal re-fetches the new status
            break;
        }
    }
    xSemaphoreGive(s_log_mtx);
}

size_t lora_meshtastic_get_msgs_since(uint32_t since, lora_meshtastic_msg_t *out,
                                      size_t max_out, uint32_t *newest_seq)
{
    if (newest_seq != NULL) {
        *newest_seq = 0;
    }
    if (s_log_mtx == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    size_t n = 0;
    xSemaphoreTake(s_log_mtx, portMAX_DELAY);

    if (newest_seq != NULL) {
        *newest_seq = s_log_seq;
    }

    // Reboot detection: s_log_seq restarts at 0 on every boot, so a client cursor
    // above the current max means the log was reset (device rebooted)
    // Treat it as a fresh client and replay the whole window, otherwise a still-open browser tab would drop every new message
    if (since > s_log_seq) {
        since = 0;
    }
    // Walk oldest -> newest; copy the newer-than-'since' tail, capped to max_out.
    for (uint8_t i = 0; i < s_log_count; i++) {
        const lora_meshtastic_msg_t *m = &s_log[(s_log_head + i) % MESHTASTIC_MSG_LOG_CAP];
        if (m->seq <= since) {
            continue;
        }
        if (n == max_out) {
            // Buffer full: shift window forward so we keep the newest max_out.
            memmove(out, out + 1, (max_out - 1) * sizeof(*out));
            n--;
        }
        out[n++] = *m;
    }

    xSemaphoreGive(s_log_mtx);
    return n;
}

bool lora_meshtastic_enqueue_text(const char *text)
{
    // Reject when no session is active (!s_listening)
    if (!g_meshtastic_mode || !s_listening || s_tx_queue == NULL || text == NULL || text[0] == '\0') {
        return false;
    }

    mesh_tx_item_t item;
    size_t n = mesh_utf8_trunc_len((const uint8_t *)text, strlen(text), MESHTASTIC_RX_TEXT_MAX);
    memcpy(item.text, text, n);
    item.text[n] = '\0';
    if (item.text[0] == '\0') {
        return false; // Empty after truncation
    }
    return xQueueSend(s_tx_queue, &item, 0) == pdTRUE;
}

// Nodes-heard roster

// Find the slot holding node_num, or -1. Caller must hold s_nodes_mtx
static int mesh_node_find(uint32_t node_num)
{
    for (int i = 0; i < MESHTASTIC_NODE_MAX; i++) {
        if (s_nodes[i].node_num == node_num) {
            return i;
        }
    }
    return -1;
}

// Find (or allocate, evicting the oldest when full) the slot for node_num
// Caller must hold s_nodes_mtx
static int mesh_node_slot(uint32_t node_num)
{
    int idx = mesh_node_find(node_num);
    if (idx >= 0) {
        return idx;
    }

    TickType_t now = xTaskGetTickCount();
    int oldest = 0;
    for (int i = 0; i < MESHTASTIC_NODE_MAX; i++) {
        if (s_nodes[i].node_num == 0) {
            oldest = i; // An empty slot is the best possible choice
            break;
        }

        // Largest elapsed time = oldest
        // Compare elapsed (not raw ticks) so this stays correct across a tick-counter wrap, like get_nodes' age_s.
        if ((TickType_t)(now - s_nodes[i].last_heard_tick) >
            (TickType_t)(now - s_nodes[oldest].last_heard_tick)) {
            oldest = i;
        }
    }

    // Evict the oldest and return that slot for reuse
    memset(&s_nodes[oldest], 0, sizeof(s_nodes[oldest]));
    s_nodes[oldest].node_num = node_num;
    s_nodes[oldest].rssi = INT8_MIN;
    s_nodes[oldest].snr  = INT8_MIN;
    return oldest;
}

// Record a heard packet from `from`: keep the best signal, latest hops/last-heard
static void mesh_node_seen(uint32_t from, int8_t rssi, int8_t snr, uint8_t hops)
{
    if (s_nodes_mtx == NULL || from == 0 || from == s_node_num) {
        return; // Don't roster ourselves or a null id
    }

    xSemaphoreTake(s_nodes_mtx, portMAX_DELAY);
    mesh_node_t *n = &s_nodes[mesh_node_slot(from)];
    if (rssi > n->rssi) {
        n->rssi = rssi;
    }
    if (snr > n->snr) {
        n->snr = snr;
    }
    n->hops = hops;
    n->last_heard_tick = xTaskGetTickCount();
    xSemaphoreGive(s_nodes_mtx);
}

// Attach names from a NodeInfo to an already-seen node (seen runs first per packet)
static void mesh_node_set_names(uint32_t from, const char *long_name, const char *short_name)
{
    if (s_nodes_mtx == NULL || from == 0 || from == s_node_num) {
        return;
    }
    
    xSemaphoreTake(s_nodes_mtx, portMAX_DELAY);
    int i = mesh_node_find(from);
    if (i >= 0) {
        strncpy(s_nodes[i].long_name, long_name, MESHTASTIC_NODE_LONG_MAX);
        s_nodes[i].long_name[MESHTASTIC_NODE_LONG_MAX] = '\0';
        strncpy(s_nodes[i].short_name, short_name, MESHTASTIC_NODE_SHORT_MAX);
        s_nodes[i].short_name[MESHTASTIC_NODE_SHORT_MAX] = '\0';
    }
    xSemaphoreGive(s_nodes_mtx);
}

size_t lora_meshtastic_get_nodes(lora_meshtastic_node_t *out, size_t max_out)
{
    if (s_nodes_mtx == NULL || out == NULL || max_out == 0) {
        return 0;
    }

    size_t n = 0;

    xSemaphoreTake(s_nodes_mtx, portMAX_DELAY);
    TickType_t now = xTaskGetTickCount();

    for (int i = 0; i < MESHTASTIC_NODE_MAX && n < max_out; i++) {
        if (s_nodes[i].node_num == 0) {
            continue;
        }

        out[n].node_num = s_nodes[i].node_num;
        strncpy(out[n].long_name, s_nodes[i].long_name, MESHTASTIC_NODE_LONG_MAX);
        out[n].long_name[MESHTASTIC_NODE_LONG_MAX] = '\0';
        strncpy(out[n].short_name, s_nodes[i].short_name, MESHTASTIC_NODE_SHORT_MAX);
        out[n].short_name[MESHTASTIC_NODE_SHORT_MAX] = '\0';
        out[n].rssi = s_nodes[i].rssi;
        out[n].snr  = s_nodes[i].snr;
        out[n].hops = s_nodes[i].hops;

        // Tick subtraction is wrap-safe; divide by the tick rate for whole seconds
        out[n].age_s = (uint32_t)((now - s_nodes[i].last_heard_tick) / configTICK_RATE_HZ);
        n++;
    }
    xSemaphoreGive(s_nodes_mtx);

    return n;
}

size_t lora_meshtastic_node_count(void)
{
    if (s_nodes_mtx == NULL) {
        return 0;
    }

    size_t c = 0; // Start at 0

    xSemaphoreTake(s_nodes_mtx, portMAX_DELAY);
    for (int i = 0; i < MESHTASTIC_NODE_MAX; i++) {
        // Count one for every non-zero node_num
        if (s_nodes[i].node_num != 0) {
            c++;
        }
    }
    xSemaphoreGive(s_nodes_mtx);

    return c;
}

// Radio configuration

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
    // Restore the full RX payload length first
    sx126x_pkt_params_lora_t pkt = {
        .preamble_len_in_symb = MESHTASTIC_PREAMBLE_SYMB,
        .header_type          = SX126X_LORA_PKT_EXPLICIT,
        .pld_len_in_bytes     = MESHTASTIC_MAX_PACKET_LEN,
        .crc_is_on            = true,
        .invert_iq_is_on      = false,
    };
    if (sx126x_set_lora_pkt_params(NULL, &pkt) != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "RX: set pkt params failed");
    }

    sx126x_status_t status = sx126x_set_rx_with_timeout_in_rtc_step(NULL, SX126X_RX_CONTINUOUS);
    if (status != SX126X_STATUS_OK) {
        ESP_LOGE(TAG, "Failed to enter continuous RX");
    }
}

// Identity

void lora_meshtastic_init(void)
{
    if (s_radio_mtx == NULL) {
        s_radio_mtx = xSemaphoreCreateMutex();
    }
    if (s_log_mtx == NULL) {
        s_log_mtx = xSemaphoreCreateMutex();
    }
    if (s_nodes_mtx == NULL) {
        s_nodes_mtx = xSemaphoreCreateMutex();
    }
    if (s_tx_queue == NULL) {
        s_tx_queue = xQueueCreate(MESH_TX_QUEUE_LEN, sizeof(mesh_tx_item_t));
    }

    // Fail loudly at boot if any of these couldn't be allocated, rather than
    // NULL-dereferencing later in the run loop / IRQ path (matches lora_task)
    configASSERT(s_radio_mtx);
    configASSERT(s_log_mtx);
    configASSERT(s_nodes_mtx);
    configASSERT(s_tx_queue);

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

const char *lora_meshtastic_node_id(void)
{
    return s_node_id;
}

// Crypto (AES-128-CTR)

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

// Build a User message: id (f1), long_name (f2), short_name (f3), all strings
// Bounded by out_cap and truncates per-field, so it stays safe if the names ever become runtime/user-configurable
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

    // Nothing transmits after the session closed
    if (!s_listening) {
        xSemaphoreGive(s_radio_mtx);
        return false;
    }
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

// Encrypt an application payload into a full Meshtastic broadcast frame and send:
// If out_id is non-NULL it receives the Meshtastic packet id used, so the caller can later match a relayed copy as an ACK
static bool mesh_send_packet(uint8_t portnum, const uint8_t *payload, size_t payload_len,
                             uint32_t *out_id)
{
    if (out_id != NULL) {
        *out_id = 0;
    }
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
    if (out_id != NULL) {
        *out_id = packet_id; // Report the id even if the radio TX below fails
    }
    mesh_aes_ctr(data, data_len, s_node_num, packet_id); // Encrypt in place

    uint8_t pkt[16 + sizeof(data)];
    // flags 0x63 = hop_limit 3 | hop_start 3 (want_ack 0, via_mqtt 0)
    mesh_build_header(pkt, 0xFFFFFFFFu, s_node_num, packet_id, 0x63, MESHTASTIC_CHANNEL_HASH);
    memcpy(pkt + 16, data, data_len);

    return mesh_radio_tx(pkt, (uint8_t)(16 + data_len));
}

// Broadcast user text. Returns whether the radio TX actually started; the
// Meshtastic packet id used is written to *out_id (even on failure) so the run
// loop can later recognise a relayed copy as an implicit delivery ACK.
static bool mesh_send_user_text(const char *text, uint32_t *out_id)
{
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "TX text -> mesh: \"%s\"", text);
#endif
    return mesh_send_packet(MESHTASTIC_PORT_TEXT, (const uint8_t *)text, strlen(text), out_id);
}

bool lora_meshtastic_send_nodeinfo(void)
{
    if (s_node_num == 0) {
        lora_meshtastic_init();
    }

    // We emit only id/long_name/short_name
    // is_licensed is omitted (proto3 default false), so normal receivers list us; by Meshtastic design a ham-licensed
    // receiver (owner.is_licensed=true) drops NodeInfo on an is_licensed mismatch, so we are simply invisible to those nodes
    uint8_t user[96];
    size_t user_len = mesh_encode_user(user, sizeof(user), s_node_id, MESH_LONG_NAME, MESH_SHORT_NAME);
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "TX nodeinfo -> mesh: %s (%s / %s)", s_node_id, MESH_LONG_NAME, MESH_SHORT_NAME);
#endif
    return mesh_send_packet(MESHTASTIC_PORT_NODEINFO, user, user_len, NULL);
}

// RX decode + print

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

// Decode a User payload: log it and store the names against the roster node
static void mesh_handle_user(uint32_t from, const uint8_t *buf, size_t len)
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
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "   NodeInfo: id=%s long=\"%s\" short=\"%s\"", id, ln, sn);
#endif
    mesh_node_set_names(from, ln, sn);
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
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "   Position: lat=%s%u.%07u lon=%s%u.%07u alt=%dm time=%u",
                 (lat_i < 0) ? "-" : "", lat_mag / 10000000u, lat_mag % 10000000u,
                 (lon_i < 0) ? "-" : "", lon_mag / 10000000u, lon_mag % 10000000u,
                 (int)alt, (unsigned)t);
#endif
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "   Position: (no fix) alt=%dm time=%u", (int)alt, (unsigned)t);
#endif
    }
}

// Track packet ids we've already stored so mesh relays of the same message
// don't appear twice in the portal. RX event task only, so no lock needed.
// Returns true if this id was already seen (and thus should be dropped).
static bool mesh_rx_is_duplicate(uint32_t id)
{
    for (uint8_t i = 0; i < MESHTASTIC_MSG_LOG_CAP; i++) {
        if (s_seen_ids[i] == id) {
            return true;
        }
    }
    s_seen_ids[s_seen_pos] = id;
    s_seen_pos = (uint8_t)((s_seen_pos + 1) % MESHTASTIC_MSG_LOG_CAP);
    return false;
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
    uint8_t  hops = (hop_start >= hop_limit) ? (uint8_t)(hop_start - hop_limit) : 0;

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "RX !%08x → %s id=0x%08x ch=0x%02x hops=%u/%u rssi=%d snr=%d len=%u%s",
             (unsigned)from, (to == 0xFFFFFFFFu) ? "ALL" : "node",
             (unsigned)id, chan, hop_limit, hop_start, rssi, snr, (unsigned)len,
             (from == s_node_num) ? " (self)" : "");
#endif

    // Roster every node we hear (any channel) with its signal/hops/last-heard; NodeInfo below fills in names
    mesh_node_seen(from, rssi, snr, hops);

    // We only hold the default LongFast key, so only ch 0x08 packets decrypt.
    if (chan != MESHTASTIC_CHANNEL_HASH) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "   (channel 0x%02x not ours [0x%02x] — skipping decrypt)",
                 chan, MESHTASTIC_CHANNEL_HASH);
#endif
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

    if (inner == NULL) {
        inner = payload; // Valid address; inner_len == 0, so nothing is read from it
    }

    switch (portnum) {
    case MESHTASTIC_PORT_TEXT:
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "   TEXT: \"%.*s\"", (int)inner_len, (const char *)inner);
#endif
        if (from == s_node_num) {
            // Our own broadcast relayed back by a neighbour = implicit delivery ACK
            mesh_log_mark_acked(id);
        } else if (!(id != 0 && mesh_rx_is_duplicate(id))) {
            // Surface to the web portal, dropping duplicate relays of a stored one
            mesh_log_append(from, false, rssi, snr, hops, inner, inner_len, id, false);
        }
        break;
    case MESHTASTIC_PORT_NODEINFO:
        mesh_handle_user(from, inner, inner_len);
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

    // Re-arm continuous RX, but never while a TX we started is still on air, and
    // only while a portal session is active - once the page closes we let the
    // radio fall to standby (a TX_DONE here just clears s_tx_busy, no re-arm)
    if (!s_tx_busy && s_listening) {
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

void lora_meshtastic_resume_rx(void)
{
    // Only re-arm if a portal session is active; otherwise the radio should stay in the standby it woke into
    if (s_radio_mtx == NULL || !s_listening) {
        return;
    }
    // A light-sleep entry idled the radio (set_standby + clear IRQ) while a TX
    // may have been in flight; that TX is gone, so clear the stale gate and put
    // the radio back into continuous RX so the node hears the mesh again.
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    s_tx_busy = false;
    mesh_enter_rx();
    xSemaphoreGive(s_radio_mtx);
}

void lora_meshtastic_listen_start(void)
{
    // Portal page opened: start listening for the mesh and announce ourselves
    if (!g_meshtastic_mode || s_radio_mtx == NULL) {
        return;
    }

    // Start each session with an empty TX queue so a message that somehow survived
    // a previous session's close (a send racing the reset) can never broadcast now
    if (s_tx_queue != NULL) {
        xQueueReset(s_tx_queue);
    }
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    s_tx_busy   = false;
    s_listening = true;
    mesh_enter_rx();
    xSemaphoreGive(s_radio_mtx);

    // Let the run task (sole TX producer) broadcast our NodeInfo for this session
    s_announce_pending = true;
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Meshtastic: listening (portal open)");
#endif
}

void lora_meshtastic_listen_stop(void)
{
    // Portal page closed: idle the radio to standby to stop draining the battery
    if (s_radio_mtx == NULL) {
        return;
    }
    xSemaphoreTake(s_radio_mtx, portMAX_DELAY);
    s_listening        = false;
    s_announce_pending = false;
    s_tx_busy          = false;
    sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
    sx126x_clear_irq_status(NULL, SX126X_IRQ_ALL);
    xSemaphoreGive(s_radio_mtx);

    // Drop any text queued but not yet transmitted this session
    if (s_tx_queue != NULL) {
        xQueueReset(s_tx_queue);
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Meshtastic: idle (portal closed)");
#endif
}

void lora_meshtastic_run(void)
{
    lora_meshtastic_init();
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "=== Meshtastic mode ENABLED (radio idle until portal opens) ===");
    ESP_LOGI(TAG, "Node %s (0x%08x)  LongFast/US  %u.%03u MHz  ch-hash 0x%02x",
             s_node_id, (unsigned)s_node_num,
             (unsigned)(MESHTASTIC_LORA_FREQ_HZ / 1000000UL),
             (unsigned)((MESHTASTIC_LORA_FREQ_HZ / 1000UL) % 1000UL),
             MESHTASTIC_CHANNEL_HASH);
#endif

    // The radio stays in standby until the user opens the Meshtastic portal page
    TickType_t last_nodeinfo = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();

        // Only touch the radio while a portal session is active
        if (s_listening && !s_tx_busy) {
            mesh_tx_item_t tx_item;

            // Guard the handle too
            if (s_tx_queue != NULL && xQueueReceive(s_tx_queue, &tx_item, 0) == pdTRUE) {
                uint32_t tx_id = 0;
                bool tx_ok = mesh_send_user_text(tx_item.text, &tx_id);

                // Echo our own message into the log so the portal shows what the user sent,
                // flagged as failed when the radio never actually put it on air
                mesh_log_append(s_node_num, true, 0, 0, 0,
                                (const uint8_t *)tx_item.text, strlen(tx_item.text), tx_id, !tx_ok);
            } else if (s_announce_pending) {
                lora_meshtastic_send_nodeinfo(); // Announce at session start
                s_announce_pending = false;
                last_nodeinfo = now;
            } else if ((now - last_nodeinfo) >= pdMS_TO_TICKS(MESHTASTIC_NODEINFO_PERIOD_MS)) {
                lora_meshtastic_send_nodeinfo();
                last_nodeinfo = now;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
