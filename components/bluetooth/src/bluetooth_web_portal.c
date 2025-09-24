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

#include "bluetooth_web_portal.h"

#define TAG "BLUETOOTH_WEB_PORTAL"

#define WIFI_PASS_NS "wifi_pass"
#define WIFI_PASS_KEY "pass"

#define BT_SCRIPT_NS "bt_portal"
#define BT_SCRIPT_KEY_FMT "script_%02d"

#define BT_SCRIPT_MENU_NS "keyb_menu"
#define BT_SCRIPT_MENU_KEY_COUNT "count"
#define BT_SCRIPT_MENU_KEY_FMT "item_%02d"

#define MAX_HTTP_BODY_TXT 2048

extern char bt_wifi_portal_pass[];

static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ip[16] = "192.168.4.1";

// Web page HTML (UI: pick index, name, and payload)
static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>PolyCast5 BT Scripts</title>"
"<style>body{font-family:system-ui,Arial,sans-serif;margin:16px}label{display:block;margin:8px 0 4px}"
"input,textarea,select{width:100%;box-sizing:border-box}textarea{height:180px}</style>"
"</head><body>"
"<h2>PolyCast5 BT Scripts</h2>"
"<header>Script index (0-based)</header>"
"<small>So the first one you add is 0, next is 1, etc.</small>"
"<div><label>Script index</label><input id=idx type=number min=0 max=15 value=0> <button id=load>Load</button></div>"
"<div><label>Name</label><input id=name maxlength=32 placeholder='Short label for device menu'></div>"
"<div><label>Payload</label><textarea id=body placeholder='What the device should type…'></textarea></div>"
"<div><button id=save>Save (add/edit)</button> <button id=del>Delete</button> <span id=msg></span></div>"

"<p>Here are some additional commands so you can do more than just type text:</p>"
"<p>"
"&lt;delay=x&gt; - Wait for x milliseconds"
"<br>&lt;hold:c=x&gt; - Hold c for x milliseconds"
"<br>&lt;enter&gt; - Enter"
"<br>&lt;tab&gt; - Tab"
"<br>&lt;esc&gt; - Escape"
"<br>&lt;ctrl&gt; - Ctrl"
"<br>&lt;shift&gt; - Shift"
"<br>&lt;alt&gt; - Alt/Option"
"<br>&lt;win&gt; - Windows/Cmd"
"<br>&lt;space&gt; - Space"
"<br>&lt;bs&gt; - Backspace"
"<br>&lt;del&gt; - Forward delete"
"<br>&lt;up&gt; - Up arrow"
"<br>&lt;down&gt; - Down arrow"
"<br>&lt;left&gt; - Left arrow"
"<br>&lt;right&gt; - Right arrow"
"<br>&lt;home&gt; - Home"
"<br>&lt;pgup&gt; - Page up"
"<br>&lt;pgdn&gt; - Page down"
"<br>&lt;fx&gt; - Function x (e.g. f1, f2, etc.)"
"<br><br>You can also combine commands like &lt;ctrl+shift+v&gt; or &lt;ctrl+c&gt;. Please also note that hitting return/enter when typing is treated as &lt;enter&gt;."
"<br><br>Example: If I want to open up a given program to run something on windows I would maybe have something like this with delays to make "
"sure that the computer opened whatever before continuing: "
"&lt;win+s&gt;&lt;delay=500&gt;browser&lt;enter&gt;&lt;delay=500&gt;https://youtu.be/dQw4w9WgXcQ?si=fCygLTFX4Usi2nlR&lt;enter&gt;"
"<br><br>More examples and info here:"
"<br>github.com/RoboticWorx/PolyCast5/tree/main/scripts/bluetooth_examples"
"</p>"

"<hr><h3>Existing Scripts</h3><select id=list size=6 style='height:160px'></select>"
"<script>"
"async function refreshList(){let r=await fetch('/api/scripts');if(!r.ok)return;"
" let j=await r.json();let s=document.getElementById('list');s.innerHTML='';"
" for(let i=0;i<j.count;i++){let o=document.createElement('option');o.value=i;o.textContent=`${i}: ${j.labels[i]||'(unnamed)'}`;s.appendChild(o);}}"
"async function loadOne(i){let r=await fetch('/api/script?index='+i);if(!r.ok){document.getElementById('msg').textContent='Not found';return}"
" let j=await r.json();document.getElementById('idx').value=j.index;document.getElementById('name').value=j.name||'';"
" document.getElementById('body').value=j.body||'';document.getElementById('msg').textContent='Loaded';}"
"async function save(){let i=+document.getElementById('idx').value;let name=document.getElementById('name').value;let body=document.getElementById('body').value;"
" let r=await fetch('/api/script',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:i,name,body})});"
" document.getElementById('msg').textContent=r.ok?'Saved!':'Save failed';if(r.ok)refreshList();}"
"document.getElementById('load').onclick=()=>loadOne(+document.getElementById('idx').value);"
"document.getElementById('save').onclick=save;"
"document.getElementById('del').onclick=async()=>{let i=+document.getElementById('idx').value;let r=await fetch('/api/script?index='+i,{method:'DELETE'});"
"document.getElementById('msg').textContent=r.ok?'Deleted':'Delete failed';if(r.ok){refreshList();document.getElementById('name').value='';document.getElementById('body').value='';}};"
"document.getElementById('list').ondblclick=e=>{if(e.target&&e.target.value!==undefined)loadOne(+e.target.value)};"
"refreshList();"
"</script></body></html>";

/* ========= NVS handlers ========= */

// Read the count of user scripts
uint8_t bluetooth_script_count_get(void)
{
	xSemaphoreTake(xBluetoothScriptMutex, portMAX_DELAY); // Lock Bluetooth

 	nvs_handle_t h;
 	uint8_t count = 0;
 	
 	// Open NVS
 	esp_err_t err = nvs_open(BT_SCRIPT_MENU_NS, NVS_READONLY, &h);
 	if (err == ESP_OK) {
		// Get count
 	 	if (nvs_get_u8(h, BT_SCRIPT_MENU_KEY_COUNT, &count) != ESP_OK) {
			// 0 if DNE
 	 	 	count = 0;
 	 	 	
 	 	 	#ifdef POLYCAST5_DEBUG
			ESP_LOGE(TAG, "bluetooth_script_count_get nvs_get_u8 failed: %s", esp_err_to_name(err));
			#endif
 	 	}
 	 	
 	 	// Close NVS
 	 	nvs_close(h);
 	}
 	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "bluetooth_script_count_get nvs_open failed: %s", esp_err_to_name(err));
		#endif
	}
	
 	if (count > MAX_KEYBOARD_SCRIPTS) {
	    count = MAX_KEYBOARD_SCRIPTS;
	    ESP_LOGW(TAG, "bluetooth_script_count_get MAX_KEYBOARD_SCRIPTS reached: %d", MAX_KEYBOARD_SCRIPTS);
	}

	xSemaphoreGive(xBluetoothScriptMutex); // Release Bluetooth
	
	return count;
}

// Persist the count of user scripts
static esp_err_t bluetooth_script_count_set(uint8_t count)
{
 	nvs_handle_t h;
 	
 	if (count > MAX_KEYBOARD_SCRIPTS) {
	    count = MAX_KEYBOARD_SCRIPTS;
	    ESP_LOGW(TAG, "bluetooth_script_count_set MAX_KEYBOARD_SCRIPTS reached: %d", MAX_KEYBOARD_SCRIPTS);
	}
 	
 	// Open NVS
 	esp_err_t err = nvs_open(BT_SCRIPT_MENU_NS, NVS_READWRITE, &h);
 	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "bluetooth_script_count_set nvs_open failed: %s", esp_err_to_name(err));
		#endif
		
 	 	return err;
 	}
 	
 	// Set count
 	err = nvs_set_u8(h, BT_SCRIPT_MENU_KEY_COUNT, count);
 	if (err == ESP_OK) {
		// Commit changes on success
 	 	err = nvs_commit(h);
 	}
 	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "bluetooth_script_count_set nvs_set_u8 failed: %s", esp_err_to_name(err));
		#endif
	}
	
	// Close NVS
 	nvs_close(h);
 	return err;
}

// Read a script label into caller buffer (buflen should be >= bluetooth_SCRIPT_LABEL_MAX_LEN + 1)
esp_err_t bluetooth_script_label_get(uint8_t idx, char *buf, size_t buflen)
{
	xSemaphoreTake(xBluetoothScriptMutex, portMAX_DELAY); // Lock Bluetooth

 	nvs_handle_t h;
 	
 	// Open NVS
 	esp_err_t err = nvs_open(BT_SCRIPT_MENU_NS, NVS_READONLY, &h);
 	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "bluetooth_script_label_get nvs_open failed: %s", esp_err_to_name(err));
		#endif
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

	xSemaphoreGive(xBluetoothScriptMutex); // Release Bluetooth
 	return err;
}

// Write a label for the given script index
static esp_err_t bluetooth_script_label_set(uint8_t idx, const char *label)
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
 	}
 	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "bluetooth_script_label_set nvs_set_str failed: %s", esp_err_to_name(err));
		#endif
	}
	
	// Close NVS
 	nvs_close(h);
 	return err;
}

// Read a payload body into caller buffer (need must fit into buflen)
esp_err_t bluetooth_script_body_get(uint8_t idx, char *buf, size_t buflen, size_t *outlen)
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
 	
 	// Get the body string len
 	err = nvs_get_str(h, key, NULL, &need);
 	
 	// If NVS good and size is within allowed
 	if ((err == ESP_OK) && (need > 0) && (need <= buflen)) {
		// Get the actual body string
 	 	err = nvs_get_str(h, key, buf, &need);
 	 	if (outlen != NULL) {
			// Update outlen
 	 	 	*outlen = need;
 	 	}
 	}
 	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "bluetooth_script_body_get string parameters wrong or NVS failed: %s", esp_err_to_name(err));
		#endif
	}
	
	// Close NVS
 	nvs_close(h);
 	return err;
}

// Write a payload body for the given script index
static esp_err_t bluetooth_script_body_set(uint8_t idx, const char *body)
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
 	
 	// Set body string
 	err = nvs_set_str(h, key, (body != NULL) ? body : ""); // If body NULL, set empty string
 	if (err == ESP_OK) {
		// Commit changes on success
 	 	err = nvs_commit(h);
 	}
 	else {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "bluetooth_script_body_set nvs_set_str failed: %s", esp_err_to_name(err));
		#endif
	}
	
	// Close NVS
 	nvs_close(h);
 	return err;
}

/* ========= HTTP handlers ========= */

// Serve the single-page HTML UI
static esp_err_t root_get(httpd_req_t *req)
{
 	httpd_resp_set_type(req, "text/html");
 	
 	return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// GET /api/scripts -> {"count":N,"labels":[...]}
static esp_err_t scripts_list_get(httpd_req_t *req)
{
	// Get num current scripts
 	uint8_t count = bluetooth_script_count_get();

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
 	for (uint8_t i = 0; i < count; i++) {
 	 	char lbl[BT_SCRIPT_LABEL_MAX_LEN + 1] = {0}; // Buffer
 	 	
 	 	// Add the label or "" to the array
 	 	if ((bluetooth_script_label_get(i, lbl, sizeof(lbl)) == ESP_OK) && (lbl[0] != '\0')) {
 	 	 	cJSON_AddItemToArray(labels, cJSON_CreateString(lbl));
 	 	}
 	 	else {
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

// Parse index from the query string
static bool get_query_index(httpd_req_t *req, int *out_idx)
{
	// Get query string len
 	int len = httpd_req_get_url_query_len(req);
 	if (len <= 0) {
 	 	return false; // Not found
 	}
 	
 	// Buffer to hold query string
 	char *q = (char *)calloc(1, (size_t)len + 1);
 	if (q == NULL) {
 	 	return false;
 	}
 	
 	// Copies the raw query string (e.g., "index=3") into q
 	(void)httpd_req_get_url_query_str(req, q, (size_t)len + 1);
 	
 	// Output buf
 	char val[8] = {0};
 	
 	// Extract the value of key "index" into val
 	bool ok = (httpd_query_key_value(q, "index", val, sizeof(val)) == ESP_OK);
 	
 	free(q);
 	if (!ok) {
 	 	return false;
 	}
 	
 	// Convert the ASCII number in val to an int and store it in *out_idx
 	*out_idx = atoi(val);
 	
 	// Was found
 	return true;
}

// GET /api/script?index=N -> {"index":N,"name":"...","body":"..."}
static esp_err_t script_one_get(httpd_req_t *req)
{
 	int idx = -1;
 	
 	// Parse ?index=N
 	if ((!get_query_index(req, &idx)) || (idx < 0) || (idx >= MAX_KEYBOARD_SCRIPTS)) {
		// If missing or outside [0, MAX_KEYBOARD_SCRIPTS - 1], returns 400 "bad index"
 	 	return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
 	}

 	// Name buf
 	char name[BT_SCRIPT_LABEL_MAX_LEN + 1] = {0};

 	// Allocate a large heap buffer for the script's payload to protect the httpd task stack
 	char *body = (char *)malloc(MAX_HTTP_BODY_TXT + 1);
 	if (body == NULL) { // Error check
 	 	return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
 	}
 	body[0] = '\0'; // NUL-terminate

 	size_t blen = 0;
 	
 	// Read label and body into buffers
 	(void)bluetooth_script_label_get((uint8_t)idx, name, sizeof(name));
 	(void)bluetooth_script_body_get((uint8_t)idx, body, MAX_HTTP_BODY_TXT + 1, &blen);

 	// Build the JSON response
 	cJSON *root = cJSON_CreateObject();
 	if (root == NULL) {
		// If allocation fails, free the body buffer and return 500
 	 	free(body);
 	 	return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
 	}
 	
 	// Add fields to the JSON: always include index, name, and body
 	cJSON_AddNumberToObject(root, "index", idx);
 	cJSON_AddStringToObject(root, "name", (name[0] != '\0') ? name : "");
 	cJSON_AddStringToObject(root, "body", (body[0] != '\0') ? body : "");

	// Serialize to a compact JSON string, free the tree, and check for OOM
 	char *txt = cJSON_PrintUnformatted(root);
 	cJSON_Delete(root);
 	if (txt == NULL) {
 	 	free(body);
 	 	return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
 	}

	// Send the JSON
 	httpd_resp_set_type(req, "application/json");
 	esp_err_t err = httpd_resp_sendstr(req, txt);
 	free(txt);
 	free(body);
 	
 	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "script_one_get httpd_resp_sendstr failed: %s", esp_err_to_name(err));
		#endif
	}
	
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

// POST /api/script -> {"index":N,"name":"...","body":"..."}
static esp_err_t script_one_post(httpd_req_t *req)
{
 	// Reject bodies larger than MAX_HTTP_BODY_TXT + small JSON overhead.
 	if (req->content_len > (MAX_HTTP_BODY_TXT + 256)) {
 	 	return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
 	}

 	// Create script body buf
 	char *buf = (char *)malloc((size_t)req->content_len + 1);
 	if (buf == NULL) {
 	 	return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
 	}
 	
 	size_t r = 0;
 	
 	// Read from the socket until the entire body is received
 	while (r < (size_t)req->content_len) {
		// Read the body into the heap buffer
 	 	int got = httpd_req_recv(req, buf + r, req->content_len - (int)r);
 	 	
 	 	if (got <= 0) {
			// On error clean up and return 500
 	 	 	free(buf);
 	 	 	return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
 	 	}
 	 	
 	 	r += (size_t)got; // Increment
 	}
 	buf[r] = '\0'; // NUL-terminate

 	// Parse the body into a JSON object
 	cJSON *j = cJSON_Parse(buf);
 	if (j == NULL) {
		// On error clean up
 	 	free(buf);
 	 	return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
 	}
 	
 	// Extract the three fields
 	cJSON *jidx = cJSON_GetObjectItemCaseSensitive(j, "index");
 	cJSON *jname = cJSON_GetObjectItemCaseSensitive(j, "name");
 	cJSON *jbody = cJSON_GetObjectItemCaseSensitive(j, "body");
 	
 	// Index must be a number
 	if (!cJSON_IsNumber(jidx) || ((jname != NULL) && !cJSON_IsString(jname)) || ((jbody != NULL) && !cJSON_IsString(jbody))) {
		// Else clean up and throw error
 	 	cJSON_Delete(j);
 	 	free(buf);
 	 	return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fields");
 	}

 	// Validate index bounds
 	int idx = jidx->valueint;
 	if ((idx < 0) || (idx >= MAX_KEYBOARD_SCRIPTS)) {
		// Else clean up and throw error
 	 	cJSON_Delete(j);
 	 	free(buf);
 	 	return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
 	}

 	// Pull strings out of cJSON
 	const char *name_in = ((jname != NULL) && (jname->valuestring != NULL)) ? jname->valuestring : "";
 	const char *body_in = ((jbody != NULL) && (jbody->valuestring != NULL)) ? jbody->valuestring : "";

	// Reject saving empty scripts (ask client to use DELETE)
    if ((name_in[0] == '\0') && (body_in[0] == '\0')) {
		// Clean up
        cJSON_Delete(j);
		free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty script (use DELETE)");
    }

	// Get count
	uint8_t count = bluetooth_script_count_get();

	// Avoid sparse indices: if idx > count, append at tail
	if ((uint8_t)idx > count) {
		idx = count;
	}

	// Build/clean label
	char label[BT_SCRIPT_LABEL_MAX_LEN + 1];

	// If name exists, use that
	if (name_in[0] != '\0') {
		strncpy(label, name_in, BT_SCRIPT_LABEL_MAX_LEN);
		label[BT_SCRIPT_LABEL_MAX_LEN] = '\0';
		trim_ascii(label);
	}
	// Else create label from body
	else {
		label_from_body(body_in, label, sizeof(label));

		// If not that, just number it
		if (label[0] == '\0') {
			snprintf(label, sizeof(label), "Script %02d", idx);
		}
	}

	// Grow count only when appending at tail
	if ((uint8_t)idx >= count) {
		// Update count
		esp_err_t ecount = bluetooth_script_count_set((uint8_t)(idx + 1));

		// Check
		if (ecount != ESP_OK) {
			cJSON_Delete(j); free(buf);
			return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs-count");
		}
	}

 	// Persist label to NVS
 	esp_err_t err = bluetooth_script_label_set((uint8_t)idx, label);
 	// If NVS update fails, return 500
 	if (err != ESP_OK) {
 	 	cJSON_Delete(j);
 	 	free(buf);
 	 	return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs-label");
 	}
 	// Persist body to NVS
 	err = bluetooth_script_body_set((uint8_t)idx, body_in);
 	// If NVS update fails, return 500
 	if (err != ESP_OK) {
 	 	cJSON_Delete(j);
 	 	free(buf);
 	 	return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs-body");
 	}

 	// Send a small success JSON in one call (no chunking)
 	char okjson[32];
 	int n = snprintf(okjson, sizeof(okjson), "{\"ok\":true,\"index\":%d}", idx);
 	if ((n < 0) || (n >= (int)sizeof(okjson))) {
 	 	// Extremely unlikely with small ints; fallback to minimal success
 	 	strcpy(okjson, "{\"ok\":true}");
 	}
 	httpd_resp_set_type(req, "application/json");
 	err = httpd_resp_sendstr(req, okjson);
 	
 	if (err != ESP_OK) {
		#ifdef POLYCAST5_DEBUG
		ESP_LOGE(TAG, "script_one_post httpd_resp_sendstr failed: %s", esp_err_to_name(err));
		#endif
	}

	// Frees the parsed JSON and the body buffer, and returns the send status
 	cJSON_Delete(j);
 	free(buf);
 	return err;
}

static esp_err_t script_one_delete(httpd_req_t *req)
{
    int idx = -1;
    if ((!get_query_index(req, &idx)) || (idx < 0)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }

    uint8_t count = bluetooth_script_count_get();
    if ((uint8_t)idx >= count) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }

    char next_label[BT_SCRIPT_LABEL_MAX_LEN + 1];
    char *next_body = (char *)malloc(MAX_HTTP_BODY_TXT + 1);
    if (!next_body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");

    // Shift [idx+1..count-1] down into [idx..count-2]
    for (uint8_t i = (uint8_t)idx; i + 1 < count; i++) {
        next_label[0] = '\0';
        size_t blen = 0;
        (void)bluetooth_script_label_get(i + 1, next_label, sizeof(next_label));
        (void)bluetooth_script_body_get (i + 1, next_body, MAX_HTTP_BODY_TXT + 1, &blen);

        (void)bluetooth_script_label_set(i, next_label);
        (void)bluetooth_script_body_set (i, (blen > 0) ? next_body : "");
    }

    // Clear old tail
    (void)bluetooth_script_label_set(count - 1, "");
    (void)bluetooth_script_body_set (count - 1, "");
    free(next_body);

    // Decrement count
    (void)bluetooth_script_count_set(count - 1);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ========== HTTP server bootstrap ========== */

// Start the embedded HTTP server and register endpoints
static httpd_handle_t start_http(void)
{
	// C0nfigure default
 	httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
 	cfg.max_uri_handlers = 8;
 	cfg.stack_size = 8192;

	// Start HTTP
 	httpd_handle_t srv = NULL;
 	if (httpd_start(&srv, &cfg) != ESP_OK) {
 	 	return NULL;
 	}

 	// UI
 	httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get};
 	httpd_register_uri_handler(srv, &root);

 	// API
 	httpd_uri_t list = {.uri="/api/scripts", .method=HTTP_GET, .handler=scripts_list_get};
 	httpd_register_uri_handler(srv, &list);

 	httpd_uri_t g1 = {.uri="/api/script", .method=HTTP_GET, .handler=script_one_get};
 	httpd_register_uri_handler(srv, &g1);

 	httpd_uri_t p1 = {.uri="/api/script", .method=HTTP_POST, .handler=script_one_post};
 	httpd_register_uri_handler(srv, &p1);

    httpd_uri_t d1 = {.uri="/api/script", .method=HTTP_DELETE, .handler=script_one_delete};
    httpd_register_uri_handler(srv, &d1);

 	return srv;
}

/* ========== Portal management ========== */

// Start the SoftAP and the web portal
esp_err_t bluetooth_web_portal_start(void)
{
 	// If already running, do nothing
 	if (s_server != NULL) {
 	 	#ifdef POLYCAST5_DEBUG
 	 	ESP_LOGW(TAG, "Portal already running at http://%s", s_ip);
 	 	#endif
 	 	
 	 	return ESP_OK;
 	}

 	// Create default AP netif if needed
 	if (s_ap_netif == NULL) {
 	 	s_ap_netif = esp_netif_create_default_wifi_ap();
 	 	if (s_ap_netif == NULL) {
 	 	 	return ESP_FAIL;
 	 	}
 	}

 	// Init Wi-Fi (tolerate "already init" state)
 	wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
 	esp_err_t err = esp_wifi_init(&wcfg);
 	if ((err != ESP_OK) && (err != ESP_ERR_WIFI_INIT_STATE)) {
 	 	#ifdef POLYCAST5_DEBUG
 	 	ESP_LOGW(TAG, "esp_wifi_init error: %s", esp_err_to_name(err));
 	 	#endif
 	 	
 	 	return err;
 	}

 	// Keep config in RAM so nothing persists accidentally.
 	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

 	// Configure SoftAP using project macros for SSID/PASS
 	wifi_config_t ap = {0};
 	strcpy((char *)ap.ap.ssid, PORTAL_SSID);
 	ap.ap.ssid_len = strlen(PORTAL_SSID);
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
 	if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
 	 	snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
 	}

 	// Bring up HTTP server and register endpoints.
 	s_server = start_http();
 	if (s_server == NULL) {
 	 	#ifdef POLYCAST5_DEBUG
 	 	ESP_LOGE(TAG, "start_http failed");
 	 	#endif
 	 	
 	 	return ESP_FAIL;
 	}

 	#ifdef POLYCAST5_DEBUG
 	ESP_LOGI(TAG, "Portal running at http://%s (SSID: " PORTAL_SSID ")", s_ip);
 	#endif
 	
 	return ESP_OK;
}

// Stop the web server and SoftAP
void bluetooth_web_portal_stop(void)
{
 	if (s_server != NULL) {
 	 	httpd_stop(s_server);
 	 	s_server = NULL;
 	}
 	
 	// Leave the netif allocated for now
 	(void)esp_wifi_stop();
}

// Return the AP IP string for on-screen instructions
const char *bluetooth_web_portal_get_ip(void)
{
 	return s_ip;
}

esp_err_t bluetooth_wifi_pass_save_nvs(const char *val)
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

esp_err_t bluetooth_wifi_pass_load_nvs(char *out, size_t out_sz)
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