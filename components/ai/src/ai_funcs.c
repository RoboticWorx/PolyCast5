#include "polycast5_macros.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "cJSON.h"
#include "nvs.h"

#include "wifi_funcs.h"
#include "gpio_funcs.h"
#include "bluetooth_funcs.h"

#include "wifi_task.h"
#include "gpio_task.h"
#include "ai_funcs.h"
#include "ai_prompt.h"
#include "ai_task.h"

#define TAG "AI_FUNCS"

// NVS keys for OpenAI API key
#define OPENAI_NS  "openai"
#define OPENAI_KEY "api_key"

// Hard ceiling so a bad response doesn't eat all RAM
#define AI_HTTP_BODY_MAX_CAP (48 * 1024)

// TODO: Add mutexes if these are shared across tasks
extern bool wifi_connected;
extern bluetooth_state_t bluetooth_state;

typedef struct {
	char *buf; // Accumulated body bytes (NUL-terminated for convenience)
	size_t len; // Current length (not including NUL)
	size_t cap; // Allocated capacity (includes space for NUL)
	bool oom; // Set if we failed to allocate more memory
	bool truncated; // Set if we hit max cap and refused to grow
	bool caps_alloc; // true if allocated via heap_caps_* (PSRAM/8BIT), false if malloc/realloc
} http_accum_t;

// Save API key string to NVS
esp_err_t openai_save_api_key_nvs(const char *api_key)
{
	nvs_handle_t h;

	// Open NVS namespace
	esp_err_t err = nvs_open(OPENAI_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}

	// Save the API key string
	err = nvs_set_str(h, OPENAI_KEY, api_key);

	// Commit only on success
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}

	// Close handle
	nvs_close(h);

	return err;
}

// Load API key string from NVS
esp_err_t openai_load_api_key_nvs(char *out, size_t out_sz)
{
	nvs_handle_t h;

	// Open NVS namespace
	esp_err_t err = nvs_open(OPENAI_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		return err;
	}

	// Read the API key string
	size_t sz = out_sz;
	err = nvs_get_str(h, OPENAI_KEY, out, &sz);

	// Close handle
	nvs_close(h);

	return err;
}


/* String cleanup helpers */

// Find the last occurrence of needle in haystack (local helper)
static char *strrstr_local(const char *haystack, const char *needle)
{
	// Treat empty needle as "found at start"
	if (!haystack || !needle || !*needle) {
		return (char *)haystack;
	}

	char *last = NULL;

	// Walk forward, remembering the last match
	for (const char *p = haystack; (p = strstr(p, needle)) != NULL; ++p) {
		last = (char *)p;
	}

	return last;
}

// Trim leading/trailing whitespace in-place
static void trim_inplace(char *s)
{
	if (!s) {
		return;
	}

	// Leading trim
	char *p = s;
	while (*p && (unsigned char)*p <= ' ') {
		++p;
	}

	// Shift down if we advanced
	if (p != s) {
		memmove(s, p, strlen(p) + 1);
	}

	// Trailing trim
	size_t n = strlen(s);
	while (n > 0 && (unsigned char)s[n - 1] <= ' ') {
		s[n - 1] = '\0';
		n--;
	}
}

// Strip surrounding "quotes" and/or ``` fenced blocks from an output string
static void strip_wrappers_inplace(char *s)
{
	if (!s) {
		return;
	}

	// Normalize whitespace first
	trim_inplace(s);

	// Remove surrounding quotes:  " ... "
	size_t n = strlen(s);
	if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
		memmove(s, s + 1, n - 2);
		s[n - 2] = '\0';

		trim_inplace(s);
	}

	// Remove ``` fenced blocks
	if (!strncmp(s, "```", 3)) {
		char *first_nl = strchr(s, '\n');
		if (first_nl) {
			char *body = first_nl + 1;

			// Truncate at last fence
			char *last_fence = strrstr_local(body, "```");
			if (last_fence) {
				*last_fence = '\0';
			}

			// Shift body down
			memmove(s, body, strlen(body) + 1);

			trim_inplace(s);
		}
	}
}


/* Responses API parsing helpers */

// The "text" field can be a string OR {"value":"..."} depending on output shape
static const char *json_text_string_or_value_obj(cJSON *text_item)
{
	// Simple string case: "text":"..."
	if (cJSON_IsString(text_item)) {
		return text_item->valuestring;
	}

	// Object case: "text":{"value":"..."}
	if (cJSON_IsObject(text_item)) {
		cJSON *val = cJSON_GetObjectItem(text_item, "value");
		if (cJSON_IsString(val)) {
			return val->valuestring;
		}
	}

	return NULL;
}

// Extract assistant output text from a /v1/responses JSON payload
static const char *responses_extract_text(cJSON *json)
{
	if (!json) {
		return NULL;
	}

	// Fast path: sometimes there's a convenience "output_text" field
	cJSON *out_text = cJSON_GetObjectItem(json, "output_text");
	if (cJSON_IsString(out_text) && out_text->valuestring && out_text->valuestring[0]) {
		return out_text->valuestring;
	}

	// Common REST shape: output: [ { content:[{type:"output_text", text:"..."}] }, ... ]
	cJSON *output = cJSON_GetObjectItem(json, "output");
	if (!cJSON_IsArray(output)) {
		return NULL;
	}

	cJSON *item = NULL;
	cJSON_ArrayForEach(item, output) {
		// Some items include "content" array
		cJSON *content = cJSON_GetObjectItem(item, "content");
		if (!cJSON_IsArray(content)) {
			continue;
		}

		cJSON *part = NULL;
		cJSON_ArrayForEach(part, content) {
			// Preferred: { "type":"output_text", "text":"..." }
			cJSON *ptype = cJSON_GetObjectItem(part, "type");
			if (cJSON_IsString(ptype) && strcmp(ptype->valuestring, "output_text") == 0) {
				cJSON *t = cJSON_GetObjectItem(part, "text");
				const char *s = json_text_string_or_value_obj(t);
				if (s && s[0]) {
					return s;
				}
			}

			// Fallback: sometimes a part might just have "text":"..."
			cJSON *t2 = cJSON_GetObjectItem(part, "text");
			const char *s2 = json_text_string_or_value_obj(t2);
			if (s2 && s2[0]) {
				return s2;
			}
		}
	}

	return NULL;
}


/* HTTP accumulator (safe alloc/realloc/free)  */

// Free the accumulator buffer using the correct allocator
static void acc_free(http_accum_t *a)
{
	if (!a || !a->buf) {
		return;
	}

	// Free with the matching heap
	if (a->caps_alloc) {
		heap_caps_free(a->buf);
	}
	else {
		free(a->buf);
	}

	a->buf = NULL;
	a->len = 0;
	a->cap = 0;
	a->oom = false;
	a->truncated = false;
	a->caps_alloc = false;
}

// Initialize the accumulator buffer (prefer PSRAM, fallback to normal heap)
static bool acc_init(http_accum_t *a, size_t initial_cap)
{
	if (!a) {
		return false;
	}

	// Clear state
	memset(a, 0, sizeof(*a));

	// Ensure minimum capacity and room for NUL terminator
	if (initial_cap < 512) {
		initial_cap = 512;
	}

	// Try PSRAM first (8-bit accessible)
	a->buf = (char *)heap_caps_malloc(initial_cap, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	a->caps_alloc = (a->buf != NULL);

	// Fallback to normal heap if PSRAM alloc failed
	if (!a->buf) {
		a->buf = (char *)malloc(initial_cap);
		a->caps_alloc = false;
	}

	// Bail if we couldn't allocate anything
	if (!a->buf) {
		a->oom = true;
		return false;
	}

	// Initialize empty string
	a->cap = initial_cap;
	a->len = 0;
	a->buf[0] = '\0';

	return true;
}

// Grow the accumulator buffer (IMPORTANT: do not mix heap_caps_realloc with malloc pointers)
static bool acc_reserve(http_accum_t *a, size_t need_cap)
{
	if (!a) {
		return false;
	}

	// Already enough space
	if (need_cap <= a->cap) {
		return true;
	}

	// Clamp growth so we don't eat all RAM on huge replies
	size_t nc = a->cap ? a->cap : 1024;
	while (nc < need_cap) {
		nc *= 2;
	}

	if (nc > AI_HTTP_BODY_MAX_CAP) {
		a->truncated = true;
		return false;
	}

	// Realloc using the matching allocator
	char *nb = NULL;

	// If this buffer came from heap_caps_*, only use heap_caps_realloc
	if (a->caps_alloc) {
		nb = (char *)heap_caps_realloc(a->buf, nc, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
	}
	// If this buffer came from malloc/realloc, only use realloc
	else {
		nb = (char *)realloc(a->buf, nc);
	}

	// If realloc failed, mark OOM and keep old buffer intact
	if (!nb) {
		a->oom = true;
		return false;
	}

	// Commit the grow
	a->buf = nb;
	a->cap = nc;

	return true;
}


/* esp_http_client event handler */

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
	http_accum_t *a = (http_accum_t *)evt->user_data;

	// Ignore events if we weren't given an accumulator
	if (!a) {
		return ESP_OK;
	}

	// Connected: reset length so we never accidentally append stale bytes
	if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
		a->len = 0;

		if (a->buf && a->cap > 0) {
			a->buf[0] = '\0';
		}

		return ESP_OK;
	}

	// Body chunks: append safely and always keep NUL-terminated
	if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
		// If we already know we're out-of-memory or truncated, just ignore remaining data
		if (a->oom || a->truncated) {
			return ESP_OK;
		}

		size_t n = (size_t)evt->data_len;

		// Ensure capacity for new bytes + NUL
		if (!acc_reserve(a, a->len + n + 1)) {
			// acc_reserve sets oom/truncated flags; stop appending
			return ESP_OK;
		}

		// Append chunk
		memcpy(a->buf + a->len, evt->data, n);
		a->len += n;

		// Keep it a valid C-string for logging/parsing convenience
		a->buf[a->len] = '\0';

		return ESP_OK;
	}

	return ESP_OK;
}


/* OpenAI request (Responses API) */

// Send a user command to OpenAI Responses API and return the generated HID script
esp_err_t openai_send_command(const char *command, char *response_buf, size_t buf_sz)
{
	// Validate args
	if (!command || !response_buf || buf_sz == 0) {
		return ESP_ERR_INVALID_ARG;
	}

	// Default output to empty
	response_buf[0] = '\0';

	// Load API key from NVS
	char api_key[AI_API_KEY_MAX_LEN] = {0};
	if (openai_load_api_key_nvs(api_key, sizeof(api_key)) != ESP_OK || api_key[0] == '\0') {
		ESP_LOGE(TAG, "Failed to load OpenAI API key from NVS");
		return ESP_FAIL;
	}

	// Create JSON payload root
	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return ESP_ERR_NO_MEM;
	}

	// Model
	cJSON_AddStringToObject(root, "model", "gpt-5-nano");

	// Output token budget (keep moderate so replies stay small)
	cJSON_AddNumberToObject(root, "max_output_tokens", 2048);

	// Force plain text output (reduces chance of tool output / weird formats)
	cJSON *text = cJSON_AddObjectToObject(root, "text");
	cJSON *format = cJSON_AddObjectToObject(text, "format");
	cJSON_AddStringToObject(format, "type", "text");
	cJSON_AddStringToObject(text, "verbosity", "medium");

	// Reduce chance 'all tokens go to reasoning; no message output'
	cJSON *reasoning = cJSON_AddObjectToObject(root, "reasoning");
	cJSON_AddStringToObject(reasoning, "effort", "low");

	// Build input message array
	cJSON *input = cJSON_AddArrayToObject(root, "input");

	// Developer message (instructions)
	cJSON *dev = cJSON_CreateObject();
	cJSON_AddStringToObject(dev, "role", "developer");
	cJSON_AddStringToObject(dev, "content", AI_PROMPT);
	cJSON_AddItemToArray(input, dev);

	// User message (the actual command)
	cJSON *usr = cJSON_CreateObject();
	cJSON_AddStringToObject(usr, "role", "user");
	cJSON_AddStringToObject(usr, "content", command);
	cJSON_AddItemToArray(input, usr);

	// Serialize JSON payload
	char *payload = cJSON_PrintUnformatted(root);

	// Root no longer needed after serialization
	cJSON_Delete(root);

	// Bail on allocation failure
	if (!payload) {
		return ESP_ERR_NO_MEM;
	}

	// Initialize HTTP body accumulator
	http_accum_t acc;
	if (!acc_init(&acc, 2048)) {
		free(payload);
		return ESP_ERR_NO_MEM;
	}

	// Configure HTTPS request
	esp_http_client_config_t config = {
		.url = "https://api.openai.com/v1/responses",
		.method = HTTP_METHOD_POST,

		// Use ESP-IDF CA bundle
		.crt_bundle_attach = esp_crt_bundle_attach,

		// Reasonable timeout for cellular/hotspot too
		.timeout_ms = 20000,

		// Capture response body via callback
		.event_handler = http_evt,
		.user_data = &acc,
	};

	// Create client
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client) {
		free(payload);
		acc_free(&acc);
		return ESP_FAIL;
	}

	// Build Authorization header
	char auth_header[512];
	snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

	// Set headers
	esp_http_client_set_header(client, "Authorization", auth_header);
	esp_http_client_set_header(client, "Content-Type", "application/json");
	esp_http_client_set_header(client, "Accept", "application/json");

	// Attach POST body
	esp_http_client_set_post_field(client, payload, strlen(payload));

	// Perform the request
	esp_err_t err = esp_http_client_perform(client);

	// Read HTTP status code
	int status = esp_http_client_get_status_code(client);

	// Cleanup request resources
	free(payload);
	esp_http_client_cleanup(client);

	// Network/TLS/HTTP failure
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));

		acc_free(&acc);
		return err;
	}

	// Empty body means something went wrong upstream
	if (acc.len == 0 || !acc.buf) {
		ESP_LOGE(TAG, "Empty HTTP body (len=0) from OpenAI");

		acc_free(&acc);
		return ESP_FAIL;
	}

	// Log a safe snippet for debugging
	const int snip = (acc.len > 300) ? 300 : (int)acc.len;
	ESP_LOGI(TAG, "OpenAI HTTP %d body[0:%d]=%.*s%s",
			status,
			snip,
			snip,
			acc.buf,
			(acc.len > (size_t)snip) ? "..." : "");

	// Non-200 still might include useful error JSON in body (already logged)
	if (status != 200) {
		acc_free(&acc);
		return ESP_FAIL;
	}

	// If we ran out of memory or hit the cap, don't try to parse partial JSON
	if (acc.oom || acc.truncated) {
		ESP_LOGE(TAG, "HTTP body too large/failed to grow (len=%u cap=%u) oom=%d trunc=%d",
				(unsigned)acc.len,
				(unsigned)acc.cap,
				acc.oom,
				acc.truncated);

		acc_free(&acc);
		return ESP_ERR_NO_MEM;
	}

	// Parse JSON response
	cJSON *json = cJSON_ParseWithLength(acc.buf, acc.len);
	if (!json) {
		ESP_LOGE(TAG, "JSON parse failed (body_len=%u)", (unsigned)acc.len);

		acc_free(&acc);
		return ESP_FAIL;
	}

	// Extract output text from Responses API payload
	const char *extracted_text = responses_extract_text(json);

	// Handle no-output cases with better logging
	if (!extracted_text || !extracted_text[0]) {
		cJSON *st  = cJSON_GetObjectItem(json, "status");
		cJSON *inc = cJSON_GetObjectItem(json, "incomplete_details");

		const bool inc_present = (inc && !cJSON_IsNull(inc));

		ESP_LOGE(TAG, "No output. status=%s incomplete_details=%s",
				cJSON_IsString(st) ? st->valuestring : "(none)",
				inc_present ? "(present)" : "(null/none)");

		cJSON_Delete(json);
		acc_free(&acc);
		return ESP_FAIL;
	}

	// Copy extracted text to user buffer
	strncpy(response_buf, extracted_text, buf_sz - 1);
	response_buf[buf_sz - 1] = '\0';

	// Cleanup JSON + HTTP body
	cJSON_Delete(json);
	acc_free(&acc);

	// Strip quotes/fences and trim whitespace
	strip_wrappers_inplace(response_buf);

	// If we ended up empty after stripping, treat as error
	if (response_buf[0] == '\0') {
		return ESP_FAIL;
	}

	return ESP_OK;
}