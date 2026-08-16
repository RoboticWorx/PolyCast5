#ifndef WIFI_CSI_RUVIEW_H
#define WIFI_CSI_RUVIEW_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "wifi_csi.h"

/** Counters for the stream page, so a silent link is visible rather than assumed working */
typedef struct {
    uint32_t sent;
    uint32_t dropped_socket; // Send failed, usually a full lwIP transmit queue
    uint32_t dropped_rate;   // Above the configured cap
    uint32_t dest_ip;        // Where frames are actually going, host byte order
    uint16_t dest_port;
    uint8_t node_id;
} wifi_csi_ruview_stats_t;

/**
 * @brief Open the UDP socket used to stream CSI to a RuView host
 *
 * @param [in] host_ip Dotted-quad host address, or an empty string for subnet broadcast
 * @param [in] port Host UDP port, 5005 for a stock sensing-server
 * @param [in] node_id Identifier the host uses to separate nodes, 0 to derive one from the MAC
 *
 * @return ESP_OK on success, an esp_err_t otherwise
 */
esp_err_t wifi_csi_ruview_start(const char *host_ip, uint16_t port, uint8_t node_id);

/**
 * @brief Close the socket, safe to call when not started
 */
void wifi_csi_ruview_stop(void);

/**
 * @brief Pack one captured frame as an ADR-018 raw CSI datagram and send it
 *
 * Called from csi_task. A refused send is counted rather than retried. This briefly waits on the
 * lwIP tcpip task, which runs above csi_task, so it yields but does not stall on a full queue.
 *
 * @param [in] rec Captured frame
 * @param [in] ppdu_type Host PPDU bucket for byte 18, mapped by the caller from cur_bb_format
 */
void wifi_csi_ruview_send(const wifi_csi_record_t *rec, uint8_t ppdu_type);

/**
 * @brief Snapshot the transmit counters
 */
void wifi_csi_ruview_get_stats(wifi_csi_ruview_stats_t *out);

#endif // WIFI_CSI_RUVIEW_H
