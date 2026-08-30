#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "nvs.h"
#include "cJSON.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif_ip_addr.h" // For IPSTR/IP2STR

#include "ai_key_portal_html.h" // AI_PORTAL_HTML
#include "ai_key_portal.h"
#include "ai_utils.h"
#include "ai_provider.h"
#include "ai_prompts.h"

#define TAG "AI_PORTAL"

// Keep the Wi-Fi AP password NVS as-is (this module still needs portal creds)
#define AI_PASS_NS "ai_wifi"
#define AI_PASS_KEY "pass"

#define MAX_KEY_BODY 1024

// The one-shot provider body can carry two keys + a custom URL + two models; sized past the
// sum of the field maxima (2x255 keys + 255 URL + 2x95 models + JSON structure ~= 1075)
#define MAX_PROVIDER_BODY 2048

extern char ai_wifi_portal_pass[];

POLYCAST5_USE_PSRAM_BSS char prompt_buf[AI_PROMPT_NVS_MAX_LEN + 1];

// Default AP info (replace with your existing portal creds when desired)
static httpd_handle_t ai_server = NULL;
static esp_netif_t *ai_ap_netif = NULL;

static char ai_portal_ssid[32] = "PolyCast5-AI-Portal"; // AP SSID
static char ai_portal_ip[16] = "192.168.4.1"; // AP IP cached

/* =============== HTTP handlers =============== */

static esp_err_t root_get(httpd_req_t *req)
{
    // Serve HTML
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, AI_PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static void trim_ascii(char *s)
{
    // Trim in-place
    if (!s) {
        return;
    }

    // Skip leading
    char *p = s;
    while (*p && (unsigned char)*p <= ' ') {
        p++;
    }

    // Find trailing
    size_t n = strlen(p);
    while (n && (unsigned char)p[n - 1] <= ' ') {
        p[--n] = '\0'; // Chop tail
    }

    // Move to front
    if (p != s) {
        memmove(s, p, n + 1); // Include NUL
    }
}

// Provider-agnostic API key sanity: plausible length, no embedded whitespace.
// No vendor prefix check - any provider's format (xai-, sk-, gsk_, sk-or-, ...) is accepted.
static bool key_is_sane(const char *key)
{
    if (!key) {
        return false;
    }

    const size_t L = strlen(key);
    if (L < 16 || L >= AI_API_KEY_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < L; ++i) {
        if ((unsigned char)key[i] <= ' ') {
            return false;
        }
    }

    return true;
}

// Copy + trim a JSON string field into a fixed buffer. Returns false when the value is
// present but longer than the buffer holds, so callers reject the request with a clear
// error instead of silently truncating and saving a mangled value. An absent field keeps
// the caller's default and returns true.
static bool copy_json_str(const cJSON *j, const char *name, char *dst, size_t dst_sz)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!cJSON_IsString(v) || !v->valuestring) {
        return true;
    }

    if (strlen(v->valuestring) >= dst_sz) {
        return false;
    }

    strncpy(dst, v->valuestring, dst_sz - 1);
    trim_ascii(dst);
    return true;
}

// Parse an optional provider-index field. Absent => *out unchanged (caller's default).
// Present + valid in-range integer => stored. Present but malformed/out-of-range => false,
// so the caller rejects with 400 instead of silently coercing to a default and overwriting
// a good stored selection.
static bool parse_provider_idx(const cJSON *j, const char *name, uint8_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    if (v == NULL) {
        return true; // Absent: keep the caller's default
    }
    if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint >= AI_PROVIDER_COUNT) {
        return false; // Present but not a valid provider index
    }
    *out = (uint8_t)v->valueint;
    return true;
}

// cJSON 1.7.x parses recursively and allows up to CONFIG_CJSON_NESTING_LIMIT (1000) levels,
// deep enough to overflow the httpd task stack before cJSON's own limit trips. Every portal
// body is a flat object of scalars, so a linear pre-scan rejecting anything beyond a small
// nesting depth keeps a crafted "[[[[..." request from crashing the device. Skips brackets
// inside JSON strings (and their escapes) so legitimate payloads are never miscounted.
#define MAX_JSON_DEPTH 8
static bool json_depth_ok(const char *s)
{
    int depth = 0;
    bool in_str = false;

    for (; *s; ++s) {
        if (in_str) {
            if (*s == '\\' && s[1] != '\0') {
                ++s; // Skip the escaped character
            } else if (*s == '"') {
                in_str = false;
            }
            continue;
        }

        if (*s == '"') {
            in_str = true;
        } else if (*s == '[' || *s == '{') {
            if (++depth > MAX_JSON_DEPTH) {
                return false;
            }
        } else if (*s == ']' || *s == '}') {
            if (depth > 0) {
                --depth;
            }
        }
    }

    return true;
}

static void make_key_hint(const char *key, char *out, size_t out_sz)
{
    // Mask all but last 4 chars: "xai-...ABCD"
    if (!out || out_sz == 0) {
        return;
    }

    out[0] = '\0';

    if (!key || !key[0]) {
        return;
    }

    const size_t n = strlen(key);
    const char *tail = (n > 4) ? (key + (n - 4)) : key;

    // Try to preserve prefix if it looks like an xAI key
    if (strncmp(key, "xai-", 4) == 0) {
        snprintf(out, out_sz, "xai-...%s", tail);
    } else {
        snprintf(out, out_sz, "...%s", tail);
    }
}

static esp_err_t key_get(httpd_req_t *req)
{
    // Do not return the full key. Only return a boolean + masked hint.
    POLYCAST5_USE_PSRAM_BSS static char key[AI_API_KEY_MAX_LEN];
    memset(key, 0, sizeof(key)); // Static: clear stale bytes from a prior request
    esp_err_t e = ai_utils_load_api_key_nvs(key, sizeof(key));

    bool has_key = (e == ESP_OK && key[0] != '\0');

    char hint[32] = {0};
    if (has_key) {
        make_key_hint(key, hint, sizeof(hint));
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddBoolToObject(root, "has_key", has_key);
    if (has_key) {
        cJSON_AddStringToObject(root, "key_hint", hint);
    }

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, txt);
    free(txt);
    return ret;
}

static esp_err_t key_post(httpd_req_t *req)
{
    // Bound content length
    if (req->content_len <= 0 || req->content_len > MAX_KEY_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    // Read body
    char *buf = (char*)calloc(1, (size_t)req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    size_t r = 0;
    while (r < (size_t)req->content_len) {
        int g = httpd_req_recv(req, buf + r, req->content_len - (int)r);
        if (g <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        r += (size_t)g;
    }

    // Reject pathologically nested JSON before the recursive parser overflows the stack
    if (!json_depth_ok(buf)) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request JSON is too deeply nested");
    }

    // Parse JSON
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    // Accept either "api_key" or legacy "key"
    const cJSON *jk = cJSON_GetObjectItemCaseSensitive(j, "api_key");
    if (!cJSON_IsString(jk)) {
        jk = cJSON_GetObjectItemCaseSensitive(j, "key");
    }
    if (!cJSON_IsString(jk) || !jk->valuestring) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad api_key");
    }

    // Copy and trim (over-long values are rejected, never silently truncated)
    POLYCAST5_USE_PSRAM_BSS static char key[AI_API_KEY_MAX_LEN];
    memset(key, 0, sizeof(key)); // Static: clear stale bytes; also guarantees NUL after a 255-char strncpy
    if (strlen(jk->valuestring) >= sizeof(key)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "API key is too long (max 255 chars)");
    }
    strncpy(key, jk->valuestring, sizeof(key) - 1);
    trim_ascii(key);

    // Minimal sanity checks (an empty string here erases the saved key)
    if (key[0] != '\0' && !key_is_sane(key)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid api_key");
    }

    // Save to NVS (canonical storage in ai_funcs.c)
    esp_err_t e = ai_utils_save_api_key_nvs(key);

    cJSON_Delete(j);

    if (e != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - the key was not saved");
    }

    // Bind a non-empty key to the currently selected chat provider (empty = erase, no stamp needed)
    if (key[0] != '\0') {
        ai_provider_cfg_t cur;
        ai_provider_load_config_nvs(&cur);
        if (ai_provider_save_key_provider_nvs(cur.chat_prov) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - the key was saved but not bound to its provider; re-enter it");
        }
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t provider_get(httpd_req_t *req)
{
    // Return the current provider selection + overrides (never the full STT key)
    ai_provider_cfg_t cfg;
    ai_provider_load_config_nvs(&cfg);

    POLYCAST5_USE_PSRAM_BSS static char stt_key[AI_API_KEY_MAX_LEN];
    memset(stt_key, 0, sizeof(stt_key)); // Static: clear stale bytes from a prior request
    bool has_stt_key = (ai_provider_load_stt_key_nvs(stt_key, sizeof(stt_key)) == ESP_OK && stt_key[0] != '\0');
    char stt_hint[32] = {0};
    if (has_stt_key) {
        make_key_hint(stt_key, stt_hint, sizeof(stt_hint));
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddNumberToObject(root, "chat_prov", cfg.chat_prov);
    cJSON_AddStringToObject(root, "chat_model", cfg.chat_model);
    cJSON_AddStringToObject(root, "cust_url", cfg.cust_url);
    cJSON_AddNumberToObject(root, "stt_sep", cfg.stt_sep);
    cJSON_AddNumberToObject(root, "stt_prov", cfg.stt_prov);
    cJSON_AddStringToObject(root, "stt_model", cfg.stt_model);
    cJSON_AddBoolToObject(root, "has_stt_key", has_stt_key);
    if (has_stt_key) {
        cJSON_AddStringToObject(root, "stt_key_hint", stt_hint);
    }

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, txt);
    free(txt);
    return ret;
}

static esp_err_t provider_post(httpd_req_t *req)
{
    // Bound content length (sized so every individually-valid field combination fits)
    if (req->content_len <= 0 || req->content_len > MAX_PROVIDER_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body too large");
    }

    // Read body
    char *buf = (char*)calloc(1, (size_t)req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    size_t r = 0;
    while (r < (size_t)req->content_len) {
        int g = httpd_req_recv(req, buf + r, req->content_len - (int)r);
        if (g <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        r += (size_t)g;
    }

    // Reject pathologically nested JSON before the recursive parser overflows the stack
    if (!json_depth_ok(buf)) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request JSON is too deeply nested");
    }

    // Parse JSON
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    ai_provider_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    // A clear-only request ({"clear_key":1} / {"clear_stt_key":1}) carries no settings
    // fields; it must erase the key WITHOUT re-validating or rewriting the stored config.
    // (Case-sensitive lookup on purpose: a case-insensitive match would let a crafted
    // "CHAT_PROV" body be treated as a config save and overwrite the stored config.)
    const bool has_cfg = (cJSON_GetObjectItemCaseSensitive(j, "chat_prov") != NULL);

    // Numeric selections: reject a present-but-malformed value rather than coercing it to a
    // default and silently overwriting the stored config (defaults were memset above)
    const cJSON *v;
    if (!parse_provider_idx(j, "chat_prov", &cfg.chat_prov)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid chat provider");
    }
    if (!parse_provider_idx(j, "stt_prov", &cfg.stt_prov)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid STT provider");
    }
    v = cJSON_GetObjectItemCaseSensitive(j, "stt_sep");
    if (v != NULL && !cJSON_IsNumber(v) && !cJSON_IsBool(v)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid stt_sep");
    }
    cfg.stt_sep = ((cJSON_IsNumber(v) && v->valueint != 0) || cJSON_IsTrue(v)) ? 1 : 0;

    // String fields: copy + trim; over-long values are rejected, never silently truncated
    if (!copy_json_str(j, "model", cfg.chat_model, sizeof(cfg.chat_model))) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "model name is too long (max 95 chars)");
    }
    if (!copy_json_str(j, "custom_url", cfg.cust_url, sizeof(cfg.cust_url))) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "custom URL is too long (max 255 chars)");
    }
    if (!copy_json_str(j, "stt_model", cfg.stt_model, sizeof(cfg.stt_model))) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STT model name is too long (max 95 chars)");
    }

    // Optional keys: staged here, written only after every check below passes
    POLYCAST5_USE_PSRAM_BSS static char api_key[AI_API_KEY_MAX_LEN];
    POLYCAST5_USE_PSRAM_BSS static char stt_key[AI_API_KEY_MAX_LEN];
    memset(api_key, 0, sizeof(api_key)); // Static buffers persist across requests - clear stale bytes
    memset(stt_key, 0, sizeof(stt_key));

    if (!copy_json_str(j, "api_key", api_key, sizeof(api_key))) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "API key is too long (max 255 chars)");
    }
    if (!copy_json_str(j, "stt_key", stt_key, sizeof(stt_key))) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "STT key is too long (max 255 chars)");
    }

    // Explicit erase requests (an empty key field means "keep the saved one", not "clear it")
    v = cJSON_GetObjectItemCaseSensitive(j, "clear_key");
    const bool clear_key = (cJSON_IsNumber(v) && v->valueint != 0);
    v = cJSON_GetObjectItemCaseSensitive(j, "clear_stt_key");
    const bool clear_stt_key = (cJSON_IsNumber(v) && v->valueint != 0);

    /* ---- Validate everything BEFORE touching NVS, so a rejected request never leaves a
       half-applied config (e.g. a new key saved against the old provider) ---- */

    if (has_cfg) {
        // Custom provider requires a plausible http(s) endpoint URL + an explicit model
        if (cfg.chat_prov == AI_PROVIDER_CUSTOM) {
            if (strncmp(cfg.cust_url, "http://", 7) != 0 && strncmp(cfg.cust_url, "https://", 8) != 0) {
                cJSON_Delete(j);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "custom URL must start with http:// or https://");
            }
            if (cfg.chat_model[0] == '\0') {
                cJSON_Delete(j);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "model required for custom provider");
            }
        }

        // A kept (blank-field) key must have been saved for the newly selected provider:
        // the request layer refuses a mismatched key, so accepting the save here would
        // strand the user in a config-page loop after this handler reported success
        if (api_key[0] == '\0' && !clear_key && ai_provider_get(cfg.chat_prov)->key_required) {
            POLYCAST5_USE_PSRAM_BSS static char saved_chat[AI_API_KEY_MAX_LEN];
            memset(saved_chat, 0, sizeof(saved_chat)); // Static: clear stale bytes from a prior request
            bool have = (ai_utils_load_api_key_nvs(saved_chat, sizeof(saved_chat)) == ESP_OK && saved_chat[0] != '\0');
            if (have && ai_provider_load_key_provider_nvs() != cfg.chat_prov) {
                cJSON_Delete(j);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the saved API key belongs to a different provider - re-enter the key for the selected provider");
            }
        }

        // The separate STT provider must actually offer STT
        if (cfg.stt_sep && !ai_provider_get(cfg.stt_prov)->has_stt) {
            cJSON_Delete(j);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the selected STT provider has no speech-to-text");
        }

        // A separate STT provider that differs from the chat provider needs its own key,
        // supplied now or already saved (same-provider STT falls back to the chat key)
        if (cfg.stt_sep && cfg.stt_prov != cfg.chat_prov && stt_key[0] == '\0') {
            // A key being erased in this same request does not count as saved, and neither
            // does one stamped for a different provider (the device would refuse it)
            bool have_saved = false;
            bool stale_saved = false;
            if (!clear_stt_key) {
                POLYCAST5_USE_PSRAM_BSS static char saved_stt[AI_API_KEY_MAX_LEN];
                memset(saved_stt, 0, sizeof(saved_stt)); // Static: clear stale bytes from a prior request
                bool present = (ai_provider_load_stt_key_nvs(saved_stt, sizeof(saved_stt)) == ESP_OK && saved_stt[0] != '\0');
                have_saved = present && (ai_provider_load_stt_key_provider_nvs() == cfg.stt_prov);
                stale_saved = present && !have_saved;
            }
            if (!have_saved) {
                cJSON_Delete(j);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, stale_saved
                    ? "the saved STT key belongs to a different provider - re-enter the STT key for the selected STT provider"
                    : "the separate STT provider needs its own STT API key");
            }
        }
    }

    // Key sanity (only when a new value was supplied)
    if (api_key[0] != '\0' && !key_is_sane(api_key)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid api_key");
    }
    if (stt_key[0] != '\0' && !key_is_sane(stt_key)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid stt_key");
    }

    cJSON_Delete(j);

    /* ---- All checks passed: apply. Only NVS faults can fail past this point.
       Config first, keys after: if a key write then fails, the recoverable state is
       "old key + new provider" (an auth error), never a new key silently applied
       against the old provider. ---- */

    if (has_cfg) {
        if (ai_provider_save_config_nvs(&cfg) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - settings may be incomplete; check and re-save");
        }
    }

    if (clear_key) {
        // Empty string reads back as "no key" everywhere (callers test key[0] != '\0')
        if (ai_utils_save_api_key_nvs("") != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - the API key was not erased");
        }
    } else if (api_key[0] != '\0') {
        if (ai_utils_save_api_key_nvs(api_key) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - settings saved but the API key was not; re-enter it");
        }
        // Bind the key to the provider it was entered for. On a key-only request (no chat_prov
        // in the body) use the stored selection. A missing stamp defaults to xAI, which is
        // fail-safe: a mismatch simply prevents the key from being used, never mis-sent.
        uint8_t kp = cfg.chat_prov;
        if (!has_cfg) {
            ai_provider_cfg_t cur;
            ai_provider_load_config_nvs(&cur);
            kp = cur.chat_prov;
        }
        if (ai_provider_save_key_provider_nvs(kp) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - the key was saved but not bound to its provider; re-enter it");
        }
    }

    if (clear_stt_key) {
        if (ai_provider_save_stt_key_nvs("") != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - the STT key was not erased");
        }
    } else if (stt_key[0] != '\0' && cfg.stt_sep) {
        // Gated on stt_sep so a value typed and then abandoned behind the unchecked
        // portal checkbox is never persisted
        if (ai_provider_save_stt_key_nvs(stt_key) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - settings saved but the STT key was not; re-enter it");
        }
        // Bind the STT key to its provider. On a body without chat_prov the parsed cfg holds
        // defaults, not the stored selection, so fall back to the stored stt_prov
        uint8_t sp = cfg.stt_prov;
        if (!has_cfg) {
            ai_provider_cfg_t cur;
            ai_provider_load_config_nvs(&cur);
            sp = cur.stt_prov;
        }
        if (ai_provider_save_stt_key_provider_nvs(sp) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed - the STT key was saved but not bound to its provider; re-enter it");
        }
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t keyboard_prompt_get(httpd_req_t *req)
{
    // Return current prompt (NVS override if set; else compiled default)
    memset(prompt_buf, 0, sizeof(prompt_buf)); // Clear buffer
    esp_err_t e = ai_utils_keyboard_prompt_load_nvs(prompt_buf, sizeof(prompt_buf));

    if (e != ESP_OK || prompt_buf[0] == '\0') {
        strncpy(prompt_buf, AI_PROMPT_AUTOKEY, sizeof(prompt_buf) - 1);
        prompt_buf[sizeof(prompt_buf) - 1] = '\0';
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddStringToObject(root, "prompt", prompt_buf);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, txt);
    free(txt);
    return ret;
}

static esp_err_t keyboard_prompt_post(httpd_req_t *req)
{
    // Bound content length
    if (req->content_len <= 0 || req->content_len > AI_PROMPT_NVS_MAX_LEN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    // Read body
    char *buf = (char*)calloc(1, (size_t)req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    size_t r = 0;
    while (r < (size_t)req->content_len) {
        int g = httpd_req_recv(req, buf + r, req->content_len - (int)r);
        if (g <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        r += (size_t)g;
    }

    // Reject pathologically nested JSON before the recursive parser overflows the stack
    if (!json_depth_ok(buf)) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request JSON is too deeply nested");
    }

    // Parse JSON
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "prompt");
    if (!cJSON_IsString(jp) || !jp->valuestring) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad prompt");
    }

    // Limit prompt size (keep a hard ceiling)
    memset(prompt_buf, 0, sizeof(prompt_buf)); // Clear buffer
    strncpy(prompt_buf, jp->valuestring, sizeof(prompt_buf) - 1);

    // Allow empty string to mean "use default" (saved empty)
    esp_err_t e = ai_utils_keyboard_prompt_save_nvs(prompt_buf);
    cJSON_Delete(j);

    if (e != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t pkt_analysis_prompt_get(httpd_req_t *req)
{
    // Return current RAW_FRAMES prompt (NVS override if set; else compiled default)
    memset(prompt_buf, 0, sizeof(prompt_buf)); // Clear buffer
    esp_err_t e = ai_utils_pkt_analysis_prompt_load_nvs(prompt_buf, sizeof(prompt_buf));

    if (e != ESP_OK || prompt_buf[0] == '\0') {
        strncpy(prompt_buf, AI_PROMPT_PKT_ANALYSIS, sizeof(prompt_buf) - 1);
        prompt_buf[sizeof(prompt_buf) - 1] = '\0';
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    cJSON_AddStringToObject(root, "prompt", prompt_buf);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, txt);
    free(txt);
    return ret;
}

static esp_err_t pkt_analysis_prompt_post(httpd_req_t *req)
{
    // Bound content length
    if (req->content_len <= 0 || req->content_len > AI_PROMPT_NVS_MAX_LEN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    // Read body
    char *buf = (char*)calloc(1, (size_t)req->content_len + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    size_t r = 0;
    while (r < (size_t)req->content_len) {
        int g = httpd_req_recv(req, buf + r, req->content_len - (int)r);
        if (g <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
        }
        r += (size_t)g;
    }

    // Reject pathologically nested JSON before the recursive parser overflows the stack
    if (!json_depth_ok(buf)) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request JSON is too deeply nested");
    }

    // Parse JSON
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }

    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "prompt");
    if (!cJSON_IsString(jp) || !jp->valuestring) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad prompt");
    }

    // Hard ceiling buffer (same as keyboard prompt)
    memset(prompt_buf, 0, sizeof(prompt_buf)); // Clear buffer
    strncpy(prompt_buf, jp->valuestring, sizeof(prompt_buf) - 1);

    // Save (empty string is allowed => means "use default" in your semantics)
    esp_err_t e = ai_utils_pkt_analysis_prompt_save_nvs(prompt_buf);
    cJSON_Delete(j);

    if (e != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* =============== HTTPD bring-up =============== */

static httpd_handle_t ai_httpd_start(void)
{
    // Configure server
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192; // Increase if needed
    cfg.max_uri_handlers = 12; // Default is 8; we register 9 (root + key x2 + provider x2 + prompts x4)

    // Start server
    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        return NULL; // Failed to start
    }

    // Register endpoints
    httpd_uri_t u_root = {
        .uri="/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL
    };
    httpd_uri_t u_key_get  = {
        .uri="/api/key", .method = HTTP_GET, .handler = key_get, .user_ctx = NULL
    };
    httpd_uri_t u_key_post = {
        .uri="/api/key", .method = HTTP_POST, .handler = key_post, .user_ctx = NULL
    };
    httpd_uri_t u_provider_get = {
        .uri="/api/provider", .method = HTTP_GET, .handler = provider_get, .user_ctx = NULL
    };
    httpd_uri_t u_provider_post = {
        .uri="/api/provider", .method = HTTP_POST, .handler = provider_post, .user_ctx = NULL
    };
    httpd_uri_t u_keyboard_get = {
        .uri="/api/ai_keyboard_prompt", .method = HTTP_GET, .handler = keyboard_prompt_get, .user_ctx = NULL
    };
    httpd_uri_t u_keyboard_post = {
        .uri="/api/ai_keyboard_prompt", .method = HTTP_POST, .handler = keyboard_prompt_post, .user_ctx = NULL
    };
    httpd_uri_t u_pkt_analysis_get = {
        .uri="/api/pkt_analysis_prompt", .method = HTTP_GET, .handler = pkt_analysis_prompt_get, .user_ctx = NULL
    };
    httpd_uri_t u_pkt_analysis_post = {
        .uri="/api/pkt_analysis_prompt", .method = HTTP_POST, .handler = pkt_analysis_prompt_post, .user_ctx = NULL
    };

    httpd_register_uri_handler(h, &u_root);
    httpd_register_uri_handler(h, &u_key_get);
    httpd_register_uri_handler(h, &u_key_post);
    httpd_register_uri_handler(h, &u_provider_get);
    httpd_register_uri_handler(h, &u_provider_post);
    httpd_register_uri_handler(h, &u_keyboard_get);
    httpd_register_uri_handler(h, &u_keyboard_post);
    httpd_register_uri_handler(h, &u_pkt_analysis_get);
    httpd_register_uri_handler(h, &u_pkt_analysis_post);

    return h;
}

/* =============== Wi-Fi AP bring-up =============== */

esp_err_t ai_key_portal_start(void)
{
    // If server already running, nothing to do
    if (ai_server) {
        return ESP_OK;
    }

    // Init netif (ignore already initialized)
    esp_err_t err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Create default loop (ignore already created)
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Create SoftAP netif if needed
    if (!ai_ap_netif) {
        ai_ap_netif = esp_netif_create_default_wifi_ap();
        if (!ai_ap_netif) {
            return ESP_FAIL;
        }
    }

    // Init Wi-Fi (tolerate already init)
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE) && (err != ESP_ERR_INVALID_STATE)) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
#endif
        return err;
    }

    // Keep config in RAM only
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Configure SoftAP
    wifi_config_t ap = {0};
    strlcpy((char*)ap.ap.ssid, ai_portal_ssid, sizeof(ap.ap.ssid));
    strlcpy((char*)ap.ap.password, ai_wifi_portal_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;

    // Set AP mode (idempotent)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Apply config
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    // Start driver (tolerate already started)
    err = esp_wifi_start();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    // Cache AP IP (usually 192.168.4.1)
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ai_ap_netif, &ip) == ESP_OK) {
        // Write "a.b.c.d"
        snprintf(ai_portal_ip, sizeof(ai_portal_ip), IPSTR, IP2STR(&ip.ip));
    }

    // Start HTTP server
    ai_server = ai_httpd_start(); // start_httpd helper
    if (!ai_server) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ai_key_portal_stop(void)
{
    // Stop httpd if running
    if (ai_server) {
        httpd_stop(ai_server);
        ai_server = NULL; // Mark stopped
    }
    
    esp_err_t err = esp_wifi_stop();

    // Fully detach Wi-Fi from any interface
    err = esp_wifi_set_mode(WIFI_MODE_NULL);
    
    if (ai_ap_netif) {
        esp_netif_destroy_default_wifi(ai_ap_netif); // Destroys handlers and netif
        ai_ap_netif = NULL;
    }

    // Everything else needs station mode
    err = esp_wifi_set_mode(WIFI_MODE_STA);

    return err;
}

/* =============== Public variables =============== */

const char *ai_key_portal_get_ssid(void)
{
    // Return SSID
    return ai_portal_ssid;
}

const char *ai_key_portal_get_pass(void)
{
    // Return password
    return ai_wifi_portal_pass;
}

const char *ai_key_portal_get_ip(void)
{
    // Return IP
    return ai_portal_ip;
}

/* =============== NVS =============== */

esp_err_t ai_key_portal_pass_save_nvs(const char *val)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(AI_PASS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Set the version string
    err = nvs_set_str(h, AI_PASS_KEY, val);
    
    // Persist changes if success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    
    // Close and return
    nvs_close(h);
    return err;
}

esp_err_t ai_key_portal_pass_load_nvs(char *out, size_t out_sz)
{
    nvs_handle_t h;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(AI_PASS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    size_t len = out_sz; // Must include room for '\0'
    
    // Get the saved version string
    err = nvs_get_str(h, AI_PASS_KEY, out, &len);
    
    // Close and return
    nvs_close(h);
    return err;
}
