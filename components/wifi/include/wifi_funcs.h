#ifndef WIFI_FUNCS_H
#define WIFI_FUNCS_H

#include <stdbool.h>

#include "esp_err.h"

#define WIFI_MAX_NETWORKS 40

#define WIFI_CONN_TIMEOUT_MS 10000

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
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;
    bool pmf_required; // True if PMF is required (not attackable)
    bool pmf_capable;  // True if PMF is capable
    int freq_mhz;      // Frequency in MHz for 2.4/5GHz display
} wifi_scan_deauth_t;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    char password[65];
    bool locked; // If network requires a password
    bool prev; // If connecting to the last known network
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
    int8_t  rssi;
} wifi_data_clients_t;

typedef struct {
    wifi_data_clients_t clients[MAX_MAC_CLIENTS];
    uint32_t client_count;
    uint32_t rate;
    uint32_t channel;
} wifi_data_t;

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    uint32_t packets_sent; // TODO: Technically frames
    uint16_t seq_num;
    uint32_t duration_sec;
} deauth_target_t;

typedef struct {
    bool deauthing;
    uint32_t packets_sent; // TODO: Technically frames
    uint32_t duration_sec;
} deauth_stats_t;

typedef struct {
    uint8_t key[16];
    char payload[4];
} wifi_mqtt_t;

typedef struct {
    int32_t rtt_gateway;
    int32_t rtt_dns;
} wifi_ping_t;

/**
 * @brief Gets previous Wi-Fi config from NVS
 *
 * @returns Wi-Fi login information
 */
wifi_login_t wifi_funcs_get_prev(void);

/**
 * @brief Scan and print available networks
 *
 * @param [out] wifi_scan Wi-Fi scan structure
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_scan(wifi_scan_t *wifi_scan);

/**
 * @brief Connect to a given Wi-Fi network
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_connect(void);

/**
 * @brief Configure and start the radio to join a given network
 *
 * @param [in] ssid Network SSID
 * @param [in] bssid Network BSSID
 * @param [in] password Network password
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_radio_start(const char *ssid, const uint8_t* bssid, const char *password);

/**
 * @brief Disconnects from MQTT and Wi-Fi, then stops Wi-Fi
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_radio_stop(void);

/**
 * @brief Turn on/off Wi-Fi radio to nudge driver
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_radio_cycle(void);

/**
 * @brief Creates MQTT ESP event group
 */
void wifi_funcs_wifi_event_init(void);

/**
 * @brief Initialize the MQTT client
 */
void wifi_funcs_mqtt_client_init(void);

/**
 * @brief Destroy/deinitialize the MQTT client
 */
void wifi_funcs_mqtt_client_destroy(void);

/**
 * @brief Stop the MQTT client
 */
void wifi_funcs_mqtt_client_stop(void);

/**
 * @brief Start the MQTT client
 */
void wifi_funcs_mqtt_client_start(void);

/**
 * @brief Initializes Wi-Fi promiscuous mode to sniff packets
 *
 * @param [in] network Network to sniff
 */
void wifi_funcs_init_promiscuous(wifi_sniff_t *network);

/**
 * @brief Ping the current gateway to get RTT
 *
 * @param [out] rtt_ms Round-trip time in milliseconds
 * 
 * @returns ESP error status
 */
esp_err_t wifi_funcs_ping_gateway(int32_t *rtt_ms);

/**
 * @brief Ping a given host to get RTT
 *
 * @param [in] host Hostname or IP address to ping
 * @param [out] rtt_ms Round-trip time in milliseconds
 * 
 * @returns ESP error status
 */
esp_err_t wifi_funcs_ping(const char *host, int32_t *rtt_ms);

/**
 * @brief Scan for networks suitable for deauth as (excludes PMF-required networks)
 *
 * @param [out] wifi_scan_deauth Array to store scan results
 *
 * @returns ESP error status
 */
esp_err_t wifi_funcs_scan_deauth(wifi_scan_deauth_t *wifi_scan_deauth);

/**
 * @brief Sends deauthentication frames for a specified duration
 *
 * @param [in] duration_sec Duration to send deauthentication frames for in seconds
 * @param [in] target_bssid Target's AP BSSID address
 * @param [in] channel Wi-Fi channel to send on
 * 
 * @returns ESP error status
 */
esp_err_t wifi_funcs_deauth_for_duration(uint32_t duration_sec, const uint8_t *target_bssid, uint8_t channel);

/**
 * @brief Gets the current date and time from pool.ntp
 */
void wifi_funcs_get_current_date_time(void);

/**
 * @brief Sends data via MQTT to receiver
 *
 * @param [in] payload Data to send
 * @param [in] key Unique topic key to filter
 */
void wifi_funcs_mqtt_client_publish(char *payload, const uint8_t key[16]);

#endif // WIFI_FUNCS_H