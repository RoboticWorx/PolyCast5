#include "polycast5_macros.h"

#include <string.h>

#include <stdlib.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs.h"

#include "wifi_utils.h"

#include "wifi_task.h"

#define TAG "WIFI_AUTOCONNECT"

#define MAX_KNOWN_NETWORKS 20
#define NVS_NS "autoconnect"
#define NVS_KEY_COUNT "count"
#define NVS_KEY_LIST "list"

bool last_known_network_conn_failed = false;

POLYCAST5_USE_PSRAM_BSS static wifi_login_t known_networks[MAX_KNOWN_NETWORKS];
static size_t known_network_count = 0;

static wifi_login_t last_known_pick = {0};
static bool known_networks_loaded = false;

// Serializes access to known_networks[]/known_network_count/last_known_pick, which are
// touched by the Wi-Fi task mand the LCD task
static SemaphoreHandle_t s_known_mutex = NULL;
#define KNOWN_LOCK()   do { if (s_known_mutex) xSemaphoreTake(s_known_mutex, portMAX_DELAY); } while (0)
#define KNOWN_UNLOCK() do { if (s_known_mutex) xSemaphoreGive(s_known_mutex); } while (0)

static esp_err_t wifi_autoconnect_save_to_nvs(void)
{
    // Open NVS
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_autoconnect_save_to_nvs: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Write the blob (or erase it) FIRST, then the count
    if (known_network_count > 0) {
        size_t size = known_network_count * sizeof(known_networks[0]);
        err = nvs_set_blob(nvs, NVS_KEY_LIST, known_networks, size);
    } else {
        // No networks left: erase the blob so stored SSIDs/passwords don't linger in flash
        err = nvs_erase_key(nvs, NVS_KEY_LIST);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK; // Nothing to erase is not a failure
        }
    }

    // Store the count only after the blob is in place
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, NVS_KEY_COUNT, (uint32_t)known_network_count);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_autoconnect_save_to_nvs: nvs_set_u32 failed: %s", esp_err_to_name(err));
        }
    }

    // Commit on success
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_autoconnect_save_to_nvs: nvs_commit failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "wifi_autoconnect_save_to_nvs: save failed before commit: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);

    return err;
}

static void wifi_autoconnect_load_from_nvs(void)
{
    if (known_networks_loaded) {
        return;
    }

    known_networks_loaded = true;

    nvs_handle_t nvs;
    // Open NVS
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "wifi_autoconnect_load_from_nvs: nvs_open failed: %s", esp_err_to_name(err));
        }
        return;
    }

    // Get network count
    uint32_t count = 0;
    err = nvs_get_u32(nvs, NVS_KEY_COUNT, &count);
    if (err != ESP_OK || count == 0) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "wifi_autoconnect_load_from_nvs: nvs_get_u32 failed: %s", esp_err_to_name(err));
        }
        nvs_close(nvs);
        return;
    }

    // Read the blob into the FULL buffer and derive the entry count from the bytes actually stored - NOT from the count key
    size_t size = sizeof(known_networks);
    err = nvs_get_blob(nvs, NVS_KEY_LIST, known_networks, &size);
    if (err == ESP_OK) {
        known_network_count = size / sizeof(known_networks[0]);
        if (known_network_count > MAX_KNOWN_NETWORKS) {
            known_network_count = MAX_KNOWN_NETWORKS;
        }
    } else {
        ESP_LOGE(TAG, "wifi_autoconnect_load_from_nvs: nvs_get_blob failed: %s", esp_err_to_name(err));
    }

    nvs_close(nvs);
}

void wifi_autoconnect_init(void)
{
    // Create the guard mutex before any task can race the known-networks list
    if (!s_known_mutex) {
        s_known_mutex = xSemaphoreCreateMutex();
    }

    KNOWN_LOCK();
    wifi_autoconnect_load_from_nvs();
    KNOWN_UNLOCK();

    // Autofill default with last known
    wifi_config_t current = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &current);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_autoconnect_init: esp_wifi_get_config failed: %s", esp_err_to_name(err));
        return;
    }

    KNOWN_LOCK();
    strlcpy(last_known_pick.ssid, (char *)current.sta.ssid, sizeof(last_known_pick.ssid));
    strlcpy(last_known_pick.password, (char *)current.sta.password, sizeof(last_known_pick.password));

    // Only trust the cached pick if the network is still saved
    if (last_known_pick.ssid[0] != '\0') {
        bool still_known = false;
        for (size_t i = 0; i < known_network_count; ++i) {
            if (strncmp(known_networks[i].ssid, last_known_pick.ssid, sizeof(known_networks[i].ssid)) == 0) {
                still_known = true;
                break;
            }
        }
        if (!still_known) {
            memset(&last_known_pick, 0, sizeof(last_known_pick));
        }
    }
    KNOWN_UNLOCK();

#ifdef POLYCAST5_WIFI_DUMP_NETWORKS_NVS
    ESP_LOGI(TAG, "wifi_autoconnect_init: Loaded %u known Wi-Fi network(s) from NVS", (unsigned int)known_network_count);
    for (size_t i = 0; i < known_network_count; ++i) {
        ESP_LOGI(TAG, "[%u] SSID='%s', pass='%s'", (unsigned int)i,
                known_networks[i].ssid, known_networks[i].password);
    }
#endif
}

static void wifi_autoconnect_fill_password_from_known(wifi_login_t *net)
{
    // Check if valid network
    if (!net || net->ssid[0] == '\0' || net->password[0] != '\0') {
        return;
    }

    // See if we know this network
    for (size_t i = 0; i < known_network_count; ++i) {
        if (strncmp(known_networks[i].ssid, net->ssid, sizeof(known_networks[i].ssid)) == 0) {
            // Fill in password if we have it
            if (known_networks[i].password[0] != '\0') {
                strlcpy(net->password, known_networks[i].password, sizeof(net->password));
            }
            return;
        }
    }
}

static void wifi_autoconnect_remember_network(const wifi_login_t *net)
{
    // Check if valid network
    if (!net || net->ssid[0] == '\0') {
        return;
    }

    // See if we already know this network
    for (size_t i = 0; i < known_network_count; ++i) {
        if (strncmp(known_networks[i].ssid, net->ssid, sizeof(known_networks[i].ssid)) == 0) {
            wifi_login_t updated = *net;

            if (updated.password[0] == '\0' && known_networks[i].password[0] != '\0') {
                strlcpy(updated.password, known_networks[i].password, sizeof(updated.password));
            }

            known_networks[i] = updated;
            wifi_autoconnect_save_to_nvs();
            return;
        }
    }

    // If we have space, add it
    if (known_network_count < MAX_KNOWN_NETWORKS) {
        known_networks[known_network_count++] = *net;
        wifi_autoconnect_save_to_nvs();
    }
}

void wifi_autoconnect_remember_current_network(void)
{
    wifi_config_t current = {0};

    // Get current config
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &current);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_autoconnect_remember_current_network: esp_wifi_get_config failed: %s", esp_err_to_name(err));
        return;
    }

    // Copy SSID and password from current config
    wifi_login_t net = {0};
    strlcpy(net.ssid, (char *)current.sta.ssid, sizeof(net.ssid));
    strlcpy(net.password, (char *)current.sta.password, sizeof(net.password));

    // Copy BSSID if exists
    if (current.sta.bssid_set) {
        memcpy(net.bssid, current.sta.bssid, sizeof(net.bssid));
    }

    KNOWN_LOCK();
    wifi_autoconnect_remember_network(&net);
    wifi_autoconnect_fill_password_from_known(&net);
    last_known_pick = net;
    last_known_network_conn_failed = false;
    KNOWN_UNLOCK();
}

esp_err_t wifi_autoconnect_pick_known_network(wifi_login_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    KNOWN_LOCK();
    wifi_autoconnect_load_from_nvs();

    // If last pick was successful, return it for fast connect
    if (!last_known_network_conn_failed && last_known_pick.ssid[0] != '\0') {
        wifi_autoconnect_fill_password_from_known(&last_known_pick);
        *out = last_known_pick;
        KNOWN_UNLOCK();
#ifdef POLYCAST5_DEBUG_PASSWORDS
        ESP_LOGI(TAG, "Using last known network: SSID='%s', pass='%s'", out->ssid, out->password);
#else
        ESP_LOGI(TAG, "Using last known network: SSID='%s'", out->ssid);
#endif
        return ESP_OK;
    }

    // Snapshot the count, then release the lock for the (slow) scan below
    size_t known_cnt = known_network_count;
    KNOWN_UNLOCK();
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Last known network failed, scanning for best known network...");
#endif

    // Scan only after a failed attempt

    // If no known networks, return not found
    if (known_cnt == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    // Set Wi-Fi to station mode
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_autoconnect_pick_known_network: esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // Start Wi-Fi if not already started
    bool started = false;
    err = esp_wifi_start();
    if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
        started = true;
    } else {
        ESP_LOGE(TAG, "wifi_autoconnect_pick_known_network: esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 50, // At least 50ms each channel
                .max = 120 // At most 120ms each channel
            },
            .passive = 50, // 50ms for passive scan
        },
    };

    // Scan for network to connect to
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        if (started) {
            esp_wifi_stop();
        }
        ESP_LOGE(TAG, "wifi_autoconnect_pick_known_network: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Get number of APs found
    uint16_t ap_num = 0;
    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK || ap_num == 0) {
        if (started) {
            esp_wifi_stop();
        }
        ESP_LOGE(TAG, "wifi_autoconnect_pick_known_network: esp_wifi_scan_get_ap_num failed or no APs found: %s", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }

    // Allocate array to hold results
    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_list) {
        if (started) {
            esp_wifi_stop();
        }
        return ESP_ERR_NO_MEM;
    }

    // Pull the records
    err = esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    if (err != ESP_OK) {
        free(ap_list);
        if (started) {
            esp_wifi_stop();
        }
        ESP_LOGE(TAG, "wifi_autoconnect_pick_known_network: esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(err));
        return err;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Found %d access point(s):", ap_num);
    for (int i = 0; i < ap_num; ++i) {
        ESP_LOGI(TAG,
                "[%d] SSID: %-32s BSSID: %02x:%02x:%02x:%02x:%02x:%02x RSSI: %3d  CH:%2d  AUTH:%d",
                i,
                (char*)ap_list[i].ssid,
                ap_list[i].bssid[0], ap_list[i].bssid[1],
                ap_list[i].bssid[2], ap_list[i].bssid[3],
                ap_list[i].bssid[4], ap_list[i].bssid[5],
                ap_list[i].rssi,
                ap_list[i].primary,
                ap_list[i].authmode
        );
    }
#endif

    int best_rssi = -127;
    bool found = false;
    wifi_login_t best = {0};

    // Go through scan results (locked: known_networks may be mutated by other tasks)
    KNOWN_LOCK();
    for (uint16_t i = 0; i < ap_num; ++i) {
        const char *ssid = (char *)ap_list[i].ssid;

        if (ssid[0] == '\0') {
            continue;
        }

        // See if we know this network
        for (size_t j = 0; j < known_network_count; ++j) {
            if (strncmp(known_networks[j].ssid, ssid, sizeof(known_networks[j].ssid)) == 0) {
                // If stronger signal than previous best, remember it
                if (ap_list[i].rssi > best_rssi) {
                    best_rssi = ap_list[i].rssi;
                    best = known_networks[j];
                    memcpy(best.bssid, ap_list[i].bssid, sizeof(best.bssid));
                    found = true;
                }
                break;
            }
        }
    }
    KNOWN_UNLOCK();

    free(ap_list);

    // Stop Wi-Fi
    if (started) {
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_autoconnect_pick_known_network: esp_wifi_stop failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }

    // Set output to the known network scanned with the strongest RSSI
    KNOWN_LOCK();
    wifi_autoconnect_fill_password_from_known(&best);
    last_known_pick = best;
    last_known_network_conn_failed = false;
    KNOWN_UNLOCK();
    *out = best;
#ifdef POLYCAST5_DEBUG_PASSWORDS
    ESP_LOGI(TAG, "Found best network: "
            "Selected SSID='%s', pass='%s', BSSID='%02x:%02x:%02x:%02x:%02x:%02x', RSSI='%d'",
            best.ssid, best.password,
            best.bssid[0], best.bssid[1], best.bssid[2], best.bssid[3], best.bssid[4], best.bssid[5],
            best_rssi);
#else
    ESP_LOGI(TAG, "Found best network: "
            "Selected SSID='%s', BSSID='%02x:%02x:%02x:%02x:%02x:%02x', RSSI='%d'",
            best.ssid,
            best.bssid[0], best.bssid[1], best.bssid[2], best.bssid[3], best.bssid[4], best.bssid[5],
            best_rssi);
#endif
    return ESP_OK;
}

size_t wifi_autoconnect_get_known_count(void)
{
    KNOWN_LOCK();
    wifi_autoconnect_load_from_nvs();
    size_t cnt = known_network_count;
    KNOWN_UNLOCK();
    return cnt;
}

bool wifi_autoconnect_get_known_ssid(size_t i, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }

    KNOWN_LOCK();
    wifi_autoconnect_load_from_nvs();
    bool ok = (i < known_network_count);
    if (ok) {
        strlcpy(out, known_networks[i].ssid, out_size);
    }
    KNOWN_UNLOCK();
    return ok;
}

bool wifi_autoconnect_get_known_info(const char *ssid, wifi_login_t *out)
{
    if (!ssid || !out) {
        return false;
    }

    KNOWN_LOCK();
    wifi_autoconnect_load_from_nvs();
    bool found = false;
    for (size_t i = 0; i < known_network_count; ++i) {
        if (strncmp(known_networks[i].ssid, ssid, sizeof(known_networks[i].ssid)) == 0) {
            *out = known_networks[i];
            found = true;
            break;
        }
    }
    KNOWN_UNLOCK();
    return found;
}

bool wifi_autoconnect_is_known(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return false;
    }

    KNOWN_LOCK();
    wifi_autoconnect_load_from_nvs();
    bool found = false;
    for (size_t i = 0; i < known_network_count; ++i) {
        if (strncmp(known_networks[i].ssid, ssid, sizeof(known_networks[i].ssid)) == 0) {
            found = true;
            break;
        }
    }
    KNOWN_UNLOCK();
    return found;
}

esp_err_t wifi_autoconnect_forget_network(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t save_err = ESP_OK;

    KNOWN_LOCK();
    wifi_autoconnect_load_from_nvs();

    // Remove the matching entry from the known networks list (shift tail down)
    bool removed = false;
    for (size_t i = 0; i < known_network_count; ++i) {
        if (strncmp(known_networks[i].ssid, ssid, sizeof(known_networks[i].ssid)) == 0) {
            for (size_t j = i; (j + 1) < known_network_count; ++j) {
                known_networks[j] = known_networks[j + 1];
            }
            known_network_count--;
            memset(&known_networks[known_network_count], 0, sizeof(known_networks[0]));
            removed = true;
            break;
        }
    }
    if (removed) {
        save_err = wifi_autoconnect_save_to_nvs();
    }

    // Invalidate the in-RAM fast-path pick if it points at the forgotten network
    if (strncmp(last_known_pick.ssid, ssid, sizeof(last_known_pick.ssid)) == 0) {
        memset(&last_known_pick, 0, sizeof(last_known_pick));
        last_known_network_conn_failed = true; // Force a rescan on next pick this session
    }
    KNOWN_UNLOCK();

    // Clear esp_wifi's persisted STA config if it matches so it can't resurface on reboot
    esp_err_t cfg_err = ESP_OK;
    wifi_config_t current = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK &&
            strncmp((char *)current.sta.ssid, ssid, sizeof(current.sta.ssid)) == 0) {
        // Config portals leave storage in RAM mode and never restore it; force FLASH so
        // the cleared config actually persists (otherwise the SSID returns from flash)
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);

        wifi_config_t empty = {0};
        cfg_err = esp_wifi_set_config(WIFI_IF_STA, &empty);
        if (cfg_err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_autoconnect_forget_network: esp_wifi_set_config(clear) failed: %s", esp_err_to_name(cfg_err));
        }
    }

    // Report list-persistence failures so the caller knows the removal may not survive a reboot
    (void)cfg_err;
    if (!removed) {
        return ESP_ERR_NOT_FOUND;
    }
    return save_err;
}