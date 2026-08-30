#include <string.h>

#include "nvs.h"
#include "esp_log.h"

#include "ai_provider.h"
#include "ai_utils.h" // AI_API_KEY_MAX_LEN

#define TAG "AI_PROVIDER"

// Per-provider registry. Rows are keyed by ai_provider_id_t.
// Every target speaks the OpenAI Chat-Completions schema (choices[].message.content /
// choices[].delta.content), so the shared parsers in ai_utils.c are provider-agnostic.
// reasoning_on/off are only consulted when send_reasoning_effort is true.
static const ai_provider_t k_providers[AI_PROVIDER_COUNT] = {
    [AI_PROVIDER_XAI] = {
        .id = "xai", .display = "xAI (Grok)",
        .chat_url = "https://api.x.ai/v1/chat/completions", .chat_model_def = "grok-4.3",
        .send_reasoning_effort = true, .reasoning_on = "medium", .reasoning_off = "none",
        .key_required = true, .has_stt = true,
        .stt_url = "https://api.x.ai/v1/stt", .stt_model_def = "grok-stt", .stt_fmt_field = "format",
        .stt_send_model = false, .stt_fmt_value = "true", // /v1/stt has no model field; format=true => Inverse Text Normalization ("$100")
    },
    [AI_PROVIDER_OPENAI] = {
        .id = "openai", .display = "OpenAI",
        .chat_url = "https://api.openai.com/v1/chat/completions", .chat_model_def = "gpt-4o",
        .send_reasoning_effort = false, .reasoning_on = "none", .reasoning_off = "none",
        .key_required = true, .has_stt = true,
        .stt_url = "https://api.openai.com/v1/audio/transcriptions", .stt_model_def = "whisper-1", .stt_fmt_field = "response_format",
        .stt_send_model = true, .stt_fmt_value = "json", // OpenAI Whisper: model + response_format=json
    },
    [AI_PROVIDER_GROQ] = {
        .id = "groq", .display = "Groq",
        .chat_url = "https://api.groq.com/openai/v1/chat/completions", .chat_model_def = "openai/gpt-oss-120b",
        .send_reasoning_effort = false, .reasoning_on = "none", .reasoning_off = "none",
        .key_required = true, .has_stt = true,
        .stt_url = "https://api.groq.com/openai/v1/audio/transcriptions", .stt_model_def = "whisper-large-v3-turbo", .stt_fmt_field = "response_format",
        .stt_send_model = true, .stt_fmt_value = "json", // Groq Whisper: model + response_format=json
    },
    [AI_PROVIDER_DEEPSEEK] = {
        .id = "deepseek", .display = "DeepSeek",
        .chat_url = "https://api.deepseek.com/v1/chat/completions", .chat_model_def = "deepseek-v4-flash",
        .send_reasoning_effort = false, .reasoning_on = "none", .reasoning_off = "none",
        .key_required = true, .has_stt = false,
        .stt_url = "", .stt_model_def = "", .stt_fmt_field = "", .stt_send_model = false, .stt_fmt_value = "",
    },
    [AI_PROVIDER_OPENROUTER] = {
        .id = "openrouter", .display = "OpenRouter",
        .chat_url = "https://openrouter.ai/api/v1/chat/completions", .chat_model_def = "anthropic/claude-sonnet-4.5",
        .send_reasoning_effort = false, .reasoning_on = "none", .reasoning_off = "none",
        .key_required = true, .has_stt = false,
        .stt_url = "", .stt_model_def = "", .stt_fmt_field = "", .stt_send_model = false, .stt_fmt_value = "",
    },
    [AI_PROVIDER_CUSTOM] = {
        .id = "custom", .display = "Custom / Local",
        .chat_url = "", .chat_model_def = "",
        .send_reasoning_effort = false, .reasoning_on = "none", .reasoning_off = "none",
        .key_required = false, .has_stt = false,
        .stt_url = "", .stt_model_def = "", .stt_fmt_field = "", .stt_send_model = false, .stt_fmt_value = "",
    },
};

size_t ai_provider_count(void)
{
    return AI_PROVIDER_COUNT;
}

const ai_provider_t *ai_provider_get(int idx)
{
    // Clamp out-of-range inputs so a bad value can never reach the request builders
    if (idx < 0 || idx >= AI_PROVIDER_COUNT) {
        idx = AI_PROVIDER_DEFAULT;
    }
    return &k_providers[idx];
}

esp_err_t ai_provider_load_config_nvs(ai_provider_cfg_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Defaults: xAI chat + STT-follows-chat (first-boot / missing-key safe)
    memset(out, 0, sizeof(*out));
    out->chat_prov = AI_PROVIDER_DEFAULT;
    out->stt_sep = 0;
    out->stt_prov = AI_PROVIDER_DEFAULT;

    nvs_handle_t h;
    // A missing namespace just leaves the defaults in place
    if (nvs_open(AI_CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;
    }

    uint8_t u;
    if (nvs_get_u8(h, AI_CFG_CHAT_PROV, &u) == ESP_OK && u < AI_PROVIDER_COUNT) {
        out->chat_prov = u;
    }
    if (nvs_get_u8(h, AI_CFG_STT_SEP, &u) == ESP_OK) {
        out->stt_sep = u ? 1 : 0;
    }
    if (nvs_get_u8(h, AI_CFG_STT_PROV, &u) == ESP_OK && u < AI_PROVIDER_COUNT) {
        out->stt_prov = u;
    }

    // Strings: buffers are sized to the max; a missing key leaves the memset "" in place
    size_t sz;
    sz = sizeof(out->chat_model);
    nvs_get_str(h, AI_CFG_CHAT_MODEL, out->chat_model, &sz);
    sz = sizeof(out->cust_url);
    nvs_get_str(h, AI_CFG_CUST_URL, out->cust_url, &sz);
    sz = sizeof(out->stt_model);
    nvs_get_str(h, AI_CFG_STT_MODEL, out->stt_model, &sz);

    nvs_close(h);
    return ESP_OK;
}

esp_err_t ai_provider_save_config_nvs(const ai_provider_cfg_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_provider_save_config_nvs: NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Clamp provider indices so a bad value can never be stored
    uint8_t chat_prov = (in->chat_prov < AI_PROVIDER_COUNT) ? in->chat_prov : (uint8_t)AI_PROVIDER_DEFAULT;
    uint8_t stt_prov  = (in->stt_prov  < AI_PROVIDER_COUNT) ? in->stt_prov  : (uint8_t)AI_PROVIDER_DEFAULT;

    // Store model overrides only when they differ from the provider's preset default, so a
    // future firmware update can still migrate every device off a retired default model
    const char *chat_model = in->chat_model;
    if (strcmp(chat_model, ai_provider_get(chat_prov)->chat_model_def) == 0) {
        chat_model = "";
    }
    const char *stt_model = in->stt_model;
    if (strcmp(stt_model, ai_provider_get(stt_prov)->stt_model_def) == 0) {
        stt_model = "";
    }

    // Stop at the first failed write and skip the commit, so success is never RETURNED for a
    // partial config. NVS has no rollback, so earlier fields in this sequence may already be
    // on flash when a later write faults (e.g. NVS full) - the caller surfaces the error and
    // the user re-saves; the load path re-clamps and defaults any field that didn't land.
    err = nvs_set_u8(h, AI_CFG_CHAT_PROV, chat_prov);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, AI_CFG_STT_SEP, in->stt_sep ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, AI_CFG_STT_PROV, stt_prov);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, AI_CFG_CHAT_MODEL, chat_model);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, AI_CFG_CUST_URL, in->cust_url);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, AI_CFG_STT_MODEL, stt_model);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ai_provider_save_config_nvs: NVS write failed: %s", esp_err_to_name(err));
    }

    nvs_close(h);
    return err;
}

esp_err_t ai_provider_save_stt_key_nvs(const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, AI_CFG_STT_KEY, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

esp_err_t ai_provider_load_stt_key_nvs(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_CFG_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t sz = out_sz;
    err = nvs_get_str(h, AI_CFG_STT_KEY, out, &sz);

    nvs_close(h);
    return err;
}

esp_err_t ai_provider_resolve_chat(ai_chat_cfg_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    ai_provider_cfg_t cfg;
    ai_provider_load_config_nvs(&cfg);
    const ai_provider_t *p = ai_provider_get(cfg.chat_prov);

    // URL: custom uses the user-entered endpoint, everything else the preset
    if (cfg.chat_prov == AI_PROVIDER_CUSTOM) {
        if (cfg.cust_url[0] == '\0') {
            ESP_LOGE(TAG, "ai_provider_resolve_chat: custom provider selected but no URL configured");
            return ESP_ERR_INVALID_STATE;
        }
        strlcpy(out->url, cfg.cust_url, sizeof(out->url));
    } else {
        strlcpy(out->url, p->chat_url, sizeof(out->url));
    }

    // Model: override if set, else the preset default
    if (cfg.chat_model[0] != '\0') {
        strlcpy(out->model, cfg.chat_model, sizeof(out->model));
    } else {
        strlcpy(out->model, p->chat_model_def, sizeof(out->model));
    }

    out->send_reasoning_effort = p->send_reasoning_effort;
    out->reasoning_on = p->reasoning_on;
    out->reasoning_off = p->reasoning_off;
    out->key_required = p->key_required;
    out->provider_idx = cfg.chat_prov;
    return ESP_OK;
}

esp_err_t ai_provider_resolve_stt(ai_stt_cfg_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    ai_provider_cfg_t cfg;
    ai_provider_load_config_nvs(&cfg);

    // Pick the STT provider: the dedicated one when separate STT is enabled, else the chat provider
    const ai_provider_t *p;
    if (cfg.stt_sep) {
        p = ai_provider_get(cfg.stt_prov);
    } else {
        p = ai_provider_get(cfg.chat_prov);
    }

    if (!p->has_stt) {
        // No STT available for this selection (e.g. chat-only provider without a separate STT set)
        out->available = false;
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(out->url, p->stt_url, sizeof(out->url));

    // A model override only applies in separate-STT mode; shared mode always uses the preset default
    if (cfg.stt_sep && cfg.stt_model[0] != '\0') {
        strlcpy(out->model, cfg.stt_model, sizeof(out->model));
    } else {
        strlcpy(out->model, p->stt_model_def, sizeof(out->model));
    }

    out->fmt_field = p->stt_fmt_field;
    out->fmt_value = p->stt_fmt_value;
    out->send_model = p->stt_send_model;
    out->use_separate_key = cfg.stt_sep ? true : false;
    // Same-provider fallback: when the separate STT provider matches the chat provider, the
    // primary key is valid there too (used when no dedicated STT key has been saved)
    out->primary_key_ok = (!cfg.stt_sep || cfg.stt_prov == cfg.chat_prov);
    out->provider_idx = cfg.stt_sep ? cfg.stt_prov : cfg.chat_prov;
    out->available = true;
    return ESP_OK;
}

/* =============== Key <-> provider binding =============== */

// Stamp helpers live in the ai_cfg namespace alongside the rest of the config. The stamp is
// written ONLY when the corresponding key value is written, so a config-only save (blank key,
// "keep the saved one") can never silently re-bind a key to a newly selected provider.

static esp_err_t save_u8(const char *nvs_key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, nvs_key, v);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static uint8_t load_u8_default_xai(const char *nvs_key)
{
    uint8_t v = AI_PROVIDER_XAI; // Unstamped (legacy / pre-binding) key is treated as xAI's
    nvs_handle_t h;
    if (nvs_open(AI_CFG_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t stored;
        if (nvs_get_u8(h, nvs_key, &stored) == ESP_OK && stored < AI_PROVIDER_COUNT) {
            v = stored;
        }
        nvs_close(h);
    }
    return v;
}

esp_err_t ai_provider_save_key_provider_nvs(uint8_t prov)
{
    return save_u8(AI_CFG_KEY_PROV, (prov < AI_PROVIDER_COUNT) ? prov : (uint8_t)AI_PROVIDER_DEFAULT);
}

esp_err_t ai_provider_save_stt_key_provider_nvs(uint8_t prov)
{
    return save_u8(AI_CFG_SKEY_PROV, (prov < AI_PROVIDER_COUNT) ? prov : (uint8_t)AI_PROVIDER_DEFAULT);
}

uint8_t ai_provider_load_key_provider_nvs(void)
{
    return load_u8_default_xai(AI_CFG_KEY_PROV);
}

uint8_t ai_provider_load_stt_key_provider_nvs(void)
{
    return load_u8_default_xai(AI_CFG_SKEY_PROV);
}
