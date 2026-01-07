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

#include "ai_web_portal.h"
#include "ai_funcs.h"
#include "ai_prompts.h"

#define TAG	"AI_PORTAL"

// Keep the Wi-Fi AP password NVS as-is (this module still needs portal creds)
#define AI_PASS_NS "ai_wifi"
#define AI_PASS_KEY "pass"

#define MAX_KEY_BODY 1024
#define MAX_PROMPT_BODY 4608

extern char ai_wifi_portal_pass[];

// Default AP info (replace with your existing portal creds when desired)
static httpd_handle_t ai_server = NULL;
static esp_netif_t *ai_ap_netif = NULL;

static char ai_portal_ssid[32] = "PolyCast5-AI-Portal"; // AP SSID
static char ai_portal_ip[16] = "192.168.4.1"; // AP IP cached

// HTML (single page)
static const char *HTML =
"<!doctype html><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>PolyCast5 AI Portal</title>"
"<style>body{font-family:system-ui,Arial,sans-serif;margin:16px;max-width:640px}"
"label{display:block;margin:8px 0 4px}input,textarea{width:100%;font-size:16px;padding:8px}"
"textarea{height:240px;font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;font-size:13px}</style>"
"<h2>PolyCast5 AI Portal</h2>"
"<p>Paste your <b>Grok (xAI)</b> API key below and click save. (Typically starts with <code>xai-</code>)</p>"
"<p>If you don't have an API key, visit <a href='https://console.x.ai'>console.x.ai</a> to get one for cheap ($5).</p>"
"<p><small>For security, the key is not displayed after saving.</small></p>"
"<label>Grok API key</label><input id=key placeholder='xai-...'>"
"<p><button id=save_key>Save key</button> <span id=msg_key></span></p>"
"<p>Below is the default AI system prompt. Feel free to edit it to tweak responses, create custom commands, or anything else!</p>"
"<label>System prompt</label><textarea id=prompt></textarea>"
"<p><button id=save_prompt>Save prompt</button> <span id=msg_prompt></span></p>"
"<script>"
"async function load(){"
"let r=await fetch('/api/key');if(r.ok){let j=await r.json();document.getElementById('key').value='';"
"document.getElementById('msg_key').textContent=j.has_key?('Saved (hint: '+(j.key_hint||'')+')'):'No key saved';}"
"let p=await fetch('/api/prompt');if(p.ok){let j=await p.json();document.getElementById('prompt').value=j.prompt||'';}"
"}"
"async function save_key(){"
"let api_key=document.getElementById('key').value.trim();"
"if(!api_key){document.getElementById('msg_key').textContent='Enter a key';return;}"
"let rk=await fetch('/api/key',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({api_key})});"
"document.getElementById('key').value='';"
"document.getElementById('msg_key').textContent=rk.ok?'Saved!':'Save failed';"
"}"
"async function save_prompt(){"
"let prompt=document.getElementById('prompt').value;"
"let rp=await fetch('/api/prompt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt})});"
"document.getElementById('msg_prompt').textContent=rp.ok?'Saved!':'Save failed';"
"}"
"document.getElementById('save_key').onclick=save_key;"
"document.getElementById('save_prompt').onclick=save_prompt;"
"load();"
"</script>";

/* =============== HTTP handlers =============== */

static esp_err_t root_get(httpd_req_t *req)
{
	// Serve HTML
	httpd_resp_set_type(req, "text/html");
	return httpd_resp_send(req, HTML, HTTPD_RESP_USE_STRLEN);
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
	char key[AI_API_KEY_MAX_LEN] = {0};
	esp_err_t e = xai_load_api_key_nvs(key, sizeof(key));

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
	if (req->content_len > MAX_KEY_BODY) {
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
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

	// Copy and trim
	char key[AI_API_KEY_MAX_LEN] = {0};
	strncpy(key, jk->valuestring, sizeof(key) - 1);
	trim_ascii(key);

	// Minimal sanity checks
	const size_t L = strlen(key);
	if (L < 16 || L >= sizeof(key)) {
		cJSON_Delete(j);
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid api_key");
	}

	// Reject whitespace anywhere
	for (size_t i = 0; i < L; ++i) {
		if ((unsigned char)key[i] <= ' ') {
			cJSON_Delete(j);
			return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid api_key");
		}
	}

	// Prefer xAI key format
	if (strncmp(key, "xai-", 4) != 0) {
		cJSON_Delete(j);
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected xai- key");
	}

	// Save to NVS (canonical storage in ai_funcs.c)
	esp_err_t e = xai_save_api_key_nvs(key);

	cJSON_Delete(j);

	if (e != ESP_OK) {
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
	}

	httpd_resp_set_type(req, "application/json");
	return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t prompt_get(httpd_req_t *req)
{
	// Return current prompt (NVS override if set; else compiled default)
	char prompt[4096] = {0};
	esp_err_t e = ai_prompt_load_nvs(prompt, sizeof(prompt));

	if (e != ESP_OK || prompt[0] == '\0') {
		strncpy(prompt, AI_PROMPT_AUTOKEY, sizeof(prompt) - 1);
		prompt[sizeof(prompt) - 1] = '\0';
	}

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
	}

	cJSON_AddStringToObject(root, "prompt", prompt);

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

static esp_err_t prompt_post(httpd_req_t *req)
{
	// Bound content length
	if (req->content_len > MAX_PROMPT_BODY) {
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
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
	char prompt[4096] = {0};
	strncpy(prompt, jp->valuestring, sizeof(prompt) - 1);

	// Allow empty string to mean "use default" (saved empty)
	esp_err_t e = ai_prompt_save_nvs(prompt);

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

	// Start server
	httpd_handle_t h = NULL;
	if (httpd_start(&h, &cfg) != ESP_OK) {
		return NULL; // Failed to start
	}

	// Register endpoints
	httpd_uri_t u_root = {
		.uri="/", .method = HTTP_GET, .handler = root_get, .user_ctx = NULL
	};
	httpd_uri_t u_get  = {
		.uri="/api/key", .method = HTTP_GET, .handler = key_get, .user_ctx = NULL
	};
	httpd_uri_t u_post = {
		.uri="/api/key", .method = HTTP_POST, .handler = key_post, .user_ctx = NULL
	};
	httpd_uri_t u_pget = {
		.uri="/api/prompt", .method = HTTP_GET, .handler = prompt_get, .user_ctx = NULL
	};
	httpd_uri_t u_ppost = {
		.uri="/api/prompt", .method = HTTP_POST, .handler = prompt_post, .user_ctx = NULL
	};

	httpd_register_uri_handler(h, &u_root);
	httpd_register_uri_handler(h, &u_get);
	httpd_register_uri_handler(h, &u_post);
	httpd_register_uri_handler(h, &u_pget);
	httpd_register_uri_handler(h, &u_ppost);

	return h;
}

/* =============== Wi-Fi AP bring-up =============== */

esp_err_t ai_portal_start(void)
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
	if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE)) {
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

esp_err_t ai_portal_stop(void)
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

const char *ai_portal_get_ssid(void)
{
	// Return SSID
	return ai_portal_ssid;
}

const char *ai_portal_get_pass(void)
{
	// Return password
	return ai_wifi_portal_pass;
}

const char *ai_portal_get_ip(void)
{
	// Return IP
	return ai_portal_ip;
}

/* =============== NVS =============== */

esp_err_t ai_wifi_pass_save_nvs(const char *val)
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

esp_err_t ai_wifi_pass_load_nvs(char *out, size_t out_sz)
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
