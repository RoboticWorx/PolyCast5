#include "polycast5_macros.h"

#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#define TAG "BT_WEB_PORTAL"

#define PORTAL_SSID "PolyCast5-Script-Setup"
#define PORTAL_PASS "pc5setup"

#define NVS_NS "bt_portal"
#define NVS_KEY "script1"
#define MAX_TXT 2048

static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ip[16] = "192.168.4.1";

// Web page HTML
static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>PolyCast5: Bluetooth Text</title></head><body>"
"<h2>Bluetooth typing payload</h2>"
"<textarea id=t rows=12 style='width:100%%' placeholder='Type what you want the device to type…'></textarea>"
"<br><button id=s>Save</button> <span id=msg></span>"
"<script>"
"async function load(){let r=await fetch('/api/script');if(r.ok){document.getElementById('t').value=await r.text();}}"
"async function save(){let body=document.getElementById('t').value;"
" let r=await fetch('/api/script',{method:'POST',headers:{'Content-Type':'text/plain'},body});"
" document.getElementById('msg').textContent=r.ok?'Saved!':'Error';}"
"document.getElementById('s').onclick=save; load();"
"</script></body></html>";

// NVS helpers
static esp_err_t nvs_write_script(const char *s)
{
	nvs_handle_t h;
	esp_err_t err;
	
	// Open NVS
	err = nvs_open(NVS_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "nvs_write_script open error: %s", esp_err_to_name(err));
		#endif
	}
	
	// Write string into NVS
	err = nvs_set_str(h, NVS_KEY, s ? s : "");
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_str error: %s", esp_err_to_name(err));
	}
	else {
		// Commit changes
		err = nvs_commit(h);
	}
	
	// Close NVS
	nvs_close(h);
	
	return err;	
}
static esp_err_t nvs_read_script(char *buf, size_t buflen, size_t *outlen)
{
	nvs_handle_t h;
	esp_err_t err;
	
	// Open NVS
	err = nvs_open(NVS_NS, NVS_READONLY, &h);
	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "nvs_write_script open error: %s", esp_err_to_name(err));
		#endif
	}
	
	size_t need = 0;
	
	// Get string len from NVS
	err = nvs_get_str(h, NVS_KEY, NULL, &need);
	if (err == ESP_OK && need > 0 && need <= buflen) {
		// Get string
		err = nvs_get_str(h, NVS_KEY, buf, &need);
		if (outlen) {
			*outlen = need;
		}
	}
	
	// Close NVS
	nvs_close(h);
	
	return err;
}

// HTTP helpers
static esp_err_t root_get(httpd_req_t *req)
{
	// Sets content type to text/html and sends INDEX_HTML
	httpd_resp_set_type(req, "text/html");
	return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t script_get(httpd_req_t *req)
{
	static char buf[MAX_TXT];
	size_t len = 0;
	
	// Get script
	if (nvs_read_script(buf, sizeof(buf), &len) == ESP_OK) {
		// On success, replies text/plain charset=utf-8 with the saved text
		httpd_resp_set_type(req, "text/plain; charset=utf-8");
		return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
	}
	
	// Else send 404
	httpd_resp_send_404(req);
	
	return ESP_OK;
}
static esp_err_t script_post(httpd_req_t *req)
{
	// Check content_len <= MAX_TXT
	if (req->content_len > MAX_TXT) {
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too big");
	}
	
	static char buf[MAX_TXT+1];
	size_t r = 0;
	
	// Loops with httpd_req_recv until all bytes are read then NUL-terminates
	while (r < req->content_len) {
		int got = httpd_req_recv(req, buf + r, req->content_len - r);
		
		if (got <= 0) {
			return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
		}
		r += got;
	}
	buf[r] = 0;
	
	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "POST /api/script: %u bytes, body='%.*s'", (unsigned)r, (int)r, buf);
	#endif
	
	// nvs_write_script on success
	esp_err_t err = nvs_write_script(buf);
	
	if (err != ESP_OK) {
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
	}
	
	httpd_resp_set_type(req, "text/plain");
	return httpd_resp_sendstr(req, "OK");
}

static httpd_handle_t start_http(void)
{
	// Starts the default server
	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
	cfg.max_uri_handlers = 8;
	httpd_handle_t srv = NULL;
	if (httpd_start(&srv, &cfg) != ESP_OK) {
		return NULL;
	}

	// Registers 3 URIs: / (GET), /api/script (GET), /api/script (POST)
	httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get};
	httpd_register_uri_handler(srv, &root);
	httpd_uri_t g = {.uri="/api/script", .method=HTTP_GET, .handler=script_get};
	httpd_register_uri_handler(srv, &g);
	httpd_uri_t p = {.uri="/api/script", .method=HTTP_POST, .handler=script_post};
	httpd_register_uri_handler(srv, &p);

	// Returns the server handle or NULL on failure
	return srv;
}

esp_err_t bluetooth_web_portal_start(void)
{
	// If already running, do nothing
	if (s_server) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "Portal already running at http://%s", s_ip);
		#endif
		
		return ESP_OK;
	}

	// Creates the default AP netif if needed
	esp_err_t err;
	if (!s_ap_netif) {
		s_ap_netif = esp_netif_create_default_wifi_ap();
		
		if (!s_ap_netif) {
			return ESP_FAIL;
		}
	}

	// Init Wi-Fi with config
	wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
	err = esp_wifi_init(&wcfg);
	if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
		#ifdef POLYCAST5_DEBUG
			ESP_LOGW(TAG, "esp_wifi_init error: %s", esp_err_to_name(err));
		#endif
		
		return err;
	}

	// Keep the Wi-Fi config in RAM
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

	// Set SSID, passphrase, auth mode, max clients, channel
	wifi_config_t ap = {0};
	strcpy((char*)ap.ap.ssid, PORTAL_SSID);
	ap.ap.ssid_len = strlen(PORTAL_SSID);
	strcpy((char*)ap.ap.password, PORTAL_PASS);
	ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
	ap.ap.max_connection = 4;
	ap.ap.channel = 1;
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

	// Start Wi-Fi (OK to call if already started - driver will ignore)
	err = esp_wifi_start();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		return err;
	}

	// Copy IP into s_ip (default uses 192.168.4.1)
	esp_netif_ip_info_t ip;
	if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
		snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
	}

	// Start http
	s_server = start_http();
	if (!s_server) {
		ESP_LOGE(TAG, "start_http error %d", s_server);
		
		return ESP_FAIL;
	}

	#ifdef POLYCAST5_DEBUG
		ESP_LOGI(TAG, "Portal running at http://%s (SSID: " PORTAL_SSID ")", s_ip);
	#endif
	
	return ESP_OK;
}

void bluetooth_web_portal_stop(void)
{
	// If running, stop
	if (s_server) {
		httpd_stop(s_server);
		s_server = NULL;
	}
	
	// Stop Wi-Fi
	esp_wifi_stop();
}

const char *bluetooth_web_portal_get_ip(void)
{
	// Return web IP
	return s_ip;
}
