#include "polycast5_macros.h"

#include <stdbool.h>
#include <stdint.h>

#include "nvs.h"
#include "esp_err.h"
#include "esp_log.h"

#include "bluetooth_utils.h"
#include "bluetooth_task.h"

#define TAG "BLUETOOTH_NVS"

#define PAIRING_KEY_NS "bt_key"
#define PAIRING_KEY_KEY "key"

// Bond index (NVS-only; UI uses this while BT is OFF)
#define BT_IDX_NS "bt_index"
#define BT_IDX_KEY "peers"

#define BT_PEERS_NS "bt_peers"
#define BT_PEERS_KEY "peers"
#define BT_PEERS_PERF_KEY "pref_peer"

extern volatile bluetooth_state_t bluetooth_state;

int bluetooth_nvs_get_peers_list(bluetooth_peer_info_t *out, int max)
{
    // Guard
    if (!out || max <= 0) {
        ESP_LOGW(TAG, "bluetooth_nvs_get_peers_list: invalid args");
        return -1;
    }

    // Open NVS - missing namespace is legit empty (first boot or after wipe)
    nvs_handle_t h;
    esp_err_t err = nvs_open(BT_IDX_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return 0;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bluetooth_nvs_get_peers_list: nvs_open failed: %s", esp_err_to_name(err));
        return -1;
    }

    // Read blob
    ble_addr_t tmp[BT_MAX_PEERS] = {0};
    size_t sz = sizeof(tmp);
    err = nvs_get_blob(h, BT_IDX_KEY, tmp, &sz);
    nvs_close(h);

    // Missing key is legit empty (no peers ever written)
    if (err == ESP_ERR_NVS_NOT_FOUND || sz == 0) {
        return 0;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bluetooth_nvs_get_peers_list: nvs_get_blob failed: %s", esp_err_to_name(err));
        return -1;
    }

    // Parse
    int n = (int)(sz / sizeof(ble_addr_t));
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        out[i].addr = tmp[i];
        out[i].label[0] = '\0';
    }

    // Done
    return n;
}

// Add peer to cache (idempotent; ring if full)
void bluetooth_nvs_add_to_peers_list(const ble_addr_t *peer)
{
    // Guard
    if (!peer) {
        return;
    }

    // Open NVS
    nvs_handle_t h;
    if (nvs_open(BT_IDX_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    // Read existing
    ble_addr_t tmp[BT_MAX_PEERS] = {0};
    size_t sz = sizeof(tmp);
    if (nvs_get_blob(h, BT_IDX_KEY, tmp, &sz) != ESP_OK) {
        sz = 0;
    }

    // Current count
    int n = (int)(sz / sizeof(ble_addr_t));

    // De-dup
    for (int i = 0; i < n; i++) {
        if (tmp[i].type == peer->type && memcmp(tmp[i].val, peer->val, 6) == 0) {
            nvs_close(h);
            return;
        }
    }

    // Append or rotate
    if (n < BT_MAX_PEERS) {
        tmp[n++] = *peer;
    } else {
        memmove(&tmp[0], &tmp[1], (BT_MAX_PEERS - 1) * sizeof(ble_addr_t));
        tmp[BT_MAX_PEERS - 1] = *peer;
    }

    // Write back
    if (nvs_set_blob(h, BT_IDX_KEY, tmp, n * sizeof(ble_addr_t)) == ESP_OK) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

// Clear all nvs peers
esp_err_t bluetooth_nvs_clear_peers_list(bool preferred_only)
{
    nvs_handle_t h;
    esp_err_t err;

    // If clearing both
    if (!preferred_only) {
        // Open BT_IDX_NS NVS
        err = nvs_open(BT_IDX_NS, NVS_READWRITE, &h);
        if (err != ESP_OK) {
            return err;
        }
    
        // Erase key
        err = nvs_erase_key(h, BT_IDX_KEY);
    
        // Commit 
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    
        nvs_close(h);
    }

    // Else only preferred peer
    // Open BT_PEERS_NS NVS
    err = nvs_open(BT_PEERS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Erase key
    err = nvs_erase_key(h, BT_PEERS_KEY);
    err = nvs_erase_key(h, BT_PEERS_PERF_KEY);

    // Commit and close
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);

    // Done
    return err;
}

// Save preferred peer (type + 6 bytes) to NVS
esp_err_t bluetooth_nvs_set_preferred_peer(const ble_addr_t *peer)
{
    // Open NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(BT_PEERS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Write blob
    uint8_t blob[7] = { peer->type, peer->val[0], peer->val[1], peer->val[2], peer->val[3], peer->val[4], peer->val[5] };
    err = nvs_set_blob(h, BT_PEERS_PERF_KEY, blob, sizeof(blob));

    // Commit and close
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    // Done
    return err;
}

// Load preferred peer from NVS
esp_err_t bluetooth_nvs_get_preferred_peer(ble_addr_t *out, bool *found)
{
    // Default
    if (found) {
        *found = false;
    }

    // Open NVS
    nvs_handle_t h;
    size_t sz = 7;
    uint8_t blob[7] = {0};
    esp_err_t err = nvs_open(BT_PEERS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Read
    err = nvs_get_blob(h, BT_PEERS_PERF_KEY, blob, &sz);
    nvs_close(h);

    // Parse blob
    if (err == ESP_OK && sz == 7) {
        out->type = blob[0];
        memcpy(out->val, &blob[1], 6);
        if (found) {
            *found = true;
        }
    }

    // Done
    return err;
}

/* =============== Pairing key =============== */

esp_err_t bluetooth_nvs_pairing_key_save(uint32_t key)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(PAIRING_KEY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Set the bt pairing key
    err = nvs_set_u32(h, PAIRING_KEY_KEY, key);
    
    // Persist changes if success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    
    // Close and return
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_nvs_pairing_key_load(uint32_t *key)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(PAIRING_KEY_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Get the saved bt pairing key
    err = nvs_get_u32(h, PAIRING_KEY_KEY, key);
    
    // Close and return
    nvs_close(h);
    return err;
}

/* =============== Get name label =============== */

static void bt_label_key_from_addr(const ble_addr_t *a, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%02X%02X%02X%02X%02X%02X",
            a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

esp_err_t bluetooth_nvs_set_peer_label(const ble_addr_t *addr, const char *label)
{
    // Guard
    if (!addr) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err;

    // Open NVS
    err = nvs_open(BT_PEERS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Write or erase
    char key[20];
    bt_label_key_from_addr(addr, key, sizeof(key));
    if (label && label[0]) {
        // Write label into address key
        err = nvs_set_str(h, key, label);
    } else {
        // Erase if DNE
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }

    // Commit on success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    // Close and return
    nvs_close(h);
    return err;
}

bool bluetooth_nvs_get_peer_label(const ble_addr_t *addr, char *out, size_t out_sz)
{
    // Guard
    if (!addr || !out || out_sz == 0) {
        return false;
    }

    nvs_handle_t h;

    // Open NVS
    if (nvs_open(BT_PEERS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    // Read address into label
    char key[20];
    bt_label_key_from_addr(addr, key, sizeof(key));

    // Use address key to get string
    size_t sz = out_sz;
    esp_err_t err = nvs_get_str(h, key, out, &sz);

    // Close
    nvs_close(h);

    // Normalize result
    if (err == ESP_OK) {
        out[out_sz - 1] = '\0';
        return true;
    }

    out[0] = '\0';
    return false;
}

esp_err_t bluetooth_nvs_remove_peer(const ble_addr_t *addr)
{
    // Guard
    if (!addr) {
        return ESP_ERR_INVALID_ARG;
    }

    // Remove from index cache (BT_IDX_NS / BT_IDX_KEY)
    nvs_handle_t h;
    esp_err_t err = nvs_open(BT_IDX_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    ble_addr_t tmp[BT_MAX_PEERS] = {0};
    size_t sz = sizeof(tmp);
    if (nvs_get_blob(h, BT_IDX_KEY, tmp, &sz) != ESP_OK) {
        sz = 0; // Treat as empty list
    }

    // Number of valid entries currently stored in the blob
    int n = (int)(sz / sizeof(ble_addr_t));
    
    // Write index for our compacted array after removing the target addr
    int out = 0;
    
    // Walk the current list and copy forward every entry that is NOT the one we're deleting
    for (int i = 0; i < n; ++i) {
        // Match if both address type (public/random) and 6-byte MAC are identical
        bool same = (tmp[i].type == addr->type) && (memcmp(tmp[i].val, addr->val, 6) == 0);
    
        // Keep only non-matching entries
        if (!same) {
            tmp[out++] = tmp[i];
        }
    }
    
    // Write the compacted list back to NVS
    // If there are still entries, overwrite the blob with the first 'out' elements
    if (out > 0) {
        err = nvs_set_blob(h, BT_IDX_KEY, tmp, out * sizeof(ble_addr_t));
    } else { // Otherwise, no entries remain: erase the key entirely to avoid empty blobs
        err = nvs_erase_key(h, BT_IDX_KEY);
        // Erasing a non-existent key isn't an error
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }

    // Commit on success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    // Close
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }

    // Drop per-address label
    bluetooth_nvs_set_peer_label(addr, NULL); // Passing NULL erases the label key

    // Clear preferred if it matches the removed peer
    ble_addr_t pref = {0};
    bool found = false;
    bluetooth_nvs_get_preferred_peer(&pref, &found);
    if (found && pref.type == addr->type && memcmp(pref.val, addr->val, 6) == 0) {
        bluetooth_nvs_clear_peers_list(true); // Only delete BT_PEERS_KEY
    }

    // Unpair from NimBLE so it won't auto-reconnect
    // Only valid while the stack is running; otherwise NimBLE's bond storage would be untouched and the peer could auto-reconnect on next BT init.
    if (bluetooth_state == BT_STATE_RUNNING) {
        ble_gap_unpair((ble_addr_t *)addr);
    }

    return ESP_OK;
}