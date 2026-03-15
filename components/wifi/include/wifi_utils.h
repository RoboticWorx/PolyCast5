#ifndef WIFI_UTILS_H
#define WIFI_UTILS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_MAX_NETWORKS 40

#define WIFI_CONN_TIMEOUT_MS 12000
#define WIFI_CONN_SCAN_TIMEOUT_MS 6000 // Time until retry connection via scan

#define MAX_MAC_CLIENTS 100

// RSN cipher types (store as 1u<<type)
#define RSN_CIPHER_TKIP 2
#define RSN_CIPHER_CCMP_128 4 // AES/CCMP
#define RSN_CIPHER_GCMP_128 8
#define RSN_CIPHER_CCMP_256 10
#define RSN_CIPHER_GCMP_256 9

// RSN AKM types (store as 1u<<type)
#define RSN_AKM_8021X 1
#define RSN_AKM_PSK 2
#define RSN_AKM_SAE 8 // WPA3-Personal
#define RSN_AKM_OWE 18 // Enhanced Open (OWE)

/* Helpers to pull type/subtype from the 802.11 frame control */
#define FC_TYPE(fc)    (((fc) & 0x0C) >> 2)
#define FC_SUBTYPE(fc) (((fc) & 0xF0) >> 4)
#define TYPE_MGMT 0x00
#define SUBTYPE_BEACON 0x08
#define SUBTYPE_PROBE_RESP 0x05

#define WIFI_PROMIS_FILTER_MASK_RAW_USEFUL ( \
    WIFI_PROMIS_FILTER_MASK_MGMT       | \
    WIFI_PROMIS_FILTER_MASK_CTRL       | \
    WIFI_PROMIS_FILTER_MASK_DATA       | \
    WIFI_PROMIS_FILTER_MASK_MISC       | \
    WIFI_PROMIS_FILTER_MASK_DATA_MPDU  | \
    WIFI_PROMIS_FILTER_MASK_DATA_AMPDU   \
)
#define WIFI_MAX_RAW_FRAMES 150 // Max raw frames to capture for AI analysis

typedef struct {
    uint8_t ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;
} wifi_scan_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    char password[65];
    bool locked; // If network requires a password
} wifi_login_t;

typedef struct {
    uint8_t type;
    uint8_t channel;
    int mask;
    uint8_t target_bssid[6];
} wifi_sniff_t;

typedef enum {
    WIFI_PHY_UNKNOWN = 0,
    WIFI_PHY_11B,
    WIFI_PHY_11G,
    WIFI_PHY_11A,
    WIFI_PHY_11N,  // Wi-Fi 4
    WIFI_PHY_11AC, // Wi-Fi 5
    WIFI_PHY_11AX, // Wi-Fi 6
} wifi_phy_t;

typedef struct {
    char ssid[33];
    int channel;
    int freq;
    int rssi;
    int snr;

    uint16_t cap_info;
    uint16_t interval;
    uint64_t timestamp;

    bool rsn; // RSN IE present
    bool wpa; // WPA IE present (vendor 221: 00:50:F2:01)
    bool wps; // WPS IE present (vendor 221: 00:50:F2:04)

    // RSN details
    uint8_t  rsn_group_cipher; // Cipher type (e.g. 2 TKIP, 4 CCMP, 8 GCMP)
    uint32_t rsn_pairwise_ciphers; // Bitmask: (1u<<cipher_type)
    uint32_t rsn_akm_suites; // Bitmask: (1u<<akm_type)
    bool pmf_capable; // MFPC
    bool pmf_required; // MFPR

    // Wi-Fi PHY type
    wifi_phy_t phy;
    bool ht;
    bool vht;
    bool he;
} wifi_beacon_t;

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t pkt_count; // Number of sniffed data frames attributed to this MAC
} wifi_data_clients_t;

typedef struct {
    wifi_data_clients_t clients[MAX_MAC_CLIENTS];
    uint32_t client_count;
    uint32_t rate;
    uint32_t channel;
} wifi_data_t;

/**
 * @brief Gets previous Wi-Fi config from NVS
 *
 * @returns Wi-Fi login information
 */
wifi_login_t wifi_utils_get_prev(void);

/**
 * @brief Scan and print available networks
 *
 * @param [out] wifi_scan Wi-Fi scan structure
 *
 * @returns ESP error status
 */
esp_err_t wifi_utils_scan(wifi_scan_t *wifi_scan);

/**
 * @brief Creates ESP Wi-Fi event group
 */
void wifi_utils_wifi_event_init(void);

/**
 * @brief Connect to a given Wi-Fi network
 *
 * @returns ESP error status
 */
esp_err_t wifi_utils_connect(void);

/**
 * @brief Configure and start the radio to join a given network
 *
 * @param [in] ssid Network SSID
 * @param [in] bssid Network BSSID
 * @param [in] password Network password
 *
 * @returns ESP error status
 */
esp_err_t wifi_utils_radio_start(const char *ssid, const uint8_t* bssid, const char *password);

/**
 * @brief Disconnects from MQTT and Wi-Fi, then stops Wi-Fi
 *
 * @returns ESP error status
 */
esp_err_t wifi_utils_radio_stop(void);

/**
 * @brief Turn on/off Wi-Fi radio to nudge driver
 *
 * @returns ESP error status
 */
esp_err_t wifi_utils_radio_cycle(void);

/**
 * @brief Initializes Wi-Fi promiscuous mode to sniff packets
 *
 * @param [in] network Network to sniff
 */
void wifi_utils_init_promiscuous(wifi_sniff_t *network);

/**
 * @brief Parses the RSN IE from a beacon/probe response
 *
 * @param [in] rsn Pointer to the RSN IE body
 * @param [in] rsn_len Length of the RSN IE body
 * @param [out] out Parsed beacon information
 */
void wifi_utils_parse_rsn_ie(const uint8_t *rsn, size_t rsn_len, wifi_beacon_t *out);

/**
 * @brief Gets the current date and time from pool.ntp
 */
void wifi_utils_get_current_date_time(void);


#endif // WIFI_UTILS_H