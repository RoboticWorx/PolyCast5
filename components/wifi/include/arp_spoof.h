#ifndef WIFI_ARP_SPOOF_H
#define WIFI_ARP_SPOOF_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief ARP spoof operating mode
 *
 * ARP_MODE_POISON  redirect the subnet's upstream traffic to us and drop it (deny/DoS + on-path setup).
 * ARP_MODE_FORWARD same poison, but SELECTIVELY relay redirected frames to the real gateway: small
 *                  and connection-establishing frames (DNS, TLS/QUIC handshakes, HTTP requests, ACKs)
 *                  are forwarded so victims can browse and the readout sees the sites; bulk upstream
 *                  (large uploads / POST bodies) is intentionally dropped so one STA can't stall the
 *                  subnet. Not a transparent relay -- browsing works, heavy uploads fail.
 */
typedef enum {
    ARP_MODE_OFF = 0,
    ARP_MODE_POISON,
    ARP_MODE_FORWARD,
} arp_spoof_mode_t;

/**
 * @brief ARP spoof target parameters (whole-subnet gateway impersonation)
 */
typedef struct {
    arp_spoof_mode_t mode; // Poison-only or forward
    uint32_t duration_sec; // How long to run before self-stopping
} arp_spoof_target_t;

/**
 * @brief Live ARP spoof stats pushed to the LCD
 */
typedef struct {
    bool spoofing; // False on the final message once stopped
    arp_spoof_mode_t mode; // Active mode
    uint32_t arps_sent; // Forged ARP frames broadcast
    uint32_t pkts_handled; // Redirected packets dropped (poison) or relayed (forward)
    uint32_t tx_drops; // Relay re-inject failures (Wi-Fi TX buffer exhaustion) in forward mode
    uint32_t duration_sec; // Configured run duration
} arp_spoof_stats_t;

/**
 * @brief Start the whole-subnet ARP spoof for the target's duration
 *
 * Broadcasts a gratuitous ARP mapping the gateway IP to our STA MAC so every host on the
 * subnet redirects its upstream traffic to us. Requires an active STA connection (must be
 * associated with a valid gateway). Keeps the STA link up; never stops the radio.
 *
 * @param [in] target Mode + duration (whole subnet is implied)
 *
 * @returns ESP error status
 */
esp_err_t arp_spoof_start(arp_spoof_target_t *target);

/**
 * @brief True while the ARP spoof worker is alive (starting, running, or healing on stop)
 */
bool arp_spoof_is_running(void);

#endif // WIFI_ARP_SPOOF_H
