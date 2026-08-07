#ifndef WIFI_MITM_CAPTURE_H
#define WIFI_MITM_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Submit a relayed L3 packet for MitM readout
 *
 * Called from the IPv4/IPv6 forward hooks (both run in the lwIP TCPIP thread) for each packet
 * we relay. Cheaply filters to DNS (UDP/53), TLS (TCP/443) and HTTP (TCP/80), snapshots the
 * interesting ones into a queue, and a background task parses out DNS query names, TLS SNI
 * hostnames and HTTP Host headers and logs them to serial. Non-blocking; drops if the queue
 * is full. The capture task is created lazily on first use.
 *
 * @param [in] is_v6  True if l3 points at an IPv6 header, false for IPv4
 * @param [in] l3     Start of the (contiguous) L3 packet: IP header onward
 * @param [in] l3_len Length of the L3 packet in bytes
 */
void mitm_capture_submit(bool is_v6, const uint8_t *l3, uint16_t l3_len);

/**
 * @brief True if an L3 packet is name-bearing: a DNS query, a TCP/443 TLS handshake record, or a
 *        TCP/80 HTTP request. The relay forwards these regardless of size, so TLS handshakes (incl.
 *        large post-quantum ClientHellos) and HTTP requests complete even when the bulk-shed size
 *        filter would otherwise drop them.
 *
 * @param [in] is_v6  True if l3 points at an IPv6 header, false for IPv4
 * @param [in] l3     Start of the (contiguous) L3 packet: IP header onward
 * @param [in] l3_len Length of the contiguous L3 bytes available
 *
 * @returns true if the packet should be treated as name-bearing / high-priority
 */
bool mitm_capture_is_interesting(bool is_v6, const uint8_t *l3, uint16_t l3_len);

/**
 * @brief True if a packet is high-priority to relay regardless of size: everything
 *        mitm_capture_is_interesting() covers, plus QUIC handshake packets (UDP/443 long-header) so
 *        HTTP/3 connections establish instead of stalling into a slow TCP fallback. Forward-only
 *        (QUIC payloads are encrypted and not parsed for the readout).
 *
 * @param [in] is_v6  True if l3 points at an IPv6 header, false for IPv4
 * @param [in] l3     Start of the (contiguous) L3 packet: IP header onward
 * @param [in] l3_len Length of the contiguous L3 bytes available
 *
 * @returns true if the packet should be forwarded regardless of the size threshold
 */
bool mitm_capture_is_priority(bool is_v6, const uint8_t *l3, uint16_t l3_len);

#endif // WIFI_MITM_CAPTURE_H
