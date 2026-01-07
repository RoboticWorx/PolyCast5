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

// All indexed by the same global script index: BT_SCRIPT_KEY_FMT, BT_SCRIPT_CAT_KEY_FMT, BT_SCRIPT_MENU_KEY_FMT
#define BT_SCRIPT_NS "bt_portal"
#define BT_SCRIPT_KEY_FMT "script_%02d"
#define BT_SCRIPT_CAT_KEY_FMT "cat_%02d"

#define BT_SCRIPT_MENU_NS "keyb_menu"
#define BT_SCRIPT_MENU_KEY_COUNT "count"
#define BT_SCRIPT_MENU_KEY_FMT "item_%02d"

#define MAX_HTTP_BODY_TXT 2048

extern char bt_wifi_portal_pass[];

static httpd_handle_t bt_server = NULL;
static esp_netif_t *bt_ap_netif = NULL;
static char s_ip[16] = "192.168.4.1";

// Web page HTML (UI: pick index, name, and payload)
static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>PolyCast5 BT Portal</title>"
"<style>body{font-family:system-ui,Arial,sans-serif;margin:16px}label{display:block;margin:8px 0 4px}"
"input,textarea,select{width:100%;box-sizing:border-box}textarea{height:180px}</style>"
"</head><body>"
"<h2>PolyCast5 BT Portal</h2>"
"<hr><h3>Manage Categories</h3>"
"<p>Bluetooth scripts are organized into categories to help make finding them easier.</p>"
"<p>For example, if I want all my passwords in one place, I'd create a category probably named 'Passwords' to save them under.</p>"
"<p>Category indexes are 0-based, so the first one you add will be index 0, then 1, etc.</p>"
"<p>A tutorial is also available here: https://polycast5.com/blogs/docs/using-the-bluetooth-auto-keyboard/</p>"
"<div><label>Category index</label><input id=cat_idx type=number min=0 value=0> <button id=cat_load>Load</button></div>"
"<div><label>Category Name</label><input id=cat_name maxlength=32 placeholder='Group name'></div>"
"<div><button id=cat_save>Save (add/edit)</button> <button id=cat_del>Delete</button> <span id=cat_msg></span></div>"
"<h4>Existing Categories</h4><select id=cat_list size=6 style='height:160px'></select>"

"<hr><h3>Edit Script (by category)</h3>"
"<p>Pick a category below to save specific scripts to.</p>"
"<p>For example, if I'm saving a password to my 'Passwords' category, I'd pick that one.</p>"
"<p>Script indexes are also 0-based, so the first one you add for a given category will be index 0, then 1, etc.</p>"
"<div><label>Category</label><select id=cat></select></div>"
"<div><label>Script index (local)</label><input id=idx type=number min=0 max=15 value=0> <button id=load>Load</button></div>"
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
"<br>&lt;down:c&gt; - Hold c down until &lt;up:c&gt; is called"
"<br>&lt;up:c&gt; - Release c if &lt;down:c&gt; was called"
"<br><br>You can also combine commands like &lt;ctrl+shift+v&gt; or &lt;ctrl+c&gt;."
"<br><br>Example: &lt;win+s&gt;&lt;delay=500&gt;browser&lt;enter&gt;&lt;delay=500&gt;https://youtu.be/dQw4w9WgXcQ&lt;enter&gt;"
"<br><br>More examples: github.com/RoboticWorx/PolyCast5/tree/main/scripts/bluetooth_examples"
"</p>"

"<hr><h3>Existing Scripts (global order)</h3>"
"<p>Selecting here loads by global index (you will need to adjust the local index). Fields will show that script's category.</p>"
"<select id=list size=6 style='height:160px'></select>"

"<script>"
"function $(id){return document.getElementById(id);}"

// ---------- Safe Refreshers ----------
"async function refreshList(){try{let r=await fetch('/api/scripts');if(!r.ok)return;let j=await r.json();let s=$('list');if(!s)return;s.innerHTML='';for(let i=0;i<j.count;i++){let o=document.createElement('option');o.value=i;o.textContent=`${i}: ${j.labels[i]||'(unnamed)'}`;s.appendChild(o);}}catch(e){console.error(e);}}"

"async function refreshCatList(){try{let r=await fetch('/api/categories');if(!r.ok)return;let j=await r.json();let cl=$('cat_list');if(cl)cl.innerHTML='';let cs=$('cat');if(cs)cs.innerHTML='';for(let i=0;i<j.count;i++){let n=j.names[i]||'(unnamed)';if(cl){let o=document.createElement('option');o.value=i;o.textContent=`${i}: ${n}`;cl.appendChild(o);}if(cs){let o2=document.createElement('option');o2.value=i;o2.textContent=n;cs.appendChild(o2);}}}catch(e){console.error(e);}}"

// ---------- Loaders ----------
"async function loadOneGlobal(gidx){try{let r=await fetch('/api/script?index='+gidx);if(!r.ok){let m=$('msg');if(m)m.textContent='Not found';return;}let j=await r.json();if($('cat'))$('cat').value=String(j.cat||0);if($('idx'))$('idx').value=j.index;if($('name'))$('name').value=j.name||'';if($('body'))$('body').value=j.body||'';let m=$('msg');if(m)m.textContent='';}catch(e){console.error(e);}}"

"async function loadOneLocal(lidx,cat){try{let r=await fetch('/api/script?index='+lidx+'&cat='+cat);if(!r.ok){let m=$('msg');if(m)m.textContent='Not found';return;}let j=await r.json();if($('cat'))$('cat').value=String(cat);if($('idx'))$('idx').value=lidx;if($('name'))$('name').value=j.name||'';if($('body'))$('body').value=j.body||'';let m=$('msg');if(m)m.textContent='';}catch(e){console.error(e);}}"

"async function loadCat(i){try{let r=await fetch('/api/category?index='+i);if(!r.ok){let m=$('cat_msg');if(m)m.textContent='Not found';return;}let j=await r.json();if($('cat_idx'))$('cat_idx').value=j.index;if($('cat_name'))$('cat_name').value=j.name||'';let m=$('cat_msg');if(m)m.textContent='';}catch(e){console.error(e);}}"

// ---------- Event wiring (with guards) ----------
"window.addEventListener('load',()=>{"
" try{let b=$('load');if(b)b.addEventListener('click',()=>{let c=parseInt(($('cat')||{value:'0'}).value);if(isNaN(c))c=0;let i=parseInt(($('idx')||{value:'0'}).value);if(isNaN(i))i=0;loadOneLocal(i,c);});}catch(e){console.error(e);} "
" try{let l=$('list');if(l)l.addEventListener('change',(e)=>{let g=parseInt(e.target.value||'0');if(isNaN(g))g=0;loadOneGlobal(g);});}catch(e){console.error(e);} "
" try{let s=$('save');if(s)s.addEventListener('click',async()=>{let c=parseInt(($('cat')||{value:'0'}).value);if(isNaN(c))c=0;let i=parseInt(($('idx')||{value:'0'}).value);if(isNaN(i))i=0;let data={index:i,cat:c,name:($('name')||{value:''}).value,body:($('body')||{value:''}).value};try{let r=await fetch('/api/script',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});let m=$('msg');if(r.ok){if(m)m.textContent='Saved';refreshList();}else{if(m)m.textContent='Error';}}catch(err){console.error(err);}});}catch(e){console.error(e);} "
" try{let d=$('del');if(d)d.addEventListener('click',async()=>{let c=parseInt(($('cat')||{value:'0'}).value);if(isNaN(c))c=0;let i=parseInt(($('idx')||{value:'0'}).value);if(isNaN(i))i=0;try{let r=await fetch('/api/script?index='+i+'&cat='+c,{method:'DELETE'});let m=$('msg');if(r.ok){if(m)m.textContent='Deleted';refreshList();}else{if(m)m.textContent='Error';}}catch(err){console.error(err);}});}catch(e){console.error(e);} "
" try{let cl=$('cat_load');if(cl)cl.addEventListener('click',()=>{let i=parseInt(($('cat_idx')||{value:'0'}).value);if(isNaN(i))i=0;loadCat(i);});}catch(e){console.error(e);} "
" try{let clst=$('cat_list');if(clst)clst.addEventListener('change',(e)=>{let i=parseInt(e.target.value||'0');if(isNaN(i))i=0;loadCat(i);});}catch(e){console.error(e);} "
" try{let cs=$('cat_save');if(cs)cs.addEventListener('click',async()=>{let data={index:parseInt(($('cat_idx')||{value:'0'}).value),name:($('cat_name')||{value:''}).value};try{let r=await fetch('/api/category',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});let m=$('cat_msg');if(r.ok){if(m)m.textContent='Saved';refreshCatList();}else{if(m)m.textContent='Error';}}catch(err){console.error(err);}});}catch(e){console.error(e);} "
" try{let cd=$('cat_del');if(cd)cd.addEventListener('click',async()=>{let i=parseInt(($('cat_idx')||{value:'0'}).value);if(isNaN(i))i=0;try{let r=await fetch('/api/category?index='+i,{method:'DELETE'});let m=$('cat_msg');if(r.ok){if(m)m.textContent='Deleted';if($('cat_name'))$('cat_name').value='';refreshCatList();}else{if(m)m.textContent='Error';}}catch(err){console.error(err);}});}catch(e){console.error(e);} "
" try{refreshCatList();refreshList();}catch(e){console.error(e);} "
"});"
"</script>"

"</body></html>";

/* =============== NVS =============== */

// Write a payload body for the given script index
static esp_err_t bluetooth_script_body_set_nvs(uint8_t idx, const char *body)
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
     } else {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "bluetooth_script_body_set nvs_set_str failed: %s", esp_err_to_name(err));
        #endif
    }
    
    // Close NVS
     nvs_close(h);
     return err;
}

// Persist the count of user scripts
static esp_err_t bluetooth_script_count_set_nvs(uint8_t count)
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
     } else {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "bluetooth_script_count_set nvs_set_u8 failed: %s", esp_err_to_name(err));
        #endif
    }
    
    // Close NVS
     nvs_close(h);
     return err;
}

// Write a label for the given script index
static esp_err_t bluetooth_script_label_set_nvs(uint8_t idx, const char *label)
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
uint8_t bluetooth_script_count_get_nvs(void)
{
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
     } else {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "bluetooth_script_count_get nvs_open failed: %s", esp_err_to_name(err));
        #endif
    }
    
     if (count > MAX_KEYBOARD_SCRIPTS) {
        count = MAX_KEYBOARD_SCRIPTS;
        ESP_LOGW(TAG, "bluetooth_script_count_get MAX_KEYBOARD_SCRIPTS reached: %d", MAX_KEYBOARD_SCRIPTS);
    }
    
    return count;
}

// Read a script label into caller buffer (buflen should be >= BLUETOOTH_SCRIPT_LABEL_MAX_LEN + 1)
esp_err_t bluetooth_script_label_get_nvs(uint8_t idx, char *buf, size_t buflen)
{
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

     return err;
}

// Read a payload body into caller buffer (need must fit into buflen)
esp_err_t bluetooth_script_body_get_nvs(uint8_t idx, char *buf, size_t buflen, size_t *outlen)
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
     } else {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "bluetooth_script_body_get string parameters wrong or NVS failed: %s", esp_err_to_name(err));
        #endif
    }
    
    // Close NVS
     nvs_close(h);
     return err;
}

uint8_t bluetooth_category_count_get_nvs(void)
{
    nvs_handle_t h;
    uint8_t count = 0;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        // Get count
        if (nvs_get_u8(h, "cat_count", &count) != ESP_OK) {
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
esp_err_t bluetooth_script_cat_idx_set_nvs(uint8_t idx, uint8_t cat)
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

esp_err_t bluetooth_script_cat_idx_get_nvs(uint8_t idx, uint8_t *cat)
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

esp_err_t bluetooth_category_name_get_nvs(uint8_t idx, char *buf, size_t buflen)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    char key[16];
    snprintf(key, sizeof(key), "cat_name_%02d", idx);
    
    size_t len = buflen;
    err = nvs_get_str(h, key, buf, &len);
    
    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_category_set_nvs(uint8_t idx, const char *name)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Get current count
    uint8_t count = 0;
    nvs_get_u8(h, "cat_count", &count);
    
    // If idx >= count, it's an add: update count
    if (idx >= count) {
        count = idx + 1;
        err = nvs_set_u8(h, "cat_count", count);
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    }
    
    // Set name
    char key[16];
    snprintf(key, sizeof(key), "cat_name_%02d", idx);
    err = nvs_set_str(h, key, name);
    
    // Commit if success
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t bluetooth_category_delete_nvs(uint8_t idx)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(BT_SCRIPT_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    
    // Get current count
    uint8_t count = 0;
    nvs_get_u8(h, "cat_count", &count);
    if (idx >= count) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Delete the category name
    char key[16];
    snprintf(key, sizeof(key), "cat_name_%02d", idx);
    nvs_erase_key(h, key);
    
    // Shift higher categories down
    for (uint8_t i = idx + 1; i < count; i++) {
        char old_key[16];
        snprintf(old_key, sizeof(old_key), "cat_name_%02d", i);
        
        size_t len = BT_CAT_LABEL_MAX_LEN + 1;
        char buf[BT_CAT_LABEL_MAX_LEN + 1];
        if (nvs_get_str(h, old_key, buf, &len) == ESP_OK) {
            snprintf(key, sizeof(key), "cat_name_%02d", i - 1);
            nvs_set_str(h, key, buf);
            nvs_erase_key(h, old_key);
        }
    }
    
    // Decrement count
    count--;
    nvs_set_u8(h, "cat_count", count);
    
    // Update all script categories: decrement if > idx
    uint8_t script_count = bluetooth_script_count_get_nvs();
    for (uint8_t s = 0; s < script_count; s++) {
        char cat_key[16];
        snprintf(cat_key, sizeof(cat_key), BT_SCRIPT_CAT_KEY_FMT, s);
        uint8_t cat;

        if (nvs_get_u8(h, cat_key, &cat) == ESP_OK) {
            if (cat > idx) {
                cat--;
                nvs_set_u8(h, cat_key, cat);
            } else if (cat == idx) {
                // Optional: set to 0 (default cat) or delete script? Here, set to 0.
                nvs_set_u8(h, cat_key, 0);
            }
        }
    }
    
    // Commit
    err = nvs_commit(h);
    
    // Close NVS
    nvs_close(h);
    return err;
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

/* =============== HTTP handlers =============== */

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
     uint8_t count = bluetooth_script_count_get_nvs();

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
          if ((bluetooth_script_label_get_nvs(i, lbl, sizeof(lbl)) == ESP_OK) && (lbl[0] != '\0')) {
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
static bool resolve_global_index_for_local(uint8_t cat, uint8_t local_index, bool create, uint8_t *out_global)
{
    // Get total script count
    uint8_t total = bluetooth_script_count_get_nvs();
    
    // Count how many scripts belong to 'cat' and remember their global positions in order.
    uint8_t seen = 0;
    for (uint8_t i = 0; i < total; i++) {
        uint8_t c = 0;
        (void)bluetooth_script_cat_idx_get_nvs(i, &c);

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
    if (create && local_index == seen) {
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
    uint8_t global_idx = 0;
    if (has_cat) {
        // Interpret index as local within category
        uint8_t local_idx = (uint8_t)atoi(idx_str);
        uint8_t cat = (uint8_t)atoi(cat_str);

        // Resolve (no create)
        if (!resolve_global_index_for_local(cat, local_idx, false, &global_idx)) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        }
    } else {
        // Treat as global directly
        global_idx = (uint8_t)atoi(idx_str);

        // Validate range
        if (global_idx >= bluetooth_script_count_get_nvs()) {
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
    bluetooth_script_label_get_nvs(global_idx, name, sizeof(name));
    bluetooth_script_body_get_nvs(global_idx, body, MAX_HTTP_BODY_TXT + 1, &blen);
    bluetooth_script_cat_idx_get_nvs(global_idx, &cat);

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

    // Pull values
    uint8_t idx_local_or_global = (uint8_t)jidx->valueint;
    const char *name_in = jname->valuestring;
    const char *body_in = jbody->valuestring;

    // Parse/Default category
    uint8_t cat = 0;
    bool has_cat = false;
    if (cJSON_IsNumber(jcat)) {
        cat = (uint8_t)jcat->valueint;
        has_cat = true;
    }

    // Resolve to GLOBAL index
    uint8_t global_idx = idx_local_or_global;
    if (has_cat) {
        // Resolve local -> global (allow create if local==tail)
        if (!resolve_global_index_for_local(cat, idx_local_or_global, true, &global_idx)) {
            cJSON_Delete(j);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index/cat");
        }
    } else {
        // Treat as global (edit or append at tail)
        uint8_t total = bluetooth_script_count_get_nvs();

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

    // If appending, bump count first
    uint8_t count = bluetooth_script_count_get_nvs();
    if (global_idx >= count) {
        esp_err_t ecount = bluetooth_script_count_set_nvs((uint8_t)(global_idx + 1));

        if (ecount != ESP_OK) {
            cJSON_Delete(j);
            free(buf);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs-count");
        }
    }

    // Persist label
    esp_err_t err = bluetooth_script_label_set_nvs(global_idx, label);

    // Persist body
    if (err == ESP_OK) {
        err = bluetooth_script_body_set_nvs(global_idx, body_in);
    }

    // Persist category only if supplied (keep old if not)
    if (err == ESP_OK && has_cat) {
        err = bluetooth_script_cat_idx_set_nvs(global_idx, cat);
    }

    // If any write failed, report
    if (err != ESP_OK) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
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
    uint8_t global_idx = 0;
    if (has_cat) {
        uint8_t local_idx = (uint8_t)atoi(idx_str);
        uint8_t cat = (uint8_t)atoi(cat_str);

        // Resolve local -> global (no create on delete)
        if (!resolve_global_index_for_local(cat, local_idx, false, &global_idx)) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        }
    } else {
        global_idx = (uint8_t)atoi(idx_str);
    }

    // Validate range
    uint8_t count = bluetooth_script_count_get_nvs();
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

    for (uint8_t i = global_idx; i + 1 < count; i++) {
        // Read from next slot
        next_label[0] = '\0';
        blen = 0;
        next_cat = 0;

        bluetooth_script_label_get_nvs(i + 1, next_label, sizeof(next_label));
        bluetooth_script_body_get_nvs(i + 1, next_body, MAX_HTTP_BODY_TXT + 1, &blen);
        bluetooth_script_cat_idx_get_nvs(i + 1, &next_cat);

        // Write into current slot
        bluetooth_script_label_set_nvs(i, next_label);
        bluetooth_script_body_set_nvs(i, (blen > 0) ? next_body : "");
        bluetooth_script_cat_idx_set_nvs(i, next_cat);
    }

    // Clear old tail
    bluetooth_script_label_set_nvs(count - 1, "");
    bluetooth_script_body_set_nvs(count - 1, "");
    bluetooth_script_cat_idx_set_nvs(count - 1, 0);

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
    uint8_t count = bluetooth_category_count_get_nvs();
    cJSON_AddNumberToObject(root, "count", count);

    // Names array
    cJSON *names = cJSON_AddArrayToObject(root, "names");
    if (names == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    // Populate names
    for (uint8_t i = 0; i < count; i++) {
        char buf[BT_CAT_LABEL_MAX_LEN + 1];

        if (bluetooth_category_name_get_nvs(i, buf, sizeof(buf)) == ESP_OK) {
            cJSON_AddItemToArray(names, cJSON_CreateString(buf));
        } else {
            cJSON_AddItemToArray(names, cJSON_CreateString("(unnamed)"));
        }
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
    if (bluetooth_category_name_get_nvs((uint8_t)index, buf, sizeof(buf)) == ESP_OK) {
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
    if (index < 0 || name == NULL) {
        cJSON_Delete(j);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fields");
    }

    // Save category
    esp_err_t err = bluetooth_category_set_nvs((uint8_t)index, name);

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
    esp_err_t err = bluetooth_category_delete_nvs((uint8_t)index);

    // If write failed, report
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs");
    }

    // Respond
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

    return srv;
}

/* ========== Portal management ========== */

// Start the SoftAP and the web portal
esp_err_t bluetooth_web_portal_start(void)
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
esp_err_t bluetooth_web_portal_stop(void)
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
const char *bluetooth_web_portal_get_ip(void)
{
     return s_ip;
}