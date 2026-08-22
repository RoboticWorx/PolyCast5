#include "bluetooth_task.h"
#include "driver/sdspi_host.h"
#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "cJSON.h"

#include "bluetooth_portal.h"
#include "bluetooth_portal_html.h"
#include "ai_utils.h"

#define TAG "BLUETOOTH_WEB_PORTAL"

#define WIFI_PASS_NS "wifi_pass"
#define WIFI_PASS_KEY "pass"

// All indexed by the same global script index: BT_SCRIPT_KEY_FMT, BT_SCRIPT_CAT_KEY_FMT, BT_SCRIPT_MENU_KEY_FMT
#define BT_SCRIPT_NS "bt_portal"
#define BT_SCRIPT_KEY_FMT "script_%02d"
#define BT_SCRIPT_CAT_KEY_FMT "cat_%02d"
#define BT_SCRIPT_CAT_COUNT "cat_count"
#define BT_SCRIPT_CAT_NAME_FMT "cat_name_%02d"

#define BT_SCRIPT_MENU_NS "keyb_menu"
#define BT_SCRIPT_MENU_KEY_COUNT "count"
#define BT_SCRIPT_MENU_KEY_FMT "item_%02d"

#define MAX_HTTP_BODY_TXT AI_RESPONSE_MAX_LEN

// Import carries every script at once; a single body can be ~64KB and JSON
// escaping roughly doubles the wire size, so allow well above MAX_HTTP_BODY_TXT.
#define MAX_IMPORT_BODY_TXT (512 * 1024)

// Backup file format tag + version (validated on import)
#define BT_EXPORT_FMT_TAG "polycast5-autotype"
#define BT_EXPORT_VER 1

extern char bt_wifi_portal_pass[]; // bluetooth_task.c

static httpd_handle_t bt_server = NULL;
static esp_netif_t *bt_ap_netif = NULL;
static char s_ip[16] = "192.168.4.1";

/* =============== NVS =============== */

// Write a payload body for the given script index
static esp_err_t bluetooth_script_body_set_nvs(uint16_t idx, const char *body)
{
    nvs_handle_t h;
     
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_body_set nvs_open failed: %s", esp_err_to_name(err));
#endif
       
       return err;
    }
    
    // Format string
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_KEY_FMT, idx);
    
    // Set body blob (blob supports multi-page NVS, str is limited to ~4000 bytes)
    const char *src = (body != NULL) ? body : "";
    err = nvs_set_blob(h, key, src, strlen(src) + 1); // +1 for NUL terminator
    if (err == ESP_OK) {
       // Commit changes on success
       err = nvs_commit(h);
    } else {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_body_set nvs_set_blob failed: %s", esp_err_to_name(err));
#endif
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

// Persist the count of user scripts
static esp_err_t bluetooth_script_count_set_nvs(uint16_t count)
{
    nvs_handle_t h;

    // Clamp to max (count is uint16_t and could exceed BT_MAX_KEYBOARD_SCRIPTS)
    if (count > BT_MAX_KEYBOARD_SCRIPTS) {
       count = BT_MAX_KEYBOARD_SCRIPTS;
       ESP_LOGW(TAG, "bluetooth_script_count_set BT_MAX_KEYBOARD_SCRIPTS reached: %d", BT_MAX_KEYBOARD_SCRIPTS);
    }

    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_MENU_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_count_set nvs_open failed: %s", esp_err_to_name(err));
#endif
       
       return err;
    }
    
    // Set count (u16: supports up to BT_MAX_KEYBOARD_SCRIPTS > 255)
    err = nvs_set_u16(h, BT_SCRIPT_MENU_KEY_COUNT, count);
    if (err == ESP_OK) {
       // Commit changes on success
       err = nvs_commit(h);
    } else {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_count_set nvs_set_u16 failed: %s", esp_err_to_name(err));
#endif
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

// Write a label for the given script index
static esp_err_t bluetooth_script_label_set_nvs(uint16_t idx, const char *label)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_MENU_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_label_set nvs_open failed: %s", esp_err_to_name(err));
#endif
       
         return err;
    }
    
    // Format key
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_MENU_KEY_FMT, idx);
    
    // Set the string
    err = nvs_set_str(h, key, (label != NULL) ? label : "");
    if (err == ESP_OK) {
       // Commit changes on success
         err = nvs_commit(h);
    } else {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_label_set nvs_set_str failed: %s", esp_err_to_name(err));
#endif
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

// Read the count of user scripts
uint16_t bluetooth_portal_script_count_get_nvs(void)
{
    nvs_handle_t h;
    uint16_t count = 0;

    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_MENU_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        // Get count (stored as u16)
        err = nvs_get_u16(h, BT_SCRIPT_MENU_KEY_COUNT, &count);
        nvs_close(h);

        if (err != ESP_OK) {
            // No u16 count found: either a fresh namespace, or a legacy device whose count is still stored as u8. 
            // Probe for a legacy u8 count read-only (a truly fresh device never gets a namespace/flash write) and migrate it to u16 if present; otherwise count stays 0
            count = 0;
            uint8_t legacy = 0;

            if (nvs_open(BT_SCRIPT_MENU_NS, NVS_READONLY, &h) == ESP_OK) {
                esp_err_t lerr = nvs_get_u8(h, BT_SCRIPT_MENU_KEY_COUNT, &legacy);
                nvs_close(h);

                if (lerr == ESP_OK) {
                    // Recovered a legacy u8 count: return it this boot regardless of the rewrite result
                    // A failed rewrite simply retries on the next boot
                    count = legacy;

                    // Rewrite as u16 so future reads are clean
                    if (nvs_open(BT_SCRIPT_MENU_NS, NVS_READWRITE, &h) == ESP_OK) {
                        if (nvs_set_u16(h, BT_SCRIPT_MENU_KEY_COUNT, count) == ESP_OK) {
                            nvs_commit(h);
                        }
                        nvs_close(h);
                    }
                }
            }
        }
    } else {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_count_get nvs_open failed: %s", esp_err_to_name(err));
#endif
    }

    // Clamp to max
    if (count > BT_MAX_KEYBOARD_SCRIPTS) {
        count = BT_MAX_KEYBOARD_SCRIPTS;
        ESP_LOGW(TAG, "bluetooth_script_count_get BT_MAX_KEYBOARD_SCRIPTS reached: %d", BT_MAX_KEYBOARD_SCRIPTS);
    }

    return count;
}

// Read a script label into caller buffer (buflen should be >= BLUETOOTH_SCRIPT_LABEL_MAX_LEN + 1)
esp_err_t bluetooth_portal_script_label_get_nvs(uint16_t idx, char *buf, size_t buflen)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_MENU_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_label_get nvs_open failed: %s", esp_err_to_name(err));
#endif
        return err;
    }
    
    // Format key
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_MENU_KEY_FMT, idx);
    
    size_t need = buflen;
    
    // Get the label string
    err = nvs_get_str(h, key, buf, &need);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGW(TAG, "bluetooth_script_label_get nvs_get_str failed: %s", esp_err_to_name(err));
#endif
    }
    
    // Close NVS
    nvs_close(h);

    return err;
}

// Read a payload body into caller buffer (need must fit into buflen)
esp_err_t bluetooth_portal_script_body_get_nvs(uint16_t idx, char *buf, size_t buflen, size_t *outlen)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_body_get nvs_open failed: %s", esp_err_to_name(err));
#endif
       return err;
    }
    
    // Format key
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_KEY_FMT, idx);

    size_t need = 0;

    // Get the body blob len (blob supports multi-page NVS, str is limited to ~4000 bytes)
    err = nvs_get_blob(h, key, NULL, &need);

    // If NVS good and size is within allowed
    if ((err == ESP_OK) && (need > 0) && (need <= buflen)) {
       // Get the actual body blob
         err = nvs_get_blob(h, key, buf, &need);
         if (err == ESP_OK) {
              buf[need - 1] = '\0'; // Ensure NUL-terminated
              if (outlen != NULL) {
                  *outlen = need;
              }
         }
    } else {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "bluetooth_script_body_get blob parameters wrong or NVS failed: %s", esp_err_to_name(err));
#endif
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

uint8_t bluetooth_portal_category_count_get_nvs(void)
{
    nvs_handle_t h;
    uint8_t count = 0;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        // Get count
        if (nvs_get_u8(h, BT_SCRIPT_CAT_COUNT, &count) != ESP_OK) {
            // 0 if DNE
            count = 0;
        }
        
        // Close NVS
        nvs_close(h);
    }
    
    return count;
}

// Persist the category id for a given script index
// Note: idx is the GLOBAL script index (matches script body + label indexing)
esp_err_t bluetooth_portal_script_cat_idx_set_nvs(uint16_t idx, uint8_t cat)
{
    // Open NVS under the same namespace used for bodies/cats
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "bluetooth_script_cat_set nvs_open failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Format key cat_%02d
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_CAT_KEY_FMT, idx);

    // Save u8
    err = nvs_set_u8(h, key, cat);
    if (err == ESP_OK) {
        // Commit changes on success
        err = nvs_commit(h);
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "bluetooth_script_cat_set nvs_set_u8 failed: %s", esp_err_to_name(err));
#endif
    }

    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_portal_script_cat_idx_get_nvs(uint16_t idx, uint8_t *cat)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_CAT_KEY_FMT, idx);
    
    err = nvs_get_u8(h, key, cat);
    if (err != ESP_OK) {
        *cat = 0; // Default to 0 if not found
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_portal_category_name_get_nvs(uint8_t idx, char *buf, size_t buflen)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_CAT_NAME_FMT, idx);
    
    size_t len = buflen;
    err = nvs_get_str(h, key, buf, &len);
    
    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_portal_category_set_nvs(uint8_t idx, const char *name)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Get current count
    uint8_t count = 0;
    nvs_get_u8(h, BT_SCRIPT_CAT_COUNT, &count);
    
    // If idx >= count, it's an add: update count
    if (idx >= count) {
        count = idx + 1;
        err = nvs_set_u8(h, BT_SCRIPT_CAT_COUNT, count);
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    }
    
    // Set name
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_CAT_NAME_FMT, idx);
    err = nvs_set_str(h, key, name);
    
    // Commit if success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_portal_category_delete_nvs(uint8_t idx)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Get current count
    uint8_t count = 0;
    nvs_get_u8(h, BT_SCRIPT_CAT_COUNT, &count);
    if (idx >= count) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Delete the category name
    char key[16];
    snprintf(key, sizeof(key), BT_SCRIPT_CAT_NAME_FMT, idx);
    nvs_erase_key(h, key);
    
    // Shift higher categories down
    for (uint8_t i = idx + 1; i < count; ++i) {
        char old_key[16];
        snprintf(old_key, sizeof(old_key), BT_SCRIPT_CAT_NAME_FMT, i);
        
        size_t len = BT_CAT_LABEL_MAX_LEN + 1;
        char buf[BT_CAT_LABEL_MAX_LEN + 1];
        if (nvs_get_str(h, old_key, buf, &len) == ESP_OK) {
            snprintf(key, sizeof(key), BT_SCRIPT_CAT_NAME_FMT, i - 1);
            nvs_set_str(h, key, buf);
            nvs_erase_key(h, old_key);
        }
    }
    
    // Decrement count
    count--;
    nvs_set_u8(h, BT_SCRIPT_CAT_COUNT, count);

    // Pre-allocate body buffer BEFORE committing category changes so OOM
    // cannot leave categories committed with scripts not cleaned up
    char *body_buf = (char *)malloc(MAX_HTTP_BODY_TXT + 1);
    if (body_buf == NULL) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }

    // Commit and close before script cleanup (script NVS functions open their own handles)
    err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        free(body_buf);
        return err;
    }

    // Delete scripts belonging to the removed category and update remaining cat indices
    // Iterate in reverse so that shift operations only affect already-processed positions
    esp_err_t script_err = ESP_OK;
    uint16_t sc = bluetooth_portal_script_count_get_nvs();
    char next_label[BT_SCRIPT_LABEL_MAX_LEN + 1];

    for (int s = (int)sc - 1; s >= 0; --s) {
        uint8_t cat = 0;
        if (bluetooth_portal_script_cat_idx_get_nvs((uint16_t)s, &cat) != ESP_OK) {
            script_err = ESP_FAIL;
            continue;
        }

        if (cat == idx) {
            // Delete this script: shift everything above it down by one
            // Track per-script success so we only finalize (decrement/clear) on full success
            bool shift_ok = true;

            for (uint16_t i = (uint16_t)s; i + 1 < sc; ++i) {
                next_label[0] = '\0';
                size_t blen = 0;
                uint8_t next_cat = 0;

                esp_err_t rl = bluetooth_portal_script_label_get_nvs(i + 1, next_label, sizeof(next_label));
                esp_err_t rb = bluetooth_portal_script_body_get_nvs(i + 1, body_buf, MAX_HTTP_BODY_TXT + 1, &blen);
                esp_err_t rc = bluetooth_portal_script_cat_idx_get_nvs(i + 1, &next_cat);

                if (rl != ESP_OK || rb != ESP_OK || rc != ESP_OK) {
                    shift_ok = false;
                    break; // Abort shift - leave existing data intact
                }

                if (bluetooth_script_label_set_nvs(i, next_label) != ESP_OK ||
                        bluetooth_script_body_set_nvs(i, (blen > 0) ? body_buf : "") != ESP_OK ||
                        bluetooth_portal_script_cat_idx_set_nvs(i, next_cat) != ESP_OK) {
                    shift_ok = false;
                    break;
                }
            }

            if (shift_ok) {
                // Shift succeeded - safe to clear tail and decrement count
                sc--;

                if (bluetooth_script_label_set_nvs(sc, "") != ESP_OK ||
                        bluetooth_script_body_set_nvs(sc, "") != ESP_OK ||
                        bluetooth_portal_script_cat_idx_set_nvs(sc, 0) != ESP_OK ||
                        bluetooth_script_count_set_nvs(sc) != ESP_OK) {
                    script_err = ESP_FAIL;
                }
            } else {
                // Shift failed - don't decrement or clear, stop further deletions
                script_err = ESP_FAIL;
                break;
            }
        } else if (cat > idx) {
            // Category index shifted down, update to match
            if (bluetooth_portal_script_cat_idx_set_nvs((uint16_t)s, cat - 1) != ESP_OK) {
                script_err = ESP_FAIL;
            }
        }
    }

    free(body_buf);

    // Category deletion was already committed
    // Script cleanup errors are logged but not surfaced to the caller:
    // returning failure would invite a retry on the already-shifted index, risking cascading data loss
    if (script_err != ESP_OK) {
        ESP_LOGE(TAG, "Category %u deleted but script cleanup had errors", (unsigned)idx);
    }

    return ESP_OK;
}

esp_err_t bluetooth_portal_wifi_pass_save_nvs(const char *val)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(WIFI_PASS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Set the version string
    err = nvs_set_str(h, WIFI_PASS_KEY, val);
    
    // Persist changes if success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    
    // Close and return
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_portal_wifi_pass_load_nvs(char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(WIFI_PASS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t len = out_sz; // Must include room for '\0'
    
    // Get the saved version string
    err = nvs_get_str(h, WIFI_PASS_KEY, out, &len);
    
    // Close and return
    nvs_close(h);
    return err;
}

/* =============== HTTP handlers =============== */

// Serve the single-page HTML UI
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    
    return httpd_resp_send(req, BLUETOOTH_WEB_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /api/scripts -> {"count":N,"labels":[...]}
static esp_err_t scripts_list_get(httpd_req_t *req)
{
    // Get num current scripts
    uint16_t count = bluetooth_portal_script_count_get_nvs();

    // Allocate a JSON root object
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) { // Check
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
     
    // Add the field "count": <count> to the JSON root
    cJSON_AddNumberToObject(root, "count", count);
     
    // Creates an empty array as root["labels"] = []
    cJSON *labels = cJSON_AddArrayToObject(root, "labels");

    // Loop over each saved script index
    for (uint16_t i = 0; i < count; ++i) {
        char lbl[BT_SCRIPT_LABEL_MAX_LEN + 1] = {0}; // Buffer
        
        // Add the label or "" to the array
        if ((bluetooth_portal_script_label_get_nvs(i, lbl, sizeof(lbl)) == ESP_OK) && (lbl[0] != '\0')) {
             cJSON_AddItemToArray(labels, cJSON_CreateString(lbl));
        } else {
             cJSON_AddItemToArray(labels, cJSON_CreateString(""));
        }
    }

    // Serialize the JSON tree into a compact string then free the cJSON tree
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    // Error check
    if (txt == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Set Content-Type: application/json
    httpd_resp_set_type(req, "application/json");
     
    // Send the JSON text as the HTTP response
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "scripts_list_get httpd_resp_sendstr failed: %s", esp_err_to_name(err));
#endif
    }
     
    return err;
}

// Map (category, local_index) -> global script index in NVS
// If create==true and local_index == current number of scripts in that category:
// return the next free global index (append)
static bool resolve_global_index_for_local(uint8_t cat, uint16_t local_index, bool create, uint16_t *out_global)
{
    // Get total script count
    uint16_t total = bluetooth_portal_script_count_get_nvs();

    // Count how many scripts belong to 'cat' and remember their global positions in order.
    uint16_t seen = 0;
    for (uint16_t i = 0; i < total; ++i) {
        uint8_t c = 0;
        (void)bluetooth_portal_script_cat_idx_get_nvs(i, &c);

        if (c == cat) {
            // If we've reached the requested local_index, return this global position
            if (seen == local_index) {
                *out_global = i;
                return true;
            }
            seen++;
        }
    }
    
    // If not found and caller wants to create at the tail of this category,
    // allow appending a brand-new script at the end of the global list
    // when local_index == number of scripts in this category.
    // Reject if at capacity: total < BT_MAX_KEYBOARD_SCRIPTS allows append at the last
    // valid index (bumping count to BT_MAX_KEYBOARD_SCRIPTS); total == max is full
    if (create && local_index == seen && total < BT_MAX_KEYBOARD_SCRIPTS) {
        *out_global = total; // append at tail (new global index)
        return true;
    }
    
    // Otherwise, no mapping
    return false;
}

// GET /api/script?index=N[&cat=C]  -> {"index":GLOBAL,"name":"...","body":"...","cat":C}
// If &cat=C is present, 'index' is LOCAL within that category and we resolve it to a GLOBAL index.
static esp_err_t script_one_get(httpd_req_t *req)
{
    // Read query
    char qstr[64];
    if (httpd_req_get_url_query_str(req, qstr, sizeof(qstr)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
    }

    // Parse index
    char idx_str[8] = {0};
    if (httpd_query_key_value(qstr, "index", idx_str, sizeof(idx_str)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing index");
    }

    // Parse optional category
    char cat_str[8] = {0};
    bool has_cat = (httpd_query_key_value(qstr, "cat", cat_str, sizeof(cat_str)) == ESP_OK);

    // Resolve to GLOBAL index
    uint16_t global_idx = 0;
    if (has_cat) {
        // Interpret index as local within category
        uint16_t local_idx = (uint16_t)atoi(idx_str);
        uint8_t cat = (uint8_t)atoi(cat_str);

        // Resolve (no create)
        if (!resolve_global_index_for_local(cat, local_idx, false, &global_idx)) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        }
    } else {
        // Treat as global directly
        global_idx = (uint16_t)atoi(idx_str);

        // Validate range
        if (global_idx >= bluetooth_portal_script_count_get_nvs()) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        }
    }

    // Fetch fields
    char name[BT_SCRIPT_LABEL_MAX_LEN + 1] = {0};

    // Allocate body buffer
    char *body = (char *)malloc(MAX_HTTP_BODY_TXT + 1);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    body[0] = '\0';

    size_t  blen = 0;
    uint8_t cat  = 0;

    // Read label/body/cat
    bluetooth_portal_script_label_get_nvs(global_idx, name, sizeof(name));
    bluetooth_portal_script_body_get_nvs(global_idx, body, MAX_HTTP_BODY_TXT + 1, &blen);
    bluetooth_portal_script_cat_idx_get_nvs(global_idx, &cat);

    // Build JSON
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddNumberToObject(root, "index", (int)global_idx);
    cJSON_AddStringToObject(root, "name",  (name[0] ? name : ""));
    cJSON_AddStringToObject(root, "body",  (body[0] ? body : ""));
    cJSON_AddNumberToObject(root, "cat",   (int)cat);

    // Serialize
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(body);

    if (txt == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Respond
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

// Trim leading/trailing ASCII blanks/control chars in-place
static void trim_ascii(char *s)
{
    // Make sure valid
    if (!s) {
        return;
    }

    char *p = s;

    // Go to space
    while (*p && (unsigned char)*p <= ' ') {
        p++;
    }

    size_t len = strlen(p);

    // Trim
    while (len > 0 && (unsigned char)p[len - 1] <= ' ') {
        p[--len] = '\0'; // NUL-terminate
    }

    if (p != s) {
        memmove(s, p, len + 1);
    }
}

// Derive label from first nonblank line of body
static void label_from_body(const char *body_in, char *out, size_t outlen)
{
    // Make sure valid
    if (!body_in || !out || outlen == 0) {
        return;
    }

    size_t i = 0, n = 0;

    // Skip blanks
    while (body_in[i] && (unsigned char)body_in[i] <= ' ') {
        i++;
    }

    while (body_in[i] && body_in[i] != '\n' && body_in[i] != '\r' && n + 1 < outlen) {
        out[n++] = body_in[i++];
    }
    out[n] = '\0'; // NUL-terminate

    trim_ascii(out);
}

// POST /api/script
// Body: {"index":N,"name":"...","body":"...","cat":C}
// If "cat" is provided, 'index' is LOCAL within that category.
// We resolve to a GLOBAL index (existing slot or append at end).
static esp_err_t script_one_post(httpd_req_t *req)
{
    // Reject overly large bodies
    if (req->content_len > (MAX_HTTP_BODY_TXT + 256)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
    }

    // Allocate buffer
    char *buf = (char *)malloc((size_t)req->content_len + 1);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Read full body
    size_t off = 0;
    while (off < (size_t)req->content_len) {
        int n = httpd_req_recv(req, buf + off, req->content_len - (int)off);

        // If recv failed, bail
        if (n <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }

        off += (size_t)n;
    }
    buf[off] = '\0';

    // Parse JSON
    cJSON *j = cJSON_Parse(buf);
    if (j == NULL) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    // Extract fields
    cJSON *jidx = cJSON_GetObjectItemCaseSensitive(j, "index");
    cJSON *jname = cJSON_GetObjectItemCaseSensitive(j, "name");
    cJSON *jbody = cJSON_GetObjectItemCaseSensitive(j, "body");
    cJSON *jcat = cJSON_GetObjectItemCaseSensitive(j, "cat"); // Optional

    // Validate required fields
    if (!cJSON_IsNumber(jidx) || !cJSON_IsString(jname) || !cJSON_IsString(jbody)) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing index/name/body");
    }

    // Validate index range
    // Valid indices are 0..BT_MAX_KEYBOARD_SCRIPTS-1; appending at the last index bumps count to BT_MAX_KEYBOARD_SCRIPTS (the ceiling)
    if (jidx->valueint < 0 || jidx->valueint >= BT_MAX_KEYBOARD_SCRIPTS) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "index out of range");
    }

    // Pull values
    uint16_t idx_local_or_global = (uint16_t)jidx->valueint;
    const char *name_in = jname->valuestring;
    const char *body_in = jbody->valuestring;

    // Cap body so the stored blob (strlen+1) fits the runtime send buffer (AI_RESPONSE_MAX_LEN);
    // a boundary-sized body would otherwise save but never execute
    if (strlen(body_in) >= MAX_HTTP_BODY_TXT) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    }

    // Parse/Default category
    uint8_t cat = 0;
    bool has_cat = false;
    if (cJSON_IsNumber(jcat)) {
        if (jcat->valueint < 0 || jcat->valueint >= BT_MAX_CATEGORIES) {
            cJSON_Delete(j);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cat out of range");
        }
        cat = (uint8_t)jcat->valueint;
        has_cat = true;
    }

    // Resolve to GLOBAL index
    uint16_t global_idx = idx_local_or_global;
    if (has_cat) {
        // Resolve local -> global (allow create if local==tail)
        if (!resolve_global_index_for_local(cat, idx_local_or_global, true, &global_idx)) {
            cJSON_Delete(j);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index/cat");
        }
    } else {
        // Treat as global (edit or append at tail)
        uint16_t total = bluetooth_portal_script_count_get_nvs();

        // If index past tail, reject
        if (global_idx > total) {
            cJSON_Delete(j);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
        }
    }

    // Build label from name/body
    char label[BT_SCRIPT_LABEL_MAX_LEN + 1];
    if (name_in[0] != '\0') {
        strncpy(label, name_in, BT_SCRIPT_LABEL_MAX_LEN);
        label[BT_SCRIPT_LABEL_MAX_LEN] = '\0';
        trim_ascii(label);
    } else {
        label_from_body(body_in, label, sizeof(label));
        if (label[0] == '\0') {
            snprintf(label, sizeof(label), "Script %02d", (int)global_idx);
        }
    }

    // Persist the slot BEFORE bumping the count so a failed write (e.g. NVS full) never leaves a counted-but-empty slot
    // On append this writes index == count, currently beyond the count and therefore invisible; the count bump below reveals it only after every write succeeds

    // Persist label
    esp_err_t err = bluetooth_script_label_set_nvs(global_idx, label);

    // Persist body
    if (err == ESP_OK) {
        err = bluetooth_script_body_set_nvs(global_idx, body_in);
    }

    // Persist category only if supplied (keep old if not)
    if (err == ESP_OK && has_cat) {
        err = bluetooth_portal_script_cat_idx_set_nvs(global_idx, cat);
    }

    // If any write failed, report (count not yet bumped, so no empty slot is exposed)
    if (err != ESP_OK) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    // All slot writes succeeded. If appending, reveal the new tail by bumping the count LAST
    uint16_t count = bluetooth_portal_script_count_get_nvs();
    if (global_idx >= count) {
        esp_err_t ecount = bluetooth_script_count_set_nvs((uint16_t)(global_idx + 1));

        if (ecount != ESP_OK) {
            cJSON_Delete(j);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs-count");
        }
    }

    // Respond with GLOBAL index
    char okjson[48];
    int n = snprintf(okjson, sizeof(okjson), "{\"ok\":true,\"index\":%d}", (int)global_idx);
    if ((n < 0) || (n >= (int)sizeof(okjson))) {
        strcpy(okjson, "{\"ok\":true}");
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_sendstr(req, okjson);

    // Cleanup
    cJSON_Delete(j);
    free(buf);
    return err;
}

// DELETE /api/script?index=N[&cat=C]
// If &cat=C is present, 'index' is LOCAL to that category.
// We delete the corresponding GLOBAL slot and shift tail (label/body/cat).
static esp_err_t script_one_delete(httpd_req_t *req)
{
    // Read query
    char qstr[64];
    if (httpd_req_get_url_query_str(req, qstr, sizeof(qstr)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
    }

    // Parse index
    char idx_str[8] = {0};
    if (httpd_query_key_value(qstr, "index", idx_str, sizeof(idx_str)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing index");
    }

    // Parse optional category
    char cat_str[8] = {0};
    bool has_cat = (httpd_query_key_value(qstr, "cat", cat_str, sizeof(cat_str)) == ESP_OK);

    // Resolve to GLOBAL index
    uint16_t global_idx = 0;
    if (has_cat) {
        uint16_t local_idx = (uint16_t)atoi(idx_str);
        uint8_t cat = (uint8_t)atoi(cat_str);

        // Resolve local -> global (no create on delete)
        if (!resolve_global_index_for_local(cat, local_idx, false, &global_idx)) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        }
    } else {
        global_idx = (uint16_t)atoi(idx_str);
    }

    // Validate range
    uint16_t count = bluetooth_portal_script_count_get_nvs();
    if (global_idx >= count) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    // Allocate scratch for body shift
    char *next_body = (char *)malloc(MAX_HTTP_BODY_TXT + 1);
    if (next_body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Shift down [global_idx+1 .. count-1] into [global_idx .. count-2]
    char next_label[BT_SCRIPT_LABEL_MAX_LEN + 1];
    size_t blen = 0;
    uint8_t next_cat = 0;

    for (uint16_t i = global_idx; i + 1 < count; ++i) {
        // Read from next slot
        next_label[0] = '\0';
        blen = 0;
        next_cat = 0;

        bluetooth_portal_script_label_get_nvs(i + 1, next_label, sizeof(next_label));
        bluetooth_portal_script_body_get_nvs(i + 1, next_body, MAX_HTTP_BODY_TXT + 1, &blen);
        bluetooth_portal_script_cat_idx_get_nvs(i + 1, &next_cat);

        // Write into current slot
        bluetooth_script_label_set_nvs(i, next_label);
        bluetooth_script_body_set_nvs(i, (blen > 0) ? next_body : "");
        bluetooth_portal_script_cat_idx_set_nvs(i, next_cat);
    }

    // Clear old tail
    bluetooth_script_label_set_nvs(count - 1, "");
    bluetooth_script_body_set_nvs(count - 1, "");
    bluetooth_portal_script_cat_idx_set_nvs(count - 1, 0);

    // Decrement count
    bluetooth_script_count_set_nvs(count - 1);

    // Free scratch
    free(next_body);

    // Respond
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Categories list: GET /api/categories
static esp_err_t categories_get(httpd_req_t *req)
{
    // Create root JSON
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Read count
    uint8_t count = bluetooth_portal_category_count_get_nvs();
    cJSON_AddNumberToObject(root, "count", count);

    // Names array
    cJSON *names = cJSON_AddArrayToObject(root, "names");
    if (names == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Script counts per category (for authoritative append index)
    cJSON *script_counts = cJSON_AddArrayToObject(root, "script_counts");
    if (script_counts == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Count scripts per category in a single pass over all scripts
    uint16_t total_scripts = bluetooth_portal_script_count_get_nvs();
    uint16_t per_cat[BT_MAX_CATEGORIES] = {0};

    for (uint16_t s = 0; s < total_scripts; ++s) {
        uint8_t c = 0;
        if (bluetooth_portal_script_cat_idx_get_nvs(s, &c) == ESP_OK && c < count) {
            per_cat[c]++;
        }
    }

    // Populate names and script counts
    for (uint8_t i = 0; i < count; ++i) {
        char buf[BT_CAT_LABEL_MAX_LEN + 1];

        if (bluetooth_portal_category_name_get_nvs(i, buf, sizeof(buf)) == ESP_OK) {
            cJSON_AddItemToArray(names, cJSON_CreateString(buf));
        } else {
            cJSON_AddItemToArray(names, cJSON_CreateString("(unnamed)"));
        }

        cJSON_AddItemToArray(script_counts, cJSON_CreateNumber(per_cat[i]));
    }

    // Serialize
    char *txt = cJSON_PrintUnformatted(root);
    if (txt == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);

    // Cleanup
    free(txt);
    cJSON_Delete(root);
    return ESP_OK;
}

// Single category: GET /api/category?index=N
static esp_err_t category_one_get(httpd_req_t *req)
{
    // Read query
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Parse index
    char param[32];
    int index = -1;

    if (httpd_query_key_value(query, "index", param, sizeof(param)) == ESP_OK) {
        index = atoi(param);
    }

    // Validate
    if (index < 0) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Create JSON
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddNumberToObject(root, "index", index);

    // Read name
    char buf[BT_CAT_LABEL_MAX_LEN + 1];
    if (bluetooth_portal_category_name_get_nvs((uint8_t)index, buf, sizeof(buf)) == ESP_OK) {
        cJSON_AddStringToObject(root, "name", buf);
    } else {
        cJSON_AddStringToObject(root, "name", "");
    }

    // Serialize
    char *txt = cJSON_PrintUnformatted(root);
    if (txt == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, txt);

    // Cleanup
    free(txt);
    cJSON_Delete(root);
    return ESP_OK;
}

// Add/edit category: POST /api/category {index: N, name: "Foo"}
static esp_err_t category_one_post(httpd_req_t *req)
{
    // Validate length
    int total = req->content_len;
    if (total <= 0 || total > MAX_HTTP_BODY_TXT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    // Allocate buffer
    char *buf = (char *)malloc((size_t)total + 1);
    if (buf == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Read full body
    int off = 0;
    while (off < total) {
        int n = httpd_req_recv(req, buf + off, total - off);

        // If recv failed, bail
        if (n <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }

        off += n;
    }
    buf[off] = '\0';

    // Parse JSON
    cJSON *j = cJSON_Parse(buf);
    if (j == NULL) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    // Extract fields
    cJSON *j_idx  = cJSON_GetObjectItemCaseSensitive(j, "index");
    cJSON *j_name = cJSON_GetObjectItemCaseSensitive(j, "name");

    // Validate fields
    if (!cJSON_IsNumber(j_idx) || !cJSON_IsString(j_name)) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
    }

    // Pull values
    int index = j_idx->valueint;
    const char *name = j_name->valuestring;

    // Validate values
    if (index < 0 || index >= BT_MAX_CATEGORIES || name == NULL) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fields");
    }

    // Save category
    esp_err_t err = bluetooth_portal_category_set_nvs((uint8_t)index, name);

    // Cleanup JSON/buf
    cJSON_Delete(j);
    free(buf);

    // If write failed, report
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    // Respond
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Delete category: DELETE /api/category?index=N
static esp_err_t category_one_delete(httpd_req_t *req)
{
    // Read query
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no query");
    }

    // Parse index
    char param[32];
    int index = -1;

    if (httpd_query_key_value(query, "index", param, sizeof(param)) == ESP_OK) {
        index = atoi(param);
    }

    // Validate
    if (index < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }

    // Delete category (also shifts/updates affected script cats as your impl dictates)
    esp_err_t err = bluetooth_portal_category_delete_nvs((uint8_t)index);

    // If write failed, report
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    // Respond
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ========== Backup / Restore ========== */

// GET /api/export -> downloadable JSON of every category + script
// { "fmt":"polycast5-autotype", "ver":1, "categories":[...], "scripts":[{label,cat,body},...] }
static esp_err_t export_get(httpd_req_t *req)
{
    // Nothing to export? Don't hand back an empty file.
    uint16_t script_count = bluetooth_portal_script_count_get_nvs();
    uint8_t cat_count = bluetooth_portal_category_count_get_nvs();
    if (script_count == 0 && cat_count == 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "nothing to export");
    }

    // Allocate a JSON root object
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Format tag + version so import can validate the file
    if ((cJSON_AddStringToObject(root, "fmt", BT_EXPORT_FMT_TAG) == NULL) ||
        (cJSON_AddNumberToObject(root, "ver", BT_EXPORT_VER) == NULL)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Categories array (array index == category id)
    cJSON *cats = cJSON_AddArrayToObject(root, "categories");
    if (cats == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    for (uint8_t i = 0; i < cat_count; ++i) {
        char nm[BT_CAT_LABEL_MAX_LEN + 1];
        const char *name = (bluetooth_portal_category_name_get_nvs(i, nm, sizeof(nm)) == ESP_OK) ? nm : "(unnamed)";

        // Check the allocation so a failure can't silently drop a category from the backup
        cJSON *item = cJSON_CreateString(name);
        if (item == NULL) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        cJSON_AddItemToArray(cats, item);
    }

    // Scripts array ({label, cat, body} per entry)
    cJSON *scr = cJSON_AddArrayToObject(root, "scripts");
    if (scr == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // One reusable scratch buffer for bodies (cJSON copies strings, so reuse is safe)
    char *body = (char *)malloc(MAX_HTTP_BODY_TXT + 1);
    if (body == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    for (uint16_t i = 0; i < script_count; ++i) {
        char lbl[BT_SCRIPT_LABEL_MAX_LEN + 1] = {0};
        uint8_t cat = 0;
        size_t blen = 0;

        // Label/cat are cosmetic with safe defaults (missing label -> "", missing cat -> 0),
        // so a failed read there is tolerated.
        bluetooth_portal_script_label_get_nvs(i, lbl, sizeof(lbl));
        bluetooth_portal_script_cat_idx_get_nvs(i, &cat);

        // The body is the actual payload. If it can't be fully read (NVS error, or a legacy
        // body larger than the buffer that the getter would silently skip, leaving blen == 0),
        // abort rather than hand back a backup that quietly dropped a script's data.
        body[0] = '\0';
        esp_err_t berr = bluetooth_portal_script_body_get_nvs(i, body, MAX_HTTP_BODY_TXT + 1, &blen);
        if ((berr != ESP_OK) || (blen == 0)) {
            free(body);
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
        }

        // Build the per-script object
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            free(body);
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        // Check each field so an allocation failure can't silently drop a script's data
        if ((cJSON_AddStringToObject(o, "label", lbl) == NULL) ||
            (cJSON_AddNumberToObject(o, "cat", cat) == NULL) ||
            (cJSON_AddStringToObject(o, "body", body) == NULL)) {
            cJSON_Delete(o);
            free(body);
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        cJSON_AddItemToArray(scr, o);
    }

    // Serialize then free the tree + scratch buffer
    char *txt = cJSON_PrintUnformatted(root);
    free(body);
    cJSON_Delete(root);

    if (txt == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Send as a downloadable attachment
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"polycast5-autotype.json\"");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);

    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
       ESP_LOGE(TAG, "export_get httpd_resp_sendstr failed: %s", esp_err_to_name(err));
#endif
    }

    return err;
}

// Send a JSON error for the import handler so the browser can show a specific reason.
// Every call site below fires BEFORE the NVS wipe, so existing data is left untouched.
static esp_err_t import_error(httpd_req_t *req, const char *status, const char *code)
{
    char j[64];
    snprintf(j, sizeof(j), "{\"ok\":false,\"error\":\"%s\"}", code);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, j);
}

// POST /api/import  { "fmt":"polycast5-autotype", "ver":1, "categories":[...], "scripts":[...] }
// Replace-all: validate fully BEFORE touching NVS, then wipe autotype data and restore from the file.
static esp_err_t import_post(httpd_req_t *req)
{
    // Reject empty/oversized bodies (content_len is size_t, so == 0 catches empty)
    if (req->content_len == 0 || req->content_len > MAX_IMPORT_BODY_TXT) {
        return import_error(req, "400 Bad Request", "bad_length");
    }

    // Allocate buffer
    char *buf = (char *)malloc((size_t)req->content_len + 1);
    if (buf == NULL) {
        return import_error(req, "500 Internal Server Error", "oom");
    }

    // Read full body
    size_t off = 0;
    while (off < (size_t)req->content_len) {
        int n = httpd_req_recv(req, buf + off, req->content_len - (int)off);

        // If recv failed, bail
        if (n <= 0) {
            free(buf);
            return import_error(req, "500 Internal Server Error", "recv");
        }

        off += (size_t)n;
    }
    buf[off] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        free(buf);
        return import_error(req, "400 Bad Request", "bad_json");
    }

    // Validate format tag + version BEFORE any NVS change (a bad file must not wipe data)
    cJSON *jfmt = cJSON_GetObjectItemCaseSensitive(root, "fmt");
    cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "ver");

    if (!cJSON_IsString(jfmt) || (jfmt->valuestring == NULL) || (strcmp(jfmt->valuestring, BT_EXPORT_FMT_TAG) != 0)) {
        cJSON_Delete(root);
        free(buf);
        return import_error(req, "400 Bad Request", "bad_format");
    }
    if (!cJSON_IsNumber(jver) || (jver->valueint != BT_EXPORT_VER)) {
        cJSON_Delete(root);
        free(buf);
        return import_error(req, "400 Bad Request", "bad_version");
    }

    // Both arrays are required
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(root, "categories");
    cJSON *scr = cJSON_GetObjectItemCaseSensitive(root, "scripts");
    if (!cJSON_IsArray(cats) || !cJSON_IsArray(scr)) {
        cJSON_Delete(root);
        free(buf);
        return import_error(req, "400 Bad Request", "bad_structure");
    }

    // Reject over-limit files BEFORE any NVS change (don't silently drop entries and
    // then report success). Valid range: up to BT_MAX_CATEGORIES categories and
    // BT_MAX_KEYBOARD_SCRIPTS scripts (indices 0..BT_MAX_KEYBOARD_SCRIPTS-1)
    int cat_n = cJSON_GetArraySize(cats);
    int script_n = cJSON_GetArraySize(scr);
    if (cat_n > BT_MAX_CATEGORIES || script_n > BT_MAX_KEYBOARD_SCRIPTS) {
        cJSON_Delete(root);
        free(buf);
        return import_error(req, "400 Bad Request", "too_many");
    }

    // Validate every entry BEFORE wiping, so a damaged-but-parseable file can't replace good
    // data with empty/placeholder records. Each category must be a non-null string; each script
    // must be an object with a string body. cat/label stay optional (safe defaults on restore).
    for (int i = 0; i < cat_n; ++i) {
        cJSON *c = cJSON_GetArrayItem(cats, i);
        if (!cJSON_IsString(c) || (c->valuestring == NULL)) {
            cJSON_Delete(root);
            free(buf);
            return import_error(req, "400 Bad Request", "bad_entry");
        }
    }
    for (int i = 0; i < script_n; ++i) {
        cJSON *s = cJSON_GetArrayItem(scr, i);
        if (!cJSON_IsObject(s)) {
            cJSON_Delete(root);
            free(buf);
            return import_error(req, "400 Bad Request", "bad_entry");
        }

        cJSON *jb = cJSON_GetObjectItemCaseSensitive(s, "body");
        if (!cJSON_IsString(jb) || (jb->valuestring == NULL)) {
            cJSON_Delete(root);
            free(buf);
            return import_error(req, "400 Bad Request", "bad_entry");
        }

        // Cap at MAX_HTTP_BODY_TXT-1 so the stored blob (strlen+1) still fits the runtime send
        // buffer (send_buf[AI_RESPONSE_MAX_LEN]) and every 64KB reader; a boundary-sized body
        // would otherwise import but never execute.
        if (strlen(jb->valuestring) >= MAX_HTTP_BODY_TXT) {
            cJSON_Delete(root);
            free(buf);
            return import_error(req, "400 Bad Request", "script_too_large");
        }
    }

    // Validation passed. Wipe keyb_menu (script count + labels) FIRST and check the result: if
    // that erase fails, nothing has been destroyed yet, so abort with the old data fully intact
    // instead of later publishing count 0 over still-present, now-hidden scripts.
    nvs_handle_t h;
    esp_err_t menu_wipe = ESP_FAIL;
    if (nvs_open(BT_SCRIPT_MENU_NS, NVS_READWRITE, &h) == ESP_OK) {
        menu_wipe = nvs_erase_all(h);
        if (menu_wipe == ESP_OK) {
            menu_wipe = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (menu_wipe != ESP_OK) {
        cJSON_Delete(root);
        free(buf);
        return import_error(req, "500 Internal Server Error", "wipe_failed");
    }

    // keyb_menu is now empty, so the old scripts are already gone. The bt_portal wipe is
    // best-effort: correctness comes from overwriting every record and setting both counts
    // explicitly, so we continue even if it fails (aborting now would strand an emptied index).
    if (nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    // Write categories ascending 0..cat_n-1 (set_nvs bumps cat_count only on append)
    int cats_written = 0;
    for (int i = 0; i < cat_n; ++i) {
        cJSON *c = cJSON_GetArrayItem(cats, i);
        char nm[BT_CAT_LABEL_MAX_LEN + 1];

        if (cJSON_IsString(c) && (c->valuestring != NULL)) {
            strncpy(nm, c->valuestring, BT_CAT_LABEL_MAX_LEN);
            nm[BT_CAT_LABEL_MAX_LEN] = '\0';
        } else {
            strcpy(nm, "(unnamed)");
        }

        // Stop on first failure so cat_count stays contiguous with the names written
        if (bluetooth_portal_category_set_nvs((uint8_t)i, nm) != ESP_OK) {
            break;
        }
        cats_written++;
    }

    // Force cat_count to exactly the categories written. category_set_nvs only grows the count
    // on append, so if the bt_portal wipe failed a stale-high count could otherwise survive and
    // expose orphaned category names. Setting it explicitly (like the script count) makes
    // correctness independent of the best-effort wipe.
    esp_err_t cat_count_err = ESP_FAIL;
    if (nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h) == ESP_OK) {
        cat_count_err = nvs_set_u8(h, BT_SCRIPT_CAT_COUNT, (uint8_t)cats_written);
        if (cat_count_err == ESP_OK) {
            cat_count_err = nvs_commit(h);
        }
        nvs_close(h);
    }

    // Write scripts (label + body + cat per slot); count is written LAST
    int scripts_written = 0;
    for (int i = 0; i < script_n; ++i) {
        cJSON *s = cJSON_GetArrayItem(scr, i);

        // Label (truncate to 32; default if missing)
        char label[BT_SCRIPT_LABEL_MAX_LEN + 1];
        cJSON *jl = cJSON_IsObject(s) ? cJSON_GetObjectItemCaseSensitive(s, "label") : NULL;
        if (cJSON_IsString(jl) && (jl->valuestring != NULL)) {
            strncpy(label, jl->valuestring, BT_SCRIPT_LABEL_MAX_LEN);
            label[BT_SCRIPT_LABEL_MAX_LEN] = '\0';
        } else {
            snprintf(label, sizeof(label), "Script %02d", i);
        }

        // Category (clamp out-of-range to 0)
        cJSON *jc = cJSON_IsObject(s) ? cJSON_GetObjectItemCaseSensitive(s, "cat") : NULL;
        uint8_t cat = 0;
        if (cJSON_IsNumber(jc) && (jc->valueint >= 0) && (jc->valueint < cat_n)) {
            cat = (uint8_t)jc->valueint;
        }

        // Body (default empty if missing)
        cJSON *jb = cJSON_IsObject(s) ? cJSON_GetObjectItemCaseSensitive(s, "body") : NULL;
        const char *body_in = (cJSON_IsString(jb) && (jb->valuestring != NULL)) ? jb->valuestring : "";

        // Persist the slot; stop on first failure so count never exposes a half-written slot
        esp_err_t err = bluetooth_script_label_set_nvs((uint16_t)i, label);
        if (err == ESP_OK) {
            err = bluetooth_script_body_set_nvs((uint16_t)i, body_in);
        }
        if (err == ESP_OK) {
            err = bluetooth_portal_script_cat_idx_set_nvs((uint16_t)i, cat);
        }
        if (err != ESP_OK) {
            break;
        }

        scripts_written++;
    }

    // Count last: only fully-written slots become visible
    esp_err_t count_err = bluetooth_script_count_set_nvs((uint16_t)scripts_written);

    cJSON_Delete(root);
    free(buf);

    // Success: every category and script was written and both counts committed
    char okjson[80];
    if ((count_err == ESP_OK) && (cat_count_err == ESP_OK) && (scripts_written == script_n) && (cats_written == cat_n)) {
        snprintf(okjson, sizeof(okjson), "{\"ok\":true,\"categories\":%d,\"scripts\":%d}", cats_written, scripts_written);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, okjson);
    }

    // Partial restore after wipe: report failure but keep the counts so the UI can show progress
    snprintf(okjson, sizeof(okjson), "{\"ok\":false,\"categories\":%d,\"scripts\":%d,\"error\":\"nvs\"}", cats_written, scripts_written);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, okjson);
}

/* ========== HTTP server bootstrap ========== */

// Start the embedded HTTP server and register endpoints
static httpd_handle_t start_http(void)
{
    // C0nfigure default
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;

    // Start HTTP
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        return NULL;
    }

    // UI
    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get};
    httpd_register_uri_handler(srv, &root);

    // Scripts API
    httpd_uri_t list = {.uri="/api/scripts", .method=HTTP_GET, .handler=scripts_list_get};
    httpd_register_uri_handler(srv, &list);

    httpd_uri_t g1 = {.uri="/api/script", .method=HTTP_GET, .handler=script_one_get};
    httpd_register_uri_handler(srv, &g1);

    httpd_uri_t p1 = {.uri="/api/script", .method=HTTP_POST, .handler=script_one_post};
    httpd_register_uri_handler(srv, &p1);

    httpd_uri_t d1 = {.uri="/api/script", .method=HTTP_DELETE, .handler=script_one_delete};
    httpd_register_uri_handler(srv, &d1);

    // Categories API
    httpd_uri_t cg  = {.uri="/api/categories", .method=HTTP_GET,    .handler=categories_get};
    httpd_register_uri_handler(srv, &cg);

    httpd_uri_t c1g = {.uri="/api/category",   .method=HTTP_GET,    .handler=category_one_get};
    httpd_register_uri_handler(srv, &c1g);

    httpd_uri_t c1p = {.uri="/api/category",   .method=HTTP_POST,   .handler=category_one_post};
    httpd_register_uri_handler(srv, &c1p);

    httpd_uri_t c1d = {.uri="/api/category",   .method=HTTP_DELETE, .handler=category_one_delete};
    httpd_register_uri_handler(srv, &c1d);

    // Backup / Restore API
    httpd_uri_t ex = {.uri="/api/export", .method=HTTP_GET,  .handler=export_get};
    httpd_register_uri_handler(srv, &ex);

    httpd_uri_t im = {.uri="/api/import", .method=HTTP_POST, .handler=import_post};
    httpd_register_uri_handler(srv, &im);

    return srv;
}

/* ========== Portal management ========== */

// Start the SoftAP and the web portal
esp_err_t bluetooth_portal_start(void)
{
    // If already running, do nothing
    if (bt_server != NULL) {
#ifdef POLYCAST5_DEBUG
         ESP_LOGW(TAG, "Portal already running at http://%s", s_ip);
#endif
         
         return ESP_OK;
    }

    // Create default AP netif if needed
    if (bt_ap_netif == NULL) {
        bt_ap_netif = esp_netif_create_default_wifi_ap();
        if (bt_ap_netif == NULL) {
            return ESP_FAIL;
        }
    }

    // Init Wi-Fi (tolerate "already init" state)
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE) && (err != ESP_ERR_INVALID_STATE)) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Keep config in RAM so nothing persists accidentally.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Configure SoftAP using project macros for SSID/PASS
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, BT_PORTAL_SSID);
    ap.ap.ssid_len = strlen(BT_PORTAL_SSID);
    strcpy((char *)ap.ap.password, bt_wifi_portal_pass);
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    // Start Wi-Fi
    err = esp_wifi_start();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Cache AP IP (usually 192.168.4.1)
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(bt_ap_netif, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    }

    // Bring up HTTP server and register endpoints.
    bt_server = start_http();
    if (bt_server == NULL) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "start_http failed");
#endif
         
        return ESP_FAIL;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Portal running at http://%s (SSID: " BT_PORTAL_SSID ")", s_ip);
#endif
    
    return ESP_OK;
}

// Stop the web server and SoftAP
esp_err_t bluetooth_portal_stop(void)
{
    if (bt_server != NULL) {
        httpd_stop(bt_server);
        bt_server = NULL;
    }
    
    esp_err_t err = esp_wifi_stop();

    // Fully detach Wi-Fi from any interface
    err = esp_wifi_set_mode(WIFI_MODE_NULL);
    
    if (bt_ap_netif) {
        esp_netif_destroy_default_wifi(bt_ap_netif); // Destroys handlers and netif
        bt_ap_netif = NULL;
    }

    // Everything else needs station mode
    err = esp_wifi_set_mode(WIFI_MODE_STA);

    return err;
}

// Return the AP IP string for on-screen instructions
const char *bluetooth_portal_get_ip(void)
{
    return s_ip;
}

const char *bluetooth_portal_get_ssid(void)
{
    return BT_PORTAL_SSID;
}

const char *bluetooth_portal_get_pass(void)
{
    return bt_wifi_portal_pass;
}