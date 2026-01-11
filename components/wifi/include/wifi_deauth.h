#ifndef WIFI_DEAUTH_H
#define WIFI_DEAUTH_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "wifi_utils.h"

typedef struct {
    char ssid[33];
    uint8_t bssid[WIFI_MAX_NETWORKS][6];
    uint8_t channels[WIFI_MAX_NETWORKS]; // Channel for each BSSID
    uint8_t bssid_count; // Number of BSSIDs stored for this SSID
    int8_t rssi;
    uint8_t channel; // Channel of the strongest AP (for display)
    uint8_t auth;
    bool pmf_required; // True if PMF is required (not attackable)
    bool pmf_capable; // True if PMF is capable
    int freq_mhz; // Frequency in MHz for 2.4/5GHz display
} wifi_scan_deauth_t;

typedef struct {
    // Copied from wifi_scan_deauth_t entry
    uint8_t bssid[WIFI_MAX_NETWORKS][6];
    uint8_t bssid_count;
    uint8_t channels[WIFI_MAX_NETWORKS]; // Channel for each BSSID
    uint8_t channel; // Currently active channel (updated during attack)
    char ssid[33];

    // Handled by deauth function
    uint32_t frames_sent;
    uint32_t duration_sec;
    uint16_t seq_nums[WIFI_MAX_NETWORKS]; // Sequence number for each BSSID
} deauth_target_t;

typedef struct {
    bool deauthing;
    uint32_t frames_sent;
    uint32_t duration_sec;
} deauth_stats_t;

/**
 * @brief Scan for networks suitable for deauth as (excludes PMF-required networks)
 *
 * @param [out] wifi_scan_deauth Array to store scan results
 *
 * @returns ESP error status
 */
esp_err_t wifi_deauth_scan(wifi_scan_deauth_t *wifi_scan_deauth);

/**
 * @brief Sends deauthentication frames for a specified duration
 *
 * @param [in] deauth_target Pointer to deauth target configuration
 * 
 * @returns ESP error status
 */
esp_err_t wifi_deauth_send_for_duration(deauth_target_t *deauth_target);

#endif // WIFI_DEAUTH_H