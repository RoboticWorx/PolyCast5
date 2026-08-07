#include "polycast5_macros.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h" // xQueueCreateWithCaps / vQueueDeleteWithCaps

#include "esp_log.h"
#include "esp_attr.h"       // EXT_RAM_BSS_ATTR (via POLYCAST5_USE_PSRAM_BSS)
#include "esp_heap_caps.h"  // MALLOC_CAP_SPIRAM

#include "mitm_capture.h"

#define TAG "MITM"

#define CAP_SNAP_LEN 384 // Bytes snapshotted per packet (enough to reach the TLS SNI)
#define CAP_Q_DEPTH 8

typedef struct {
    bool is_v6;
    uint16_t len;
    uint8_t data[CAP_SNAP_LEN];
} cap_item_t;

static QueueHandle_t s_cap_q = NULL;
static TaskHandle_t s_cap_task = NULL;

// Reused only from the TCPIP thread (both hooks) -> no locking needed. Kept in PSRAM: it's only
// CPU-touched (never DMA'd) and off the relay's TX path, so it spares scarce internal RAM.
POLYCAST5_USE_PSRAM_BSS static cap_item_t s_stage;

/**
 * @brief Locate the TCP/UDP header inside an L3 packet
 *
 * @returns true and fills the out-params for a plain (no IPv6 ext headers) TCP/UDP packet
 */
static bool locate_l4(bool is_v6, const uint8_t *l3, uint16_t len, uint8_t *proto, uint16_t *l4off, uint16_t *dport)
{
    uint16_t off; // Byte offset of the L4 (TCP/UDP) header inside the L3 packet
    uint8_t pr;   // L4 protocol number

    if (!is_v6) {
        // IPv4 header: byte 0 low nibble = IHL (length in 32-bit words), byte 9 = protocol
        if (len < 20) {
            return false; // Smaller than the minimum IPv4 header
        }
        uint8_t ihl = (uint8_t)((l3[0] & 0x0f) * 4); // Header length in bytes (20 + options)
        if (ihl < 20 || ihl > len) {
            return false; // Bogus, or options run past what we captured
        }
        pr = l3[9];  // Protocol field
        off = ihl;   // L4 starts right after the (variable-length) IPv4 header
    } else {
        // IPv6 header: fixed 40 bytes, byte 6 = next header (we don't walk extension headers)
        if (len < 40) {
            return false;
        }
        pr = l3[6]; // Next header (only the no-extension-header case is handled)
        off = 40;
    }

    if (pr != 6 && pr != 17) { // 6 = TCP, 17 = UDP; ignore anything else
        return false;
    }
    if ((uint16_t)(off + 4) > len) {
        return false; // Not enough bytes for the src+dst port pair
    }

    // TCP and UDP both begin with src port (2 bytes) then dst port (2 bytes), big-endian
    *proto = pr;
    *l4off = off;
    *dport = (uint16_t)((l3[off + 2] << 8) | l3[off + 3]); // Destination port
    return true;
}

/**
 * @brief Format the source address of an L3 packet for logging
 */
static void fmt_srcip(bool is_v6, const uint8_t *l3, char *buf, size_t buflen)
{
    if (!is_v6) {
        snprintf(buf, buflen, "%u.%u.%u.%u", l3[12], l3[13], l3[14], l3[15]);
    } else {
        const uint8_t *a = l3 + 8;
        snprintf(buf, buflen,
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    }
}

/**
 * @brief Replace non-printable bytes with '.' so attacker-controlled names (from DNS/SNI/HTTP)
 *        can't inject terminal escape sequences into the serial log
 */
static void sanitize_str(char *s)
{
    for (; *s; ++s) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) {
            *s = '.';
        }
    }
}

/**
 * @brief Parse a DNS query name and log it (queries only)
 */
static void log_dns(const char *srcip, const uint8_t *dns, uint16_t len)
{
    // DNS header is 12 bytes: [0-1] id, [2-3] flags, [4-5] qdcount (question count), ...
    if (len < 12) {
        return;
    }
    if (dns[2] & 0x80) {
        return; // Flags high bit (QR) set = response; we only want the query names
    }
    uint16_t qd = (uint16_t)((dns[4] << 8) | dns[5]); // Number of questions
    if (qd < 1) {
        return;
    }

    // QNAME is a chain of labels: [len][len bytes][len][len bytes]...[0]. Rebuild into "a.b.c".
    char name[128];
    int ni = 0;
    uint16_t o = 12; // Question section (QNAME) starts right after the 12-byte header

    while (o < len) {
        uint8_t l = dns[o++]; // Length of the next label
        if (l == 0) {
            break; // Zero-length root label -> end of the name
        }
        if ((l & 0xc0) != 0) {
            return; // Top 2 bits set = compression pointer/reserved (not expected in a question)
        }
        if ((uint16_t)(o + l) > len) {
            return; // Label runs past what we captured
        }
        if (ni != 0 && ni < (int)sizeof(name) - 1) {
            name[ni++] = '.'; // Separate labels with dots (but not before the first)
        }
        for (int i = 0; i < l && ni < (int)sizeof(name) - 1; ++i) {
            name[ni++] = (char)dns[o + i]; // Copy this label's characters
        }
        o += l; // Jump to the next length byte
    }
    name[ni] = '\0';

    if (ni > 0) {
        sanitize_str(name);
        ESP_LOGI(TAG, "DNS  %-15s -> %s", srcip, name);
    }
}

/**
 * @brief Parse a TLS ClientHello for the SNI server name and log it
 */
static void log_sni(const char *srcip, const uint8_t *t, uint16_t len)
{
    // Walk the ClientHello field-by-field. `p` is a size_t and every advance is validated with
    // SUBTRACTION against the bytes that remain (never `p + attacker_len`, whose uint16 form can
    // wrap and defeat the check). TLS record header: [0] content type (0x16 = handshake),
    // [1-2] version, [3-4] record length -> the handshake message begins at byte 5.
    if (len < 43 || t[0] != 0x16) {
        return;
    }
    size_t p = 5;

    // Handshake header: [5] msg type (0x01 = ClientHello), [6-8] length -> body begins at byte 9
    if (p + 4 > len || t[p] != 0x01) {
        return;
    }
    p += 4;

    // ClientHello body opens with client_version (2) + random (32) = 34 fixed bytes
    if (p + 34 > len) {
        return;
    }
    p += 34;

    // session_id: 1-byte length, then that many bytes
    if (p + 1 > len) {
        return;
    }
    uint8_t sid = t[p];
    p += 1;
    if (sid > len - p) { // Must fit in what remains (len - p can't underflow: p <= len here)
        return;
    }
    p += sid;

    // cipher_suites: 2-byte length, then that many bytes
    if (p + 2 > len) {
        return;
    }
    uint16_t cs = (uint16_t)((t[p] << 8) | t[p + 1]);
    p += 2;
    if (cs > len - p) {
        return;
    }
    p += cs;

    // compression_methods: 1-byte length, then that many bytes
    if (p + 1 > len) {
        return;
    }
    uint8_t cm = t[p];
    p += 1;
    if (cm > len - p) {
        return;
    }
    p += cm;

    // extensions: 2-byte total length, then a series of {type(2), length(2), data} blocks
    if (p + 2 > len) {
        return;
    }
    uint16_t exttot = (uint16_t)((t[p] << 8) | t[p + 1]);
    p += 2;
    size_t end = p + exttot;
    if (end > len) {
        end = len; // Clamp to what we actually captured
    }

    // Scan the extension list for Server Name Indication (extension type 0x0000)
    while (p + 4 <= end) {
        uint16_t etype = (uint16_t)((t[p] << 8) | t[p + 1]);    // Extension type
        uint16_t elen = (uint16_t)((t[p + 2] << 8) | t[p + 3]); // Extension data length
        p += 4;
        if (elen > end - p) {
            break; // Extension data runs past the extensions block
        }
        // SNI data: server_name_list_len(2), then name_type(1), name_len(2), name
        if (etype == 0x0000 && elen >= 5) {
            size_t q = p + 2;    // Skip the 2-byte server_name_list length
            uint8_t nt = t[q];   // Name type (0 = host_name)
            uint16_t nl = (uint16_t)((t[q + 1] << 8) | t[q + 2]); // Host name length
            q += 3;
            if (nt == 0 && nl > 0 && nl < 128 && nl <= (p + elen) - q) {
                char host[128];
                memcpy(host, &t[q], nl); // The server hostname, sent in the clear
                host[nl] = '\0';
                sanitize_str(host);
                ESP_LOGI(TAG, "TLS  %-15s -> %s", srcip, host);
                return;
            }
        }
        p += elen; // Advance to the next extension
    }
}

/**
 * @brief Parse an HTTP request line + Host header and log the full URL
 *
 * Plaintext HTTP exposes the whole request, so (unlike HTTPS/SNI, which is domain-only) we can
 * reconstruct method's request-URI + Host into a full http://host/path?query URL.
 */
static void log_http(const char *srcip, const uint8_t *d, uint16_t len)
{
    if (len < 16) {
        return;
    }
    // Only parse request packets (they begin with a method); skip responses and mid-stream data
    if (memcmp(d, "GET ", 4) != 0 && memcmp(d, "POST", 4) != 0 &&
            memcmp(d, "HEAD", 4) != 0 && memcmp(d, "PUT ", 4) != 0) {
        return;
    }

    // Request line looks like: "GET /path?query HTTP/1.1\r\n". Grab the middle token (the URI) by
    // skipping to the first space (end of method), then copying until the next space or newline.
    char path[192];
    int pi = 0;
    uint16_t s = 0;
    while (s < len && d[s] != ' ') { // Walk past the method (GET/POST/HEAD/PUT)
        s++;
    }
    s++; // Step over the space onto the request-URI
    while (s < len && d[s] != ' ' && d[s] != '\r' && d[s] != '\n' && pi < (int)sizeof(path) - 1) {
        path[pi++] = (char)d[s++]; // Copy the path (+ query string) up to the next space/newline
    }
    path[pi] = '\0';

    // Find the "Host:" header (case-insensitive) to get the domain. At each position, test for
    // H-o-s-t-':', then read the value after any leading spaces up to the end of that header line.
    char host[128];
    int hi = 0;
    for (uint16_t i = 0; (uint16_t)(i + 6) < len; ++i) {
        if ((d[i] == 'H' || d[i] == 'h') && (d[i + 1] == 'o' || d[i + 1] == 'O') &&
                (d[i + 2] == 's' || d[i + 2] == 'S') && (d[i + 3] == 't' || d[i + 3] == 'T') &&
                d[i + 4] == ':') {
            uint16_t j = (uint16_t)(i + 5); // Just past "Host:"
            while (j < len && d[j] == ' ') { // Skip leading spaces in the value
                j++;
            }
            while (j < len && d[j] != '\r' && d[j] != '\n' && hi < (int)sizeof(host) - 1) {
                host[hi++] = (char)d[j++]; // Copy the hostname until CR/LF
            }
            break;
        }
    }
    host[hi] = '\0';

    sanitize_str(host); // Both are attacker-controlled -> strip control chars before logging
    sanitize_str(path);

    // Log the fullest thing we could reconstruct: http://host/path, else whichever half we got
    if (hi > 0 && pi > 0) {
        ESP_LOGI(TAG, "HTTP %-15s -> http://%s%s", srcip, host, path);
    } else if (hi > 0) {
        ESP_LOGI(TAG, "HTTP %-15s -> %s", srcip, host);
    } else if (pi > 0) {
        ESP_LOGI(TAG, "HTTP %-15s -> %s", srcip, path);
    }
}

// Runs in the capture task: turn one snapshotted packet into a serial line (DNS/SNI/URL)
static void mitm_capture_parse(const cap_item_t *it)
{
    // Find the L4 header + destination port, then format the victim's source IP for the log line
    uint8_t proto;
    uint16_t l4off, dport;
    if (!locate_l4(it->is_v6, it->data, it->len, &proto, &l4off, &dport)) {
        return;
    }

    char srcip[48];
    fmt_srcip(it->is_v6, it->data, srcip, sizeof(srcip));

    if (proto == 17 && dport == 53) { // UDP/53 -> DNS. Payload is right after the 8-byte UDP header
        uint16_t off = (uint16_t)(l4off + 8);
        if (off < it->len) {
            log_dns(srcip, it->data + off, (uint16_t)(it->len - off));
        }
        return;
    }

    if (proto == 6) { // TCP -> the data offset is variable (TCP options), so compute it
        if ((uint16_t)(l4off + 13) >= it->len) {
            return; // Can't read the data-offset byte
        }
        // TCP byte 12, high nibble = header length in 32-bit words
        uint16_t tcphl = (uint16_t)(((it->data[l4off + 12] >> 4) & 0x0f) * 4);
        if (tcphl < 20) {
            return; // Malformed: a real TCP header is at least 20 bytes
        }
        uint16_t off = (uint16_t)(l4off + tcphl); // Start of the TCP payload
        if (off >= it->len) {
            return; // No payload (e.g. a bare ACK)
        }
        const uint8_t *payload = it->data + off;
        uint16_t plen = (uint16_t)(it->len - off);
        if (dport == 443) {
            log_sni(srcip, payload, plen);   // HTTPS -> TLS SNI (domain only)
        } else if (dport == 80) {
            log_http(srcip, payload, plen);  // HTTP -> full URL
        }
    }
}

static void mitm_capture_task(void *arg)
{
    (void)arg;
    POLYCAST5_USE_PSRAM_BSS static cap_item_t item; // Large; PSRAM, off both the task stack and internal RAM
    for (;;) {
        if (xQueueReceive(s_cap_q, &item, portMAX_DELAY) == pdTRUE) {
            mitm_capture_parse(&item);
        }
    }
}

/**
 * @brief True for a packet worth capturing/forwarding: a DNS query (UDP/53), a TLS handshake
 *        record (TCP/443 payload starting 0x16) or an HTTP request (TCP/80 payload starting with a
 *        method). The relay also uses this to always forward these name-bearing packets regardless
 *        of size, so TLS handshakes (incl. large post-quantum ClientHellos) and HTTP requests
 *        complete even when the bulk-shed size filter would otherwise drop them.
 */
bool mitm_capture_is_interesting(bool is_v6, const uint8_t *l3, uint16_t l3_len)
{
    if (l3 == NULL || l3_len == 0) {
        return false;
    }
    uint8_t proto;
    uint16_t l4off, dport;
    if (!locate_l4(is_v6, l3, l3_len, &proto, &l4off, &dport)) {
        return false;
    }
    if (proto == 17 && dport == 53) {
        return true; // Every DNS query (small, low volume)
    }
    if (proto == 6 && (dport == 443 || dport == 80)) {
        // Peek the first payload byte, past the TCP options. This ignores the flood of bare ACKs
        // and mid-stream data, catching only the name-bearing packet even when it fills the MTU
        // (e.g. a large post-quantum TLS ClientHello).
        if ((uint16_t)(l4off + 13) < l3_len) {
            uint16_t tcphl = (uint16_t)(((l3[l4off + 12] >> 4) & 0x0f) * 4); // TCP header length
            uint16_t poff = (uint16_t)(l4off + tcphl); // Start of TCP payload
            if (poff < l3_len) {
                uint8_t f = l3[poff]; // First payload byte (0 for a pure ACK)
                return (dport == 443) ? (f == 0x16)                      // TLS handshake record
                                      : (f == 'G' || f == 'P' || f == 'H'); // GET/POST/PUT/HEAD
            }
        }
    }
    return false;
}

/**
 * @brief True if a packet must be relayed regardless of size so a connection can establish. Covers
 *        everything mitm_capture_is_interesting() does (DNS, TLS ClientHello, HTTP request) plus
 *        QUIC handshake packets (UDP/443 long-header). QUIC's Initial fills the MTU, and shedding it
 *        makes HTTP/3 stall for seconds before the browser falls back to TCP. QUIC payloads aren't
 *        parsed for the readout (encrypted), so this is forward-only - the domain still shows via
 *        the DNS query.
 */
bool mitm_capture_is_priority(bool is_v6, const uint8_t *l3, uint16_t l3_len)
{
    if (mitm_capture_is_interesting(is_v6, l3, l3_len)) {
        return true;
    }
    // QUIC handshake: UDP/443 whose first QUIC byte is a long header (top two bits set = header
    // form + fixed bit, the invariant for Initial/Handshake packets). Short-header QUIC (1-RTT
    // data/ACKs) falls through to the size rule - small ACKs forward, bulk uploads shed.
    uint8_t proto;
    uint16_t l4off, dport;
    if (!locate_l4(is_v6, l3, l3_len, &proto, &l4off, &dport)) {
        return false;
    }
    if (proto == 17 && dport == 443) {
        uint16_t poff = (uint16_t)(l4off + 8); // Skip the 8-byte UDP header to the QUIC first byte
        if (poff < l3_len) {
            return (l3[poff] & 0xC0) == 0xC0;
        }
    }
    return false;
}

void mitm_capture_submit(bool is_v6, const uint8_t *l3, uint16_t l3_len)
{
    // Lazy one-time init (only ever called from the single TCPIP thread)
    if (s_cap_q == NULL) {
        // Queue storage in PSRAM (task-context access only) to spare internal RAM
        s_cap_q = xQueueCreateWithCaps(CAP_Q_DEPTH, sizeof(cap_item_t), MALLOC_CAP_SPIRAM);
        if (s_cap_q == NULL) {
            return;
        }
        if (xTaskCreate(mitm_capture_task, "mitm_cap", (1024 * 4), NULL,
                        POLYCAST5_PRIORITY_MEDIUM, &s_cap_task) != pdPASS) {
            vQueueDeleteWithCaps(s_cap_q);
            s_cap_q = NULL;
            return;
        }
    }

    // Only the packets actually carrying a name are worth queueing (keeps the flood of bare TCP
    // ACKs during a download from evicting the ClientHello/request).
    if (!mitm_capture_is_interesting(is_v6, l3, l3_len)) {
        return;
    }

    // Snapshot the head of the packet (bounded) into the shared staging item and hand it to the
    // parser task. Non-blocking: if the queue is full we just drop this one.
    uint16_t n = l3_len > CAP_SNAP_LEN ? CAP_SNAP_LEN : l3_len;
    s_stage.is_v6 = is_v6;
    s_stage.len = n;
    memcpy(s_stage.data, l3, n);
    (void)xQueueSend(s_cap_q, &s_stage, 0); // Non-blocking; drop if full
}
