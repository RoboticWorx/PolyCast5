#ifndef WIFI_CSI_H
#define WIFI_CSI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Valid CSI payload lengths on the ESP32-C5 are 106/114/228/234/468/490 bytes: two bytes per
// subcarrier, imaginary part first, real part second. 512 rounds the 490 maximum up for alignment.
#define WIFI_CSI_MIN_BYTES 100
#define WIFI_CSI_MAX_BYTES 512
#define WIFI_CSI_MAX_SC (WIFI_CSI_MAX_BYTES / 2)

// Staging slots live in internal RAM and are written from the Wi-Fi RX path, so keep this small.
// The analysis ring lives in PSRAM and is only ever touched by csi_task. Both are powers of two.
#define WIFI_CSI_STAGE_DEPTH 4
#define WIFI_CSI_RING_DEPTH 64

// Where the sounding traffic that carries the CSI comes from
typedef enum {
    WIFI_CSI_SRC_ASSOC_PING = 0, // Join the saved AP and ping the gateway (~50 Hz)
    WIFI_CSI_SRC_PROMISC,        // Passive on beacons, no credentials, degraded to ~10 Hz
} wifi_csi_source_t;

// Consumers of the capture stream, combined as a bitmask
#define WIFI_CSI_CONSUMER_LOCAL  (1U << 0) // On-device motion detection
#define WIFI_CSI_CONSUMER_RUVIEW (1U << 1) // UDP stream to a RuView host

// Session state, mirrored to the LCD
enum {
    WIFI_CSI_STATE_IDLE = 0,
    WIFI_CSI_STATE_CONNECTING,
    WIFI_CSI_STATE_BASELINING,
    WIFI_CSI_STATE_QUIET,
    WIFI_CSI_STATE_MOTION,
    WIFI_CSI_STATE_INVALID, // Signal too weak or rate too low to trust
    WIFI_CSI_STATE_MOVED,   // Device was picked up, baseline is void
    WIFI_CSI_STATE_EXPIRED, // Hit the session time cap
    WIFI_CSI_STATE_ERROR,
};

/** One captured CSI frame, copied out of the driver buffer before it is freed */
typedef struct {
    uint32_t seq;               // Monotonic capture counter
    uint32_t timestamp_us;      // rx_ctrl.timestamp, only precise with power save off
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;            // Primary channel
    uint8_t second;             // Secondary channel when in HT40, wifi_second_chan_t 0/1/2.
                                // rx_ctrl.second is declared 8 bits but the driver only writes
                                // the low nibble, so it is masked on capture.
    uint8_t bb_format;          // rx_ctrl.cur_bb_format, see wifi_rx_bb_format_t
    uint8_t rate;
    uint8_t first_word_invalid; // First four bytes unusable, a hardware limitation
    uint8_t mac[6];             // Source MAC of the frame the CSI came from
    uint16_t len;               // Valid bytes in iq
    int8_t iq[WIFI_CSI_MAX_BYTES];
} wifi_csi_record_t;

/** Capture health counters, published to the LCD so a bad session is visible rather than silent */
typedef struct {
    uint32_t captured;
    uint32_t dropped;  // Staging ring was full
    uint32_t invalid;  // Driver flagged the channel estimate invalid
    uint32_t badlen;   // Length outside the range this chip can produce
    uint32_t mismatch; // Length or PHY format changed mid-session
    uint16_t frames_per_sec;
    uint16_t n_subcarriers;
    int8_t rssi;
    uint8_t bb_format;
} wifi_csi_stats_t;

/** Session request, filled by the LCD and posted to xWifiCsiCmdQueue */
typedef struct {
    uint8_t start;              // 1 starts a session, 0 stops it
    uint8_t source;             // wifi_csi_source_t
    uint8_t consumers;          // WIFI_CSI_CONSUMER_* bitmask
    uint8_t channel;            // Only used by WIFI_CSI_SRC_PROMISC
    uint16_t sound_interval_ms; // Must be a multiple of the 10 ms FreeRTOS tick
    uint16_t host_port;         // RuView consumer only
    uint8_t node_id;            // RuView consumer only
    char host_ip[16];           // RuView consumer only, empty means subnet broadcast
} wifi_csi_cmd_t;

/** Status published to the LCD at ~10 Hz */
typedef struct {
    uint8_t state;         // WIFI_CSI_STATE_*
    uint8_t score;         // Motion score, 0-100
    uint8_t baseline_pct;  // Progress while baselining
    wifi_csi_stats_t stats;
    uint32_t seconds_since_motion;
    uint32_t session_seconds;
} wifi_csi_status_t;

/**
 * @brief Start a CSI capture session
 *
 * Brings the radio up for the requested source, configures CSI acquisition and spawns csi_task.
 * Any live promiscuous sniff or deauth is stopped first: the radio has one tuner.
 *
 * @param [in] cmd Session request
 *
 * @return ESP_OK on success, an esp_err_t otherwise
 */
esp_err_t wifi_csi_session_start(const wifi_csi_cmd_t *cmd);

/**
 * @brief Ask the running session to stop and block until csi_task has exited
 */
void wifi_csi_session_stop(void);

/**
 * @brief Whether a capture session is currently running
 */
bool wifi_csi_is_active(void);

/**
 * @brief Tear down all CSI state, safe to call when no session is running
 *
 * Hooked into every path that takes the radio down so a session can never outlive the radio.
 * Overrides the weak stub in the ESP-NOW component.
 */
void wifi_csi_teardown(void);

#endif // WIFI_CSI_H
