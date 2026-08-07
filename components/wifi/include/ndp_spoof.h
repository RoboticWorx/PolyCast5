#ifndef WIFI_NDP_SPOOF_H
#define WIFI_NDP_SPOOF_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief NDP operating mode
 *
 * NDP_MODE_KILL  flood Router Advertisements with Router Lifetime = 0 to evict the router as
 *                everyone's IPv6 default gateway (denial).
 * NDP_MODE_MITM  poison neighbor caches (router link-local -> our MAC) via spoofed Neighbor
 *                Advertisements and SELECTIVELY relay the victims' IPv6 upstream to the real router:
 *                small/connection-establishing frames are forwarded (browsing works, the readout
 *                sees the sites), bulk upstream is dropped. Not a transparent relay -- uploads fail.
 */
typedef enum {
    NDP_MODE_OFF = 0,
    NDP_MODE_KILL,
    NDP_MODE_MITM,
} ndp_spoof_mode_t;

/**
 * @brief NDP target parameters
 */
typedef struct {
    ndp_spoof_mode_t mode; // Kill or MitM
    uint32_t duration_sec; // How long to run before self-stopping
} ndp_spoof_target_t;

/**
 * @brief Live NDP spoof stats pushed to the LCD
 */
typedef struct {
    bool spoofing; // False on the final message once stopped
    bool router_learned; // True once we've captured the real router's link-local from an RA
    uint32_t ras_sent; // Spoofed Router Advertisements broadcast
    uint32_t tx_drops; // Relay re-inject failures (Wi-Fi TX buffer exhaustion) in MitM mode
    uint32_t duration_sec; // Configured run duration
} ndp_spoof_stats_t;

/**
 * @brief Start the IPv6 NDP spoof (KILL or MITM) for the target's duration
 *
 * Learns the real router from inbound Router Advertisements, then either (KILL) floods spoofed RAs
 * with router lifetime 0 to evict it as the default gateway and kill IPv6 for the subnet, or (MITM)
 * poisons neighbor caches and selectively relays the victims' upstream (see ndp_spoof_mode_t).
 * Requires an active STA connection. Keeps the STA link up; never stops the radio.
 *
 * @param [in] target Mode + duration
 *
 * @returns ESP error status
 */
esp_err_t ndp_spoof_start(ndp_spoof_target_t *target);

/**
 * @brief True while the NDP spoof worker is alive (starting, running, or healing on stop)
 */
bool ndp_spoof_is_running(void);

#endif // WIFI_NDP_SPOOF_H
