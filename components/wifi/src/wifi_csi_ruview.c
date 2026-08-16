#include "polycast5_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "wifi_csi.h"
#include "wifi_csi_ruview.h"
#include "wifi_utils.h"

#define TAG "WIFI_CSI_RUVIEW"

// ADR-018 raw CSI frame. Layout verified against the host parser
// (v2/crates/wifi-densepose-sensing-server/src/csi.rs::parse_esp32_frame) at release v2235:
//   magic u32 LE @0, node_id u8 @4, n_antennas u8 @5, n_subcarriers u16 LE @6,
//   freq_mhz u32 LE @8, sequence u32 LE @12, rssi i8 @16, noise i8 @17,
//   ppdu_type u8 @18, flags u8 @19, interleaved I/Q from @20.
#define RUVIEW_MAGIC_CSI 0xC5110001u
#define RUVIEW_HEADER_SIZE 20

// The host rejects a frame whose payload is shorter than 20 + n_antennas * n_subcarriers * 2
#define RUVIEW_ANTENNAS 1

// Cap the outgoing rate. Capture can exceed 100 Hz on a busy channel while the host's pipeline
// expects something closer to 20 Hz, and there is no benefit in flooding it.
#define RUVIEW_MAX_HZ 50
#define RUVIEW_MIN_INTERVAL_US (1000000 / RUVIEW_MAX_HZ)

// flags byte 19
#define RUVIEW_FLAG_BW40 (1U << 0)

// Written by whichever task tears the session down, read by csi_task on the send path
static volatile int s_sock = -1;
static struct sockaddr_in s_dst;
static uint32_t s_sequence = 0;
static int64_t s_last_send_us = 0;
static wifi_csi_ruview_stats_t s_stats;

// Header plus the largest payload this chip can produce
static uint8_t s_tx[RUVIEW_HEADER_SIZE + WIFI_CSI_MAX_BYTES];

/**
 * @brief Work out where to send when no explicit host address was configured
 *
 * Broadcasting to the local subnet means the stream reaches a laptop whose address the user has
 * not had to type in. The host binds 0.0.0.0 so it receives it.
 */
static esp_err_t ruview_default_dest(struct in_addr *out)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;

    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    out->s_addr = ip.ip.addr | ~ip.netmask.addr;

    return ESP_OK;
}

esp_err_t wifi_csi_ruview_start(const char *host_ip, uint16_t port, uint8_t node_id)
{
    wifi_csi_ruview_stop();

    memset(&s_stats, 0, sizeof(s_stats));
    s_sequence = 0;
    s_last_send_us = 0;

    // Derive a node id from the MAC when none was given, so two PolyCast5s on one host differ
    if (node_id == 0) {
        uint8_t mac[6] = {0};

        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            node_id = mac[5] ? mac[5] : 1;
        } else {
            node_id = 1;
        }
    }

    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port = htons(port);

    bool broadcast = false;

    if (host_ip && host_ip[0]) {
        if (inet_aton(host_ip, &s_dst.sin_addr) == 0) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "Bad host address '%s'", host_ip);
#endif
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        if (ruview_default_dest(&s_dst.sin_addr) != ESP_OK) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGE(TAG, "No IP address yet, cannot derive a broadcast target");
#endif
            return ESP_ERR_INVALID_STATE;
        }

        broadcast = true;
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s_sock < 0) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
#endif
        return ESP_FAIL;
    }

    // Not a blocking guarantee. lwip_sendto() ignores O_NONBLOCK on the UDP path, and with
    // CONFIG_LWIP_TCPIP_CORE_LOCKING off it posts to the tcpip mailbox and waits on a semaphore
    // with no timeout. That wait is a short preemption by the higher-priority tcpip task rather
    // than a queue stall. Set the flag anyway so the descriptor is consistently non-blocking.
    int flags = fcntl(s_sock, F_GETFL, 0);
    fcntl(s_sock, F_SETFL, flags | O_NONBLOCK);

    if (broadcast) {
        int on = 1;
        setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }

    s_stats.node_id = node_id;
    s_stats.dest_ip = ntohl(s_dst.sin_addr.s_addr);
    s_stats.dest_port = port;

#ifdef POLYCAST5_DEBUG
    uint32_t d = s_stats.dest_ip;

    ESP_LOGI(TAG, "Streaming to %u.%u.%u.%u:%u as node %u%s",
            (unsigned)(d >> 24) & 0xFF, (unsigned)(d >> 16) & 0xFF,
            (unsigned)(d >> 8) & 0xFF, (unsigned)d & 0xFF,
            port, node_id, broadcast ? " (subnet broadcast)" : "");
#endif

    return ESP_OK;
}

void wifi_csi_ruview_stop(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

void wifi_csi_ruview_send(const wifi_csi_record_t *rec, uint8_t ppdu_type)
{
    if (s_sock < 0 || !rec || rec->len < 2) {
        return;
    }

    int64_t now = esp_timer_get_time();

    if (s_last_send_us && (now - s_last_send_us) < RUVIEW_MIN_INTERVAL_US) {
        s_stats.dropped_rate++;
        return;
    }

    uint16_t n_subcarriers = rec->len / (2 * RUVIEW_ANTENNAS);
    uint32_t freq_mhz = wifi_utils_channel_to_freq(rec->channel);
    uint8_t flags = 0;

    if (rec->second != 0) {
        flags |= RUVIEW_FLAG_BW40;
    }

    s_tx[0] = (uint8_t)(RUVIEW_MAGIC_CSI & 0xFF);
    s_tx[1] = (uint8_t)((RUVIEW_MAGIC_CSI >> 8) & 0xFF);
    s_tx[2] = (uint8_t)((RUVIEW_MAGIC_CSI >> 16) & 0xFF);
    s_tx[3] = (uint8_t)((RUVIEW_MAGIC_CSI >> 24) & 0xFF);
    s_tx[4] = s_stats.node_id;
    s_tx[5] = RUVIEW_ANTENNAS;
    s_tx[6] = (uint8_t)(n_subcarriers & 0xFF);
    s_tx[7] = (uint8_t)((n_subcarriers >> 8) & 0xFF);
    s_tx[8] = (uint8_t)(freq_mhz & 0xFF);
    s_tx[9] = (uint8_t)((freq_mhz >> 8) & 0xFF);
    s_tx[10] = (uint8_t)((freq_mhz >> 16) & 0xFF);
    s_tx[11] = (uint8_t)((freq_mhz >> 24) & 0xFF);
    s_tx[12] = (uint8_t)(s_sequence & 0xFF);
    s_tx[13] = (uint8_t)((s_sequence >> 8) & 0xFF);
    s_tx[14] = (uint8_t)((s_sequence >> 16) & 0xFF);
    s_tx[15] = (uint8_t)((s_sequence >> 24) & 0xFF);
    s_tx[16] = (uint8_t)rec->rssi;
    s_tx[17] = (uint8_t)rec->noise_floor;
    s_tx[18] = ppdu_type;
    s_tx[19] = flags;

    // Straight copy, imaginary byte first then real, exactly as the hardware produced it. The
    // host labels the first byte of each pair "i", so phase comes out conjugated there, but that
    // is true of every RuView node including their own and amplitude is unaffected.
    memcpy(&s_tx[RUVIEW_HEADER_SIZE], rec->iq, rec->len);

    size_t frame_size = RUVIEW_HEADER_SIZE + rec->len;

    // The cap is charged for the ATTEMPT, not the outcome. Advancing this only on success would
    // hold the gate wide open for the whole of a socket outage and retry at the full capture rate,
    // which is precisely the moment the stack has no buffers to spare.
    s_last_send_us = now;

    // Reload the descriptor rather than reusing the one the guard above read: teardown can close
    // it in between, and a number reused by another socket must not be handed a CSI frame
    int fd = s_sock;

    if (fd < 0) {
        return;
    }

    int sent = sendto(fd, s_tx, frame_size, 0, (struct sockaddr *)&s_dst, sizeof(s_dst));

    if (sent < 0) {
        s_stats.dropped_socket++;
        return;
    }

    s_sequence++;
    s_stats.sent++;
}

void wifi_csi_ruview_get_stats(wifi_csi_ruview_stats_t *out)
{
    if (out) {
        *out = s_stats;
    }
}
