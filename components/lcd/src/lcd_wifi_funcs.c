#include "ota_update.h"
#include "polycast5_macros.h"

#include "freertos/idf_additions.h"
#include "portmacro.h"

#include "core/lv_obj_pos.h"
#include "core/lv_obj_tree.h"
#include "core/lv_obj.h"
#include "misc/lv_area.h"
#include "misc/lv_color.h"

#include "widgets/chart/lv_chart_private.h"
#include "widgets/label/lv_label.h"

#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"

#include "lcd_wifi_funcs.h"
#include "wifi_task.h"
#include "wifi_funcs.h"
#include "espnow_task.h"
#include "espnow_funcs.h"
#include "gpio_task.h"
#include "lcd_utils.h"
#include "ai_task.h"
#include "ai_funcs.h"
#include "ai_analysis_web_portal.h"

// Wi-Fi menu options
#define WIFI_MENU_NS "wifi_menu" // NVS namespace for menu entries
#define WIFI_MENU_KEY_COUNT "count" // u8: number of menu items
#define WIFI_MENU_KEY_FMT "item_%02d" // e.g. "menu_00", "menu_01", ...

// Wi-Fi topics
#define WIFI_TOPIC_NS "wifi_topics" // NVS namespace for topic list
#define WIFI_TOPIC_KEY_COUNT "count" // u8: number of topics
#define WIFI_TOPIC_KEY_FMT "topic_%02d" // e.g. "topic_00", "topic_01", ...

#define MAX_PASSWORD_LEN 32
#define NUM_CHAR_ROWS 4

#define WIFI_MENU_START_SIZE 4 // First 4 default options

#define MQTT_READY_TXT "0 = OFF        1 = ON\n  255 = UPDATE" // 'UPDATE' refers to checking and performing an OTA firmware update if available
#define MQTT_SENDING_TXT "Sending via\nMQTT broker..." 
#define MQTT_CONNECTING_TXT "Please wait...\nConnecting..."

// wifi_funcs.c
extern char raw_frames_hex_buf[]; // Accumulated hex strings
extern size_t raw_frames_hex_len; // Current length
extern uint32_t raw_frames_captured; // Counter

// gpio_task.c
extern volatile bool gpio_select_btn_held;

wifi_menu_t wifi_menu = {
    .options = {"Connect to Network", "Monitor Packets", "AI Packet Analysis", "Sync With PolyPlug"},
    .size = WIFI_MENU_START_SIZE,
    .index = 0,
    .cont = NULL,
};

wifi_login_t selected_network = {0};

bool monitoring_packets = false;

static const char* TAG = "LCD_WIFI_FUNCS";

static char *raw_frames_ai_response;

static wifi_sniff_t sniff_network;

static uint8_t mqtt_key[16];
static bool wifi_menu_overwrite = false;

// Character vars for user input
static char mqtt_name_buf[MAX_PASSWORD_LEN + 1] = {0};
static const char* char_rows[NUM_CHAR_ROWS] = {
    "_ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "abcdefghijklmnopqrstuvwxyz",
    "0123456789",
    "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~"
};

void lcd_wifi_setup_page(wifi_menu_t *menu)
{
    // Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 210, 106);
    
    // Format
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lcd_apply_scrollbar_style(menu->main_list);
    lv_obj_set_scroll_dir(menu->main_list, LV_DIR_VER);

    // Create button style
    lv_style_init(&menu->btn_style);
    
    lv_style_set_radius(&menu->btn_style, 8);
    lv_style_set_bg_color(&menu->btn_style, user_primary_color);
    
    lv_style_set_border_width(&menu->btn_style, 2);
    lv_style_set_border_color(&menu->btn_style, user_secondary_color);
    lv_style_set_border_side(&menu->btn_style, LV_BORDER_SIDE_FULL);
    
    lv_style_set_pad_top(&menu->btn_style, 3);
    lv_style_set_pad_bottom(&menu->btn_style, 3);
    
    lv_style_set_text_font(&menu->btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->btn_style, user_secondary_color);
    lv_style_set_text_align(&menu->btn_style, LV_TEXT_ALIGN_CENTER);
    
    // Create selected button style
    lv_style_init(&menu->sel_style);
    
    lv_style_set_radius(&menu->sel_style, 8);
    lv_style_set_bg_color(&menu->sel_style, user_secondary_color);
    
    lv_style_set_border_width(&menu->sel_style, 2);
    lv_style_set_border_color(&menu->sel_style, user_secondary_color);
    lv_style_set_border_side(&menu->sel_style, LV_BORDER_SIDE_FULL);
    
    lv_style_set_pad_top(&menu->sel_style, 3);
    lv_style_set_pad_bottom(&menu->sel_style, 3);
    
    lv_style_set_text_font(&menu->sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->sel_style, user_primary_color);
    lv_style_set_text_align(&menu->sel_style, LV_TEXT_ALIGN_CENTER);
    
    
    // Create buttons
    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }
    
    // Create button for each option
    for (int i = 0; i < menu->size; ++i) {

        menu->btns[i] = lv_list_add_btn(menu->main_list, NULL, menu->options[i]);
        lv_obj_set_size(menu->btns[i], 200, 30);

        // Style selected
        if (i == menu->index) {
            lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
        } else {
            lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
        }

        // Create and format text label
        lv_obj_t *lbl = lv_obj_get_child(menu->btns[i], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }

    // Format buttons as container
    menu->cont = lv_obj_get_parent(menu->btns[0]);
    lv_obj_set_flex_flow(menu->cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(menu->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
    
    // Hide for now
    lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_wifi_update_menu(wifi_menu_t *menu)
{    
    // Reveal
    lv_obj_remove_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);

    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }

    // Reset every button to unselected
    for (int i = 0; i < menu->size; ++i) {
        lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
        lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);
    
    // Enable scrolling if list gets too long
    lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
}

void lcd_wifi_create_scan_list(wifi_scan_menu_t *menu)
{
    menu->size = 0;
    
    // Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 210, 106); // h: 68
    
    // Format
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0); // y: 17
    lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lcd_apply_scrollbar_style(menu->main_list);
    lv_obj_set_scroll_dir(menu->main_list, LV_DIR_VER);

    // Create button style
    lv_style_init(&menu->btn_style);
    
    lv_style_set_radius(&menu->btn_style, 8);
    lv_style_set_bg_color(&menu->btn_style, user_primary_color);
    
    lv_style_set_border_width(&menu->btn_style, 2);
    lv_style_set_border_color(&menu->btn_style, user_secondary_color);
    lv_style_set_border_side(&menu->btn_style, LV_BORDER_SIDE_FULL);
    
    lv_style_set_pad_top(&menu->btn_style, 3);
    lv_style_set_pad_bottom(&menu->btn_style, 3);
    
    lv_style_set_text_font(&menu->btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->btn_style, user_secondary_color);
    lv_style_set_text_align(&menu->btn_style, LV_TEXT_ALIGN_CENTER);
    
    // Create selected button style
    lv_style_init(&menu->sel_style);
    
    lv_style_set_radius(&menu->sel_style, 8);
    lv_style_set_bg_color(&menu->sel_style, user_secondary_color);
    
    lv_style_set_border_width(&menu->sel_style, 2);
    lv_style_set_border_color(&menu->sel_style, user_secondary_color);
    lv_style_set_border_side(&menu->sel_style, LV_BORDER_SIDE_FULL);
    
    lv_style_set_pad_top(&menu->sel_style, 3);
    lv_style_set_pad_bottom(&menu->sel_style, 3);
    
    lv_style_set_text_font(&menu->sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->sel_style, user_primary_color);
    lv_style_set_text_align(&menu->sel_style, LV_TEXT_ALIGN_CENTER);
    
    
    // Create buttons
    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }
    
    if (menu->size > 0) {
        menu->index = 0;
    }
    
    // Hide for now
    lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

static void lcd_wifi_update_scan_menu(wifi_scan_menu_t *menu)
{    
    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }

    // Reset every button to unselected
    for (int i = 0; i < menu->size; ++i) {
        lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
        lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);
    
    // Enable scrolling if list gets too long
    lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
}

void lcd_wifi_scan_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    static lv_obj_t *lbl_wait;
    static lv_obj_t *lbl_option;
    static bool locked[WIFI_MAX_NETWORKS];
    static bool initialized = false;
    static bool scanned = false;
    static bool scanning = false;
    
    static uint8_t bssids[WIFI_MAX_NETWORKS][6];
    static uint8_t channels[WIFI_MAX_NETWORKS];
    
    // Do once
    if (!initialized) {            
        if (!monitoring_packets) {             
            // Option label
            lbl_option = lv_label_create(ACTIVE_SCR);
            lcd_format_label(lbl_option, "or press down to scan", user_secondary_color,
                    &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -10);
                             
        
            /* Add prev button */
            char buf[16];
            snprintf(buf, sizeof(buf), "Connect to Last");
            
            // Create button
            wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size] = lv_list_add_btn(wifi_menu->scan_menu.main_list, NULL, buf);
            lv_obj_set_size(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 200, 30);
    
            // Style selected
            lv_obj_add_style(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], &wifi_menu->scan_menu.sel_style, 0);
    
            // Create and format text label
            lv_obj_t *lbl = lv_obj_get_child(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
            
            // Format buttons as container
            wifi_menu->scan_menu.cont = lv_obj_get_parent(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size]);
            lv_obj_set_flex_flow(wifi_menu->scan_menu.cont, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(wifi_menu->scan_menu.cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(wifi_menu->scan_menu.cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
            
            wifi_menu->scan_menu.size++; // One larger
            
            lv_timer_handler(); // Show
        } else {
            // Disconnect if connected
            xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
            
            lbl_wait = lv_label_create(ACTIVE_SCR);
            lcd_format_label(lbl_wait, "Scanning for networks...\nPlease wait, then select\na network to monitor.", user_secondary_color,
                    &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 0);
                                                  
            lv_timer_handler(); // Show
            
            // Start scan
            xEventGroupSetBits(xWifiEventGroup, WIFI_SCAN_NETWORKS_BIT);
            
            scanning = true;
        }
        
        initialized = true;
    }

    // When networks have been scanned
    wifi_scan_t result;
    while (xQueueReceive(xWifiScanQueue, &result, 0) == pdPASS) {
        // If no networks found
        if (result.auth == 0xFF) { // (Impossible auth type)
            #ifdef POLYCAST5_DEBUG
            // This needs to be tested as an edge case!
            ESP_LOGW(TAG, "xWifiScanQueue: Received impossible auth type (%d): TEST EDGE CASE.", result.auth);
            #endif

            // Done scanning
            scanning = false;
            scanned = false;

            lv_label_set_text(lbl_wait, "No networks found.\nPress LEFT to go back.");
            lv_obj_align(lbl_wait, LV_ALIGN_CENTER, 0, 20);
            
            // Don't create any buttons for this sentinel
            continue;
        }

        // Skip any blanks
        if ((result.ssid[0] == '\0') || (strlen((char*)result.ssid) == 0)) {
            continue;
        }
        
        // Once networks have been received: delete help text
        if (!scanned) {
            lv_obj_delete(lbl_wait);
            lbl_wait = NULL;
            scanning = false;
            scanned = true;
        }
        
        // Check if != WIFI_AUTH_OPEN
        locked[wifi_menu->scan_menu.size] = (result.auth != 0) ? true : false; 
        // Copy BSSID
        memcpy(bssids[wifi_menu->scan_menu.size], result.bssid, sizeof(result.bssid));
        // Copy channel
        channels[wifi_menu->scan_menu.size] = result.channel;
        
        // Format SSID
        char buf[33];
        snprintf(buf, sizeof(buf), "%s", result.ssid); //, result.rssi, result.channel, result.auth
        
        // Add SSID as button
        wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size] = lv_list_add_btn(wifi_menu->scan_menu.main_list, NULL, buf);
        lv_obj_set_size(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 200, 30);

        // Style selected
        if (wifi_menu->scan_menu.size == wifi_menu->scan_menu.index) {
            lv_obj_add_style(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], &wifi_menu->scan_menu.sel_style, 0);
        } else {
            lv_obj_add_style(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], &wifi_menu->scan_menu.btn_style, 0);
        }

        // Create and format text label
        lv_obj_t *lbl = lv_obj_get_child(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        
        // Format buttons as container
        wifi_menu->scan_menu.cont = lv_obj_get_parent(wifi_menu->scan_menu.btns[wifi_menu->scan_menu.size]);
        lv_obj_set_flex_flow(wifi_menu->scan_menu.cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(wifi_menu->scan_menu.cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(wifi_menu->scan_menu.cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
        
        // Size one bigger now
        wifi_menu->scan_menu.size++;
    }
    
    // Scroll up
    if (scanned && ui_btns->up_btn == 1) {
        wifi_menu->scan_menu.index--;
        lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
    } else if (!scanned && ui_btns->down_btn == 1 && !monitoring_packets) { // Scan requested - not monitoring
        if (lbl_option) { // Delete if exists
            lv_obj_delete(lbl_option);
            lbl_option = NULL;
        }
        
        if (!scanning) {
            // Wait label
            lbl_wait = lv_label_create(ACTIVE_SCR);
            lcd_format_label(lbl_wait, "Scanning for networks...", user_secondary_color,
                    &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -10);
            
            // Start scan
            xEventGroupSetBits(xWifiEventGroup, WIFI_SCAN_NETWORKS_BIT);
            
            scanning = true;
        }
    } else if (scanned && ui_btns->down_btn == 1) { // Scroll down
        wifi_menu->scan_menu.index++;
        lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
    }
    // Back
    else if ((!scanning || scanned) && ui_btns->left_btn == 1) {
        if (lbl_option) { // Delete if exists
            lv_obj_delete(lbl_option);
            lbl_option = NULL;
        }
        
        // Reset
        monitoring_packets = false;
        initialized = false;
        scanned = false;
        for (int i = 0; i < wifi_menu->scan_menu.size; ++i) {
            locked[i] = false;
            memset(bssids[i], 0, sizeof(bssids[i]));
            channels[i] = 0;
        }
        wifi_menu->scan_menu.size = 0;
        wifi_menu->scan_menu.index = 0;
        lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
        
        // Clear children
        lv_obj_clean(wifi_menu->scan_menu.main_list);
        
        // Hide scan menu
        lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_PAGE;
    }
    // Go home
    else if ((!scanning || scanned) && ui_btns->home_btn == 1) {
        if (lbl_option) { // Delete if exists
            lv_obj_delete(lbl_option);
            lbl_option = NULL;
        }
        
        // Reset
        monitoring_packets = false;
        initialized = false;
        scanned = false;
        for (int i = 0; i < wifi_menu->scan_menu.size; ++i) {
            locked[i] = false;
            memset(bssids[i], 0, sizeof(bssids[i]));
            channels[i] = 0;
        }
        wifi_menu->scan_menu.size = 0;
        wifi_menu->scan_menu.index = 0;
        lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
        
        // Clear children
        lv_obj_clean(wifi_menu->scan_menu.main_list);
        
        // Hide scan menu
        lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
        
        lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
    }
    // Power off
    else if ((!scanning || scanned) && ui_btns->pwr_btn == 1) {
        if (lbl_option) { // Delete if exists
            lv_obj_delete(lbl_option);
            lbl_option = NULL;
        }
        
        // Reset
        monitoring_packets = false;
        initialized = false;
        scanned = false;
        for (int i = 0; i < wifi_menu->scan_menu.size; ++i) {
            locked[i] = false;
            memset(bssids[i], 0, sizeof(bssids[i]));
            channels[i] = 0;
        }
        wifi_menu->scan_menu.size = 0;
        wifi_menu->scan_menu.index = 0;
        lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
        
        // Clear children
        lv_obj_clean(wifi_menu->scan_menu.main_list);
        
        // Hide scan menu
        lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
        
        lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
    } else if (ui_btns->select_btn == 1 && wifi_menu->scan_menu.index == 0 && !scanning && !monitoring_packets) { // If connecting to last known
        if (lbl_option) { // Delete if exists
            lv_obj_delete(lbl_option);
            lbl_option = NULL;
        }
        
        selected_network = wifi_funcs_get_prev(); // Loads boot state saved network info
        selected_network.prev = true; // Connecting to previous

        #ifdef POLYCAST5_CHECK_OTA_ON_CONN
        // Check for OTA on connect
        xEventGroupSetBits(xWifiEventGroup, WIFI_CHECK_OTA_ON_CONN_BIT);
        #endif
        
        if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed: xWifiSelectedNetworkQueue previous_network");
        }
        
        // Reset
        monitoring_packets = false;
        initialized = false;
        scanned = false;
        for (int i = 0; i < wifi_menu->scan_menu.size; ++i) {
            locked[i] = false;
            memset(bssids[i], 0, sizeof(bssids[i]));
            channels[i] = 0;
        }
        wifi_menu->scan_menu.size = 0;
        wifi_menu->scan_menu.index = 0;
        lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
            
        // Clear children
        lv_obj_clean(wifi_menu->scan_menu.main_list);
            
        // Hide scan menu
        lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
            
        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_PAGE;
    } else if (scanned && ui_btns->select_btn == 1) { // Network selected
        // Connecting to usual network
        if (!monitoring_packets) {
            selected_network.prev = false; // Connecting to new
            
            // Copy over SSID
            lv_obj_t *btn = wifi_menu->scan_menu.btns[wifi_menu->scan_menu.index];
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            const char *ssid = lv_label_get_text(lbl);
            strlcpy((char*)selected_network.ssid, ssid, sizeof(selected_network.ssid));
            
            // Copy BSSID
            memcpy(selected_network.bssid, bssids[wifi_menu->scan_menu.index], sizeof(bssids[wifi_menu->scan_menu.index]));
            
            // If network requires password
            if (locked[wifi_menu->scan_menu.index]) {
                // Reset
                monitoring_packets = false;
                initialized = false;
                scanned = false;
                for (int i = 0; i < wifi_menu->scan_menu.size; ++i) {
                    locked[i] = false;
                    memset(bssids[i], 0, sizeof(bssids[i]));
                    channels[i] = 0;
                }
                wifi_menu->scan_menu.size = 0;
                wifi_menu->scan_menu.index = 0;
                lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
                
                // Clear children
                lv_obj_clean(wifi_menu->scan_menu.main_list);
                
                // Hide scan menu
                lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
                
                // Switch pages
                ui_menu->page = WIFI_PASSWORD_PAGE;
            } else { // Else open network: go ahead and send
                selected_network.locked = false; // Doesn't require password
                
                if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) {
                    ESP_LOGE(TAG, "Failed: xWifiSelectedNetworkQueue SSID");
                }
                
                // Reset
                monitoring_packets = false;
                initialized = false;
                scanned = false;
                for (int i = 0; i < wifi_menu->scan_menu.size; ++i) {
                    locked[i] = false;
                    memset(bssids[i], 0, sizeof(bssids[i]));
                    channels[i] = 0;
                }
                wifi_menu->scan_menu.size = 0;
                wifi_menu->scan_menu.index = 0;
                lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
                
                // Clear children
                lv_obj_clean(wifi_menu->scan_menu.main_list);
                
                // Hide scan menu
                lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
                
                // Show Wi-Fi menu
                lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
                
                // Switch pages
                ui_menu->page = WIFI_PAGE;
            }
        } else { // Monitoring packets
            // Copy in data
            // Copy channel
            sniff_network.channel = channels[wifi_menu->scan_menu.index];
            // Copy BSSID
            memcpy(sniff_network.target_bssid, bssids[wifi_menu->scan_menu.index], sizeof(bssids[wifi_menu->scan_menu.index]));
            // Set mask
            sniff_network.mask = WIFI_PROMIS_FILTER_MASK_MGMT;
            
            if (xQueueSend(xWifiSniffQueue, &sniff_network, portMAX_DELAY) != pdPASS) {
                ESP_LOGE(TAG, "Failed: xWifiSniffQueue beacon");
            }
            
            // Reset
            monitoring_packets = false;
            initialized = false;
            scanned = false;
            for (int i = 0; i < wifi_menu->scan_menu.size; ++i) {
                locked[i] = false;
                memset(bssids[i], 0, sizeof(bssids[i]));
                channels[i] = 0;
            }
            wifi_menu->scan_menu.size = 0;
            wifi_menu->scan_menu.index = 0;
            lcd_wifi_update_scan_menu(&wifi_menu->scan_menu);
                
            // Clear children
            lv_obj_clean(wifi_menu->scan_menu.main_list);
                
            // Hide scan menu
            lv_obj_add_flag(wifi_menu->scan_menu.main_list, LV_OBJ_FLAG_HIDDEN);
            
            // Show right arrow
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
                
            // Switch pages
            ui_menu->page = WIFI_BEACON_PAGE;
        }
    }
}

// TODO: Make system prompt editable
// TODO: Bug: Can recapture after capture without going back to menu
// TODO: Nicen up menu and clean up cases
void lcd_wifi_ai_packet_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    #define WIFI_AI_PKT_CONN_FAILED_TXT "Connection failed!\nPlease connect to your\nWi-Fi network at least\nonce in the 'Wi-Fi'\nmenu and make sure\nyou are in range."
    #define WIFI_AI_PKT_CAPTURE_TXT "Captured %u/%u pkts"
    #define WIFI_AI_PKT_HOLD_TXT "  Hold select to\ncapture on Ch. %u"

    typedef enum {
        AI_PKT_IDLE = 0,
        AI_PKT_CAPTURING,
        AI_PKT_RECONNECT_WAIT,
        AI_PKT_SEND_AI,
        AI_PKT_ANALYSIS_COMPLETE,
    } ai_pkt_state_t;

    // Common primary channels (2.4GHz + 5GHz). Actual availability depends on regulatory domain.
    static const uint8_t wifi_ai_pkt_channels[] = {
        // 2.4 GHz
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        // 5 GHz (non-DFS + DFS; inclusion here does not guarantee allowed in region)
        36, 40, 44, 48,
        52, 56, 60, 64,
        100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
        149, 153, 157, 161, 165
    };

    static bool init = true;
    static lv_obj_t *lbl_ins = NULL;
    static uint8_t channel = 6; // Default channel
    static size_t channel_idx = 0;

    static ai_pkt_state_t state = AI_PKT_IDLE;
    static bool last_select = false;

    static uint32_t last_frames_shown = 0;

    static bool reconnect_sent = false;
    static TickType_t disconnect_tick = 0;
    static TickType_t reconnect_start_tick = 0;

    if (init) {
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);

        // Find default channel in the list
        for (size_t i = 0; i < (sizeof(wifi_ai_pkt_channels) / sizeof(wifi_ai_pkt_channels[0])); ++i) {
            if (wifi_ai_pkt_channels[i] == channel) {
                channel_idx = i; // Set channel index to default
                break;
            }
        }
        channel = wifi_ai_pkt_channels[channel_idx];

        lv_label_set_text_fmt(lbl_ins, WIFI_AI_PKT_HOLD_TXT, (unsigned)channel);

        state = AI_PKT_IDLE;
        last_select = false;
        last_frames_shown = 0;
        reconnect_sent = false;
        disconnect_tick = reconnect_start_tick = 0;

        init = false;
    }

    // Get physical held select button state
    bool select_now = gpio_select_btn_held;
    bool select_pressed = (select_now && !last_select);
    bool select_released = (!select_now && last_select);
    last_select = select_now;
    
    // Initial press
    if (state == AI_PKT_IDLE && select_pressed) {
        // Set mask to all: previous sniff_network is disregarded in wifi_funcs_init_promiscuous
        sniff_network.mask = WIFI_PROMIS_FILTER_MASK_RAW_USEFUL;
        sniff_network.channel = channel;

        // Start sniff
        if (xQueueSend(xWifiSniffQueue, &sniff_network, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "lcd_wifi_ai_packet_page: failed: xWifiSniffQueue data");
        }

        xSemaphoreTake(xWifiRawFramesMutex, portMAX_DELAY); // Lock raw sniffed frames
        raw_frames_hex_len = 0;
        raw_frames_captured = 0;
        raw_frames_hex_buf[0] = '\0';
        xSemaphoreGive(xWifiRawFramesMutex); // Release raw sniffed frames

        last_frames_shown = 0;
        
        char buf[64];
        lv_obj_set_style_text_font(lbl_ins, &lv_font_montserrat_18, 0);
        snprintf(buf, sizeof(buf), WIFI_AI_PKT_CAPTURE_TXT, (unsigned)0, (unsigned)WIFI_MAX_RAW_FRAMES);
        lv_label_set_text(lbl_ins, buf);
        lv_timer_handler(); // Update immediately

        state = AI_PKT_CAPTURING;
    }
    // While held: update count - stop only on release (or buffer full)
    if (state == AI_PKT_CAPTURING) {
        uint32_t frames = 0;
        size_t hex_len = 0;

        xSemaphoreTake(xWifiRawFramesMutex, portMAX_DELAY);
        frames = raw_frames_captured;
        hex_len = raw_frames_hex_len;
        xSemaphoreGive(xWifiRawFramesMutex);

        if (frames != last_frames_shown) {
            char buf[64];
            lv_obj_set_style_text_font(lbl_ins, &lv_font_montserrat_18, 0);
            snprintf(buf, sizeof(buf), WIFI_AI_PKT_CAPTURE_TXT, (unsigned)frames, (unsigned)WIFI_MAX_RAW_FRAMES);
            lv_label_set_text(lbl_ins, buf);
            last_frames_shown = frames;
        }

        bool full = false;
        if (frames >= WIFI_MAX_RAW_FRAMES) {
            full = true;
        }
        if (hex_len >= (AI_CMD_MAX_LEN - 64)) {
            full = true;
            #ifdef POLYCAST5_DEBUG
            ESP_LOGW(TAG, "lcd_wifi_ai_packet_page: Stopping sniff early: raw hex buffer full (len=%u cap=%u)",
                    (unsigned)hex_len, (unsigned)AI_CMD_MAX_LEN);
            #endif
        }

        // When done capturing
        if (select_released || full) {
            lv_obj_set_style_text_font(lbl_ins, &lv_font_montserrat_16, 0);
            lv_label_set_text(lbl_ins, "Connecting to Wi-Fi...");

            // Hide top and bottom arrows
            lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

            // Stop promiscuous/Wi-Fi before reconnecting
            xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);

            disconnect_tick = xTaskGetTickCount();
            reconnect_sent = false;
            state = AI_PKT_RECONNECT_WAIT;
        }
    }
    // Reconnect (non-blocking)
    if (state == AI_PKT_RECONNECT_WAIT) {
        if (!reconnect_sent) {
            // Small settle delay after disconnect
            if ((xTaskGetTickCount() - disconnect_tick) < pdMS_TO_TICKS(2000)) {
                // Do nothing for a bit (non-blocking delay)
            } else {
                selected_network = wifi_funcs_get_prev(); // Loads boot state saved network info
                selected_network.prev = true; // Connecting to previous

                if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) {
                    ESP_LOGE(TAG, "lcd_wifi_ai_packet_page: Failed: xWifiSelectedNetworkQueue previous_network");
                    state = AI_PKT_IDLE;
                }

                reconnect_start_tick = xTaskGetTickCount();
                reconnect_sent = true;
            }
        } else {
            // Wait up to 15 seconds for Wi-Fi connection
            if (xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT) {
                state = AI_PKT_SEND_AI;
            } else if ((xTaskGetTickCount() - reconnect_start_tick) >= pdMS_TO_TICKS(15000)) {
                ESP_LOGE(TAG, "lcd_wifi_ai_packet_page: Wi-Fi reconnect timeout after sniff");
                lv_obj_set_style_text_font(lbl_ins, &lv_font_montserrat_16, 0);
                lv_label_set_text(lbl_ins, WIFI_AI_PKT_CONN_FAILED_TXT);
                
                state = AI_PKT_IDLE;
            }
        }
    }
    // Send frames to Grok once connected
    if (state == AI_PKT_SEND_AI) {
        lv_obj_set_style_text_font(lbl_ins, &lv_font_montserrat_16, 0);
        lv_label_set_text(lbl_ins, "Analyzing captured\npackets with AI...\n\nPlease wait...");
        lv_timer_handler(); // Update immediately
        
        xSemaphoreTake(xWifiRawFramesMutex, portMAX_DELAY); // Lock raw sniffed frames

        size_t src_len = raw_frames_hex_len;
        size_t copy_len = MIN(src_len, (size_t)AI_CMD_MAX_LEN - 1);

        char *frames_copy = malloc(copy_len + 1);
        if (!frames_copy) {
            ESP_LOGE(TAG, "lcd_wifi_ai_packet_page: Raw frames malloc failed (len=%u)", (unsigned)copy_len);
            state = AI_PKT_IDLE;
        }

        memcpy(frames_copy, raw_frames_hex_buf, copy_len);
        frames_copy[copy_len] = '\0';

        xSemaphoreGive(xWifiRawFramesMutex); // Release raw sniffed frames

        if (src_len > copy_len) {
            ESP_LOGW(TAG, "lcd_wifi_ai_packet_page: Raw frames truncated for AI: src_len=%u cap=%u",
                    (unsigned)src_len, (unsigned)copy_len);
        }

        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Sending raw frames to Grok for analysis (len=%u)", (unsigned)copy_len);
        
        // This is usually a lot of data :)
        //ESP_LOGI(TAG, "Raw frames being sent:\n%s", frames_copy);
        #endif

        // Format AI cmd
        ai_cmd_t cmd = {
            .type = AI_CMD_RAW_FRAMES,
            .msg = frames_copy,
            .msg_len = copy_len,
            .free_ptr = frames_copy,
            .free_on_done = true,
            .reasoning = true, // Want accuracy
        };

        // Actually send it
        if (xQueueSend(xAiCmdQueue, &cmd, portMAX_DELAY) != pdPASS) {
            free(frames_copy);
            ESP_LOGE(TAG, "Failed: xAiCmdQueue raw frames");
            state = AI_PKT_IDLE;
        }

        // Switched to AI_PKT_ANALYSIS_COMPLETE in xQueueReceive xWifiAiRawSniffQueue
        state = AI_PKT_IDLE; // Idle for now
    }

    // When response received (non-blocking)
    if (xQueueReceive(xWifiAiRawSniffQueue, &raw_frames_ai_response, 0) == pdTRUE) {
        state = AI_PKT_ANALYSIS_COMPLETE;

        lv_obj_set_style_text_font(lbl_ins, &lv_font_montserrat_16, 0);
        lv_label_set_text(lbl_ins, "Analysis complete!\n\nPress RIGHT to\nsee the results.");
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Show right arrow
        lv_timer_handler(); // Update immediately
    }

    // Change channel up
    if (ui_btns->up_btn && state == AI_PKT_IDLE) {
        channel_idx++;
        if (channel_idx >= (sizeof(wifi_ai_pkt_channels) / sizeof(wifi_ai_pkt_channels[0]))) {
            channel_idx = 0;
        }

        channel = wifi_ai_pkt_channels[channel_idx];
        lv_label_set_text_fmt(lbl_ins, WIFI_AI_PKT_HOLD_TXT, (unsigned)channel);
    } else if (ui_btns->down_btn && state == AI_PKT_IDLE) { // Change channel down
        if (channel_idx == 0) {
            channel_idx = (sizeof(wifi_ai_pkt_channels) / sizeof(wifi_ai_pkt_channels[0])) - 1;
        } else {
            channel_idx--;
        }

        channel = wifi_ai_pkt_channels[channel_idx];
        lv_label_set_text_fmt(lbl_ins, WIFI_AI_PKT_HOLD_TXT, (unsigned)channel);
    } else if (ui_btns->right_btn && state == AI_PKT_ANALYSIS_COMPLETE) { // See results
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(lbl_ins);

        // Reset capture state
        state = AI_PKT_IDLE;
        last_select = false;
        reconnect_sent = false;

        // Reset statics
        init = true;
        lbl_ins = NULL;

        // Switch to results page
        ui_menu->page = WIFI_AI_PACKET_RESULTS_PAGE;
    } else if (ui_btns->left_btn) { // Go back
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Show top and bottom arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(lbl_ins);

        // Reset capture state
        state = AI_PKT_IDLE;
        last_select = false;
        reconnect_sent = false;

        // Reset statics
        init = true;
        lbl_ins = NULL;

        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = WIFI_PAGE;

        // Disable Wi-Fi
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
    }
}

void lcd_wifi_ai_packet_results_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    #define WIFI_AI_PKT_RESULTS_Y_OFFSET 40
    
    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;
    
    if (!init) {
        // Create a scrollable container for the instructions
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
        lv_obj_set_style_shadow_width(cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content

        // Title label
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "How to View:");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instructions label (scrollable if text is long)
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_align_to(instr_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

        // Set instruction text
        const char *res = (raw_frames_ai_response) ? raw_frames_ai_response : "";

        // Copy result into portal storage and start the portal
        ai_analysis_portal_set_result(res);

        // Activate web portal
        xEventGroupSetBits(xWiFiPortalEventGroup, WIFI_PORTAL_START_AI_PKT_ANALYSIS_BIT);

        char instr_text[512];
        snprintf(instr_text, sizeof(instr_text),
                    "The results are displayed using a web portal.\nTo access it, please connect to the following Wi-Fi network:\n\n"
                    "SSID:\n - %s\n\n"
                    "After connecting, simply search:\n\n"
                    "http://%s/\n\n"
                    "Do NOT leave this page until you're done viewing the results!",
                    ai_analysis_portal_get_ssid(),
                    ai_analysis_portal_get_ip());

        lv_label_set_text(instr_lbl, instr_text);

        init = true;
    }
    
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, WIFI_AI_PKT_RESULTS_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -WIFI_AI_PKT_RESULTS_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->left_btn) { // Go back
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
            
        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Show top and bottom arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Switch back
        ui_menu->page = WIFI_PAGE;

        // Disable portal and Wi-Fi (extra safety)
        xEventGroupClearBits(xWiFiPortalEventGroup, WIFI_PORTAL_START_AI_PKT_ANALYSIS_BIT);
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
        
         lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep

        // Stop the analysis portal if running
        xEventGroupClearBits(xWiFiPortalEventGroup, WIFI_PORTAL_START_AI_PKT_ANALYSIS_BIT);
    }
}

static void update_password_label_lcd(lv_obj_t *lbl_display, char cur_char, int cur_pos)
{
    char display[MAX_PASSWORD_LEN + 2];
    size_t len = cur_pos + 1;

    if (len > MAX_PASSWORD_LEN) {
        len = MAX_PASSWORD_LEN;
    }

    // Copy existing
    memcpy(display, mqtt_name_buf, cur_pos);

    // Show current selection
    display[cur_pos] = cur_char;
    display[cur_pos + 1] = '\0';

    lv_label_set_text(lbl_display, display);
    lv_obj_align(lbl_display, LV_ALIGN_CENTER, 0, 30);
}

void lcd_wifi_get_password(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    // Statics
    static lv_obj_t *lbl_dirs, *lbl_user_in;
    static int cur_pos = 0;
    static int row_idx = 0; // Active row
    static int char_idx = 0; // Index within that row
    static bool initialized = false;
    static char cur_char; // Current character

    // Do once
    if (!initialized) {
        // Everything is zero'd out to start
        memset(mqtt_name_buf, 0, sizeof mqtt_name_buf);
        row_idx = 0;
        char_idx = 0;
        cur_char = char_rows[0][0]; // Start at 0, 0

        // Helper labels
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_dirs, "Enter Wi-Fi password: Press\n  home to cycle characters.", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -30);

        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
        
        initialized = true;
    }

    // Switch rows (ex A->a)
    if (ui_btns->home_btn) {
         // Increment with wrap
        row_idx = (row_idx + 1) % NUM_CHAR_ROWS;
        
        // Update current char
        char_idx = 0;
        cur_char = char_rows[row_idx][char_idx];
        
        // Show to LCD
        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->up_btn || ui_btns->down_btn) { // Cycle specific character
        // Get length of selected row
        size_t row_len = strlen(char_rows[row_idx]);
        
        // Increment/decrement that row with wrap
        if (ui_btns->up_btn) {
            char_idx = (char_idx + 1) % row_len;
        } else if (ui_btns->down_btn) {
            char_idx = (char_idx + row_len - 1) % row_len;
        }
        
        // Update the current character
        cur_char = char_rows[row_idx][char_idx];
        
        // Show to LCD
        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->right_btn) { // Move character position
        // Save current char to name buffer
        mqtt_name_buf[cur_pos] = cur_char;
        
        // Increment position
        if (cur_pos < MAX_PASSWORD_LEN) {
            cur_pos++;
            
            // Reset characters
            char_idx = 0;
            cur_char = char_rows[row_idx][0];
        }
        
        // Show to LCD
        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->left_btn && cur_pos == 0) { // Back
        // Delete objects
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        
        // Reset statics
        lbl_user_in = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        initialized = false;

        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_PAGE;
        
        return;
    } else if (ui_btns->pwr_btn) { // Power off
        // Delete objects
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        
        // Reset statics
        lbl_user_in = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        initialized = false;

        lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
    } else if (ui_btns->left_btn) { // Backspace
        // Save change to name buffer
        mqtt_name_buf[cur_pos] = '\0';
        
        // Decrement position
        if (cur_pos > 0) {
            cur_pos--;
        }

        // Reload cur_char from the new slot
        char target = mqtt_name_buf[cur_pos] ? mqtt_name_buf[cur_pos] : '_';
        // Search each row for that character
        for (row_idx = 0; row_idx < NUM_CHAR_ROWS; row_idx++) {
            const char *row = char_rows[row_idx]; // Get active row
            const char *p = strchr(row, target); // Scan row for character target
            if (p) {
                char_idx = (uint16_t)(p - row); // How many chars from row to reach p
                break;
            }
        }

        // Update cur_char
        cur_char = char_rows[row_idx][char_idx];

        update_password_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->select_btn) { // Save
        // Commit the current character
        if (cur_pos < MAX_PASSWORD_LEN && mqtt_name_buf[cur_pos] == '\0') {
            mqtt_name_buf[cur_pos++] = cur_char;
        }
    
        mqtt_name_buf[cur_pos] = '\0'; // Null-terminate
        
        // Send to Wi-Fi task
        selected_network.locked = true; // Requires password
        strlcpy((char*)selected_network.password, mqtt_name_buf, sizeof(selected_network.password));
        if (xQueueSend(xWifiSelectedNetworkQueue, &selected_network, portMAX_DELAY) != pdPASS) { // SSID was copied earlier
            ESP_LOGE(TAG, "Failed: xWifiSelectedNetworkQueue PASSWORD");
        }

        // Delete objects
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        
        // Reset statics
        lbl_user_in = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        initialized = false;

        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_PAGE;
        
        return;
    }
}

// Colors bar more green or red based on value
static void beacon_chart_draw_cb(lv_event_t * e)
{
    // Get task and descriptor
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);
    
    // Filter to only the bars
    if (base->part != LV_PART_ITEMS) {
        return;
    }

    // Get the fill descriptor
    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    
    // Ensure valid
    if (!fill) {
        return;
    }

    // Fetch series data and compute the index
    lv_obj_t *chart = lv_event_get_target_obj(e);
    lv_chart_series_t *ser = lv_event_get_user_data(e);
    int32_t *y_array = lv_chart_get_y_array(chart, ser);

    uint32_t pc = lv_chart_get_point_count(chart);
    uint32_t idx = base->id2; // Column being painted
    uint32_t logical = (ser->start_point + idx) % pc; // Real slot
    int32_t v = y_array[logical]; // SNR 0-50
    if (v > 50) { // Cap
        v = 50;
    }

    // Skip uninitialized points
    if (v == LV_CHART_POINT_NONE) {
        return;
    }

    // Compute red-green mix ratio
    uint8_t mix = (uint8_t)((uint32_t)v * 255 / 50); // 0=red, 50=green
    fill->color = lv_color_mix(lv_palette_main(LV_PALETTE_GREEN), lv_palette_main(LV_PALETTE_RED), mix);
}

static const char* rsn_cipher_str(const wifi_beacon_t *b)
{
    if (!b) {
        return "?";
    }

    // Prefer modern ciphers if multiple present
    if (b->rsn_pairwise_ciphers & (1u << RSN_CIPHER_GCMP_256)) {
        return "GCMP-256";
    }
    if (b->rsn_pairwise_ciphers & (1u << RSN_CIPHER_CCMP_256)) {
        return "CCMP-256";
    }
    if (b->rsn_pairwise_ciphers & (1u << RSN_CIPHER_GCMP_128)) {
        return "GCMP";
    }
    if (b->rsn_pairwise_ciphers & (1u << RSN_CIPHER_CCMP_128)) {
        return "AES (CCMP)";
    }
    if (b->rsn_pairwise_ciphers & (1u << RSN_CIPHER_TKIP))     {
        return "TKIP";
    }

    return "?";
}

static const char* pmf_str(const wifi_beacon_t *b)
{
    if (!b) {
        return "PMF: ?";
    }
    if (b->pmf_required) {
        return "PMF: Required";
    }
    if (b->pmf_capable)  {
        return "PMF: Optional";
    }

    return "PMF: No";
}

static const char* akm_str(const wifi_beacon_t *b)
{
    if (!b) {
        return "?";
    }

    bool has_sae  = (b->rsn_akm_suites & (1u << RSN_AKM_SAE))   != 0;
    bool has_psk  = (b->rsn_akm_suites & (1u << RSN_AKM_PSK))   != 0;
    bool has_1x   = (b->rsn_akm_suites & (1u << RSN_AKM_8021X)) != 0;
    bool has_owe  = (b->rsn_akm_suites & (1u << RSN_AKM_OWE))   != 0;

    if (has_owe) {
        return "OWE (Enhanced Open)";
    }
    if (has_sae && has_psk) {
        return "WPA2/WPA3 Transition";
    }
    if (has_sae) {
        return "WPA3-Personal (SAE)";
    }
    if (has_1x) {
        return "WPA2-Enterprise (802.1X)";
    }
    if (has_psk) {
        return "WPA2-Personal (PSK)";
    }

    return "WPA2/WPA3 (RSN)";
}

static const char* phy_str(const wifi_beacon_t *b)
{
    switch (b->phy) {
        case WIFI_PHY_11AX: return "802.11ax (Wi-Fi 6)";
        case WIFI_PHY_11AC: return "802.11ac (Wi-Fi 5)";
        case WIFI_PHY_11N:  return "802.11n (Wi-Fi 4)";
        case WIFI_PHY_11A:  return "802.11a";
        case WIFI_PHY_11G:  return "802.11g";
        case WIFI_PHY_11B:  return "802.11b";
        default:            return "Unknown";
    }
}

void lcd_wifi_beacon_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    #define BEACON_SCROLL_STEP 55  // Pixels per button-press
    
    static bool init = false;
    static lv_obj_t *cont;
    static lv_obj_t *chart;
    static lv_chart_series_t *series;
    static lv_obj_t *lbl_rssi;
    static lv_obj_t *lbl_snr;
    static lv_obj_t *lbl_scroll;
    static lv_obj_t *lbl_info;
    static lv_obj_t *lbl_data;

    if(!init) {
        // Create a scrollable container
        cont = lv_obj_create(ACTIVE_SCR);
        // Format
        lv_obj_align(cont, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_ON);

        // Chart at the top
        chart = lv_chart_create(cont);
        // Format
        lv_obj_set_size(chart, 186, 60);
        lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(chart, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(chart, 40);
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 50);
        lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
        
        // Bar
        series = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
        // Styling
        lv_obj_set_style_width(chart, 8, LV_PART_INDICATOR);
        lv_obj_set_style_pad_column(chart, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chart, lv_palette_main(LV_PALETTE_GREEN), LV_PART_ITEMS);
        
        // Callback to change bar color based on value
        // Tell the chart to generate draw-task events
        lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        // Register your callback on DRAW_TASK_ADDED
        lv_obj_add_event_cb(chart, beacon_chart_draw_cb, LV_EVENT_DRAW_TASK_ADDED, series);
        
        // Small text
        lbl_rssi = lv_label_create(cont);
        lcd_format_label(lbl_rssi, "RSSI: ", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_TOP_LEFT, 0, -10);
                         
        lbl_snr = lv_label_create(cont);
        lcd_format_label(lbl_snr, "SNR: ", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_TOP_RIGHT, -5, -10);
                         
        lbl_scroll = lv_label_create(cont);
        lcd_format_label(lbl_scroll, "SCROLL", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, 15);
                         
        lbl_data = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_data, "DATA " LV_SYMBOL_RIGHT, user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_BOTTOM_RIGHT, -3, 0);

        // Main info
        lbl_info = lv_label_create(cont);
        lcd_format_label(lbl_info, "", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_obj_align_to(lbl_info, chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 25);

        lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
        lv_label_set_text(lbl_info, "Configuring...");

        init = true;
    }

    // On each new beacon
    wifi_beacon_t beacon;
    if (xQueueReceive(xWifiBeaconQueue, &beacon, 0) == pdTRUE) {
        //ESP_LOGI(TAG, "RX REC %d", current_snr);
        lv_chart_set_next_value(chart, series, beacon.snr);
        lv_chart_refresh(chart);
        
        // Update SNR text
        char snr_buf[10];
        snprintf(snr_buf, sizeof(snr_buf), "SNR: %d", beacon.snr);
        lv_label_set_text(lbl_snr, snr_buf);
        
        // Update RSSI text
        char rssi_buf[11];
        snprintf(rssi_buf, sizeof(rssi_buf), "RSSI: %d", beacon.rssi);
        lv_label_set_text(lbl_rssi, rssi_buf);
        
        // Update other text
        char txt_buf[256];

        // Capability bit 4 = Privacy
        bool privacy = (beacon.cap_info & 0x0010) != 0;

        char sec_line[96] = {0};

        if (!privacy && !beacon.rsn && !beacon.wpa) {
            snprintf(sec_line, sizeof(sec_line), "Open (no encryption)");
        } else if (privacy && !beacon.rsn && !beacon.wpa) {
            snprintf(sec_line, sizeof(sec_line), "WEP / legacy encryption");
        } else if (beacon.rsn) {
            // RSN network: show AKM + cipher + PMF
            snprintf(sec_line, sizeof(sec_line), "%s\n - %s\n - %s",
                    akm_str(&beacon), rsn_cipher_str(&beacon), pmf_str(&beacon));
        } else if (beacon.wpa) {
            snprintf(sec_line, sizeof(sec_line), "WPA (legacy)");
        } else {
            snprintf(sec_line, sizeof(sec_line), "Encrypted (unknown)");
        }

        const char *wps = beacon.wps ? "Yes" : "No";

        // beacon.timestamp is in seconds
        uint64_t timestamp_mins = beacon.timestamp / 60; 
        uint64_t timestamp_days = beacon.timestamp / (24 * 60 * 60); 
        
        int len = snprintf(txt_buf, sizeof(txt_buf),
            "SSID:\n - %.16s...\n"
            "Channel:\n - %d\n"
            "Type:\n - %s\n - %.3f GHz\n"
            "Security:\n - %s\n"
            " - WPS: %s\n"
            "Compatibility Code:\n - 0x%04X\n"
            "Beacon Interval:\n - %u ms\n"
            "Time since reboot:\n - %" PRIu64 "m / %" PRIu64 " days",
            beacon.ssid,
            beacon.channel,
            phy_str(&beacon),
            beacon.freq / 1000.0f,
            sec_line,
            wps,
            beacon.cap_info,
            beacon.interval,
            timestamp_mins, // Minutes
            timestamp_days // Days
        );
        // Check for truncation
        if (len < 0 || (size_t)len >= sizeof(txt_buf)) {
            ESP_LOGE(TAG, "Label truncation: lcd_wifi_beacon_page");
        } else {
            lv_label_set_text(lbl_info, txt_buf);
        }
    }
    
    // Scroll down
    if (ui_btns->down_btn) {
        lv_obj_scroll_by_bounded(cont, 0, -BEACON_SCROLL_STEP, true);
    } else if (ui_btns->up_btn) { // Scroll up
        lv_obj_scroll_by_bounded(cont, 0, BEACON_SCROLL_STEP, true);
    } else if (ui_btns->left_btn) { // Back
        // Delete obj
        lv_obj_delete(cont); // Also deletes children
        lv_obj_delete(lbl_data); // Not child of cont
        
        // Reset statics
        init = false;
        cont = chart = lbl_rssi = lbl_snr = lbl_scroll = lbl_info = lbl_data = NULL;
        series = NULL;
        
        // Turn off Wi-Fi
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Go home or power off
        // Delete obj
        lv_obj_delete(cont); // Also deletes children
        lv_obj_delete(lbl_data); // Not child of cont
        
        // Reset statics
        init = false;
        cont = chart = lbl_rssi = lbl_snr = lbl_scroll = lbl_info = lbl_data = NULL;
        series = NULL;
        
        // Turn off Wi-Fi
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
        
        lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    } else if (ui_btns->right_btn) { // Switch to data frames
        // Delete obj
        lv_obj_delete(cont); // Also deletes children
        lv_obj_delete(lbl_data); // Not child of cont
        
        // Reset statics
        init = false;
        cont = chart = lbl_rssi = lbl_snr = lbl_scroll = lbl_info = lbl_data = NULL;
        series = NULL;
        
        // Switch mask
        sniff_network.mask = WIFI_PROMIS_FILTER_MASK_DATA;
        
        // Restart
        if (xQueueSend(xWifiSniffQueue, &sniff_network, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed: xWifiSniffQueue data");
        }
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_DATA_PAGE;
    }
}


// Colors bar more green or red based on value
static void data_chart_draw_cb(lv_event_t * e)
{
    // Get task and descriptor
    lv_draw_task_t *task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(task);
    
    // Filter to only the bars
    if (base->part != LV_PART_ITEMS) {
        return;
    }

    // Get the fill descriptor
    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(task);
    
    // Ensure valid
    if (!fill) {
        return;
    }

    // Fetch series data and compute the index
    lv_obj_t *chart = lv_event_get_target_obj(e);
    lv_chart_series_t *ser = lv_event_get_user_data(e);
    int32_t *y_array = lv_chart_get_y_array(chart, ser);

    uint32_t pc = lv_chart_get_point_count(chart);
    uint32_t idx = base->id2; // Column being painted
    uint32_t logical = (ser->start_point + idx) % pc; // Real slot
    
    int32_t  rssi_db = y_array[logical];       // e.g. –40

    if (rssi_db == LV_CHART_POINT_NONE) {
        return;
    }

    // Given your chart range is MIN=-100, MAX=0:
    const int32_t MIN_DB = -100;
    const int32_t MAX_DB = -40;
    const uint32_t RANGE = (uint32_t)(MAX_DB - MIN_DB);
    
    // rssi_db is something between MIN_DB...MAX_DB
    // Shift into 0..RANGE
    uint32_t strength = (uint32_t)(rssi_db - MIN_DB); // e.g. –40->60, –100->0, 0->100
    
    // Cap at max
    if (strength > RANGE) {
        strength = RANGE;
    }
    
    // Compute mix 0...255
    uint8_t mix = (uint8_t)((strength * 255u) / RANGE);
    
    // Create color
    fill->color = lv_color_mix(lv_palette_main(LV_PALETTE_GREEN), lv_palette_main(LV_PALETTE_RED), mix);
}

static int cmp_rssi(const void *a, const void *b)
{
    const wifi_data_clients_t *A = a, *B = b;
    return B->rssi - A->rssi; // Descending (-30 dBm above -70 dBm)
}

void lcd_wifi_data_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    #define MAX_BARS 40
    #define SCROLL_STEP 53 // Pixels per button-press

    // Statics
    static bool init = false;
    static lv_obj_t *cont, *chart, *lbl_info, *lbl_clients, *lbl_scroll, *lbl_beacon;
    static lv_chart_series_t *series;

    // Do once
    if(!init) {
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Create container
        cont = lv_obj_create(ACTIVE_SCR);
        // Format
        lv_obj_align(cont, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_set_scroll_dir(cont, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_ON);
        
        // Chart at the top
        chart = lv_chart_create(cont);
        // Format
        lv_obj_set_size(chart, 186, 60);
        lv_obj_align(chart, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(chart, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(chart, MAX_BARS);
        lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -100, -30);
        lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
        
        // Bar
        series = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
        // Styling
        lv_obj_set_style_width(chart, 8, LV_PART_ITEMS);
        lv_obj_set_style_pad_column(chart, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chart, lv_palette_main(LV_PALETTE_GREEN), LV_PART_ITEMS);
        
        // Callback to change bar color based on value
        // Tell the chart to generate draw-task events
        lv_obj_add_flag(chart, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        // Register your callback on DRAW_TASK_ADDED
        lv_obj_add_event_cb(chart, data_chart_draw_cb, LV_EVENT_DRAW_TASK_ADDED, series);
        
        // Top text
        lbl_clients = lv_label_create(cont);
        lcd_format_label(lbl_clients, "", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, -10);
        
        // Helper texts
        lbl_scroll = lv_label_create(cont);
        lcd_format_label(lbl_scroll, "SCROLL", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, 15);
                         
        lbl_beacon = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_beacon, LV_SYMBOL_LEFT " BEACON", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 3, 0);

        // MACs text
        lbl_info = lv_label_create(cont);
        lv_obj_set_style_text_color(lbl_info, user_secondary_color, 0);
        lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_14, 0);
        lv_label_set_text(lbl_info, "Waiting for MACs...");
        lv_label_set_long_mode(lbl_info, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl_info, 190);
        lv_obj_align_to(lbl_info, chart, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 25);

        init = true;
    }

    // When new data received
    wifi_data_t *wifi_data;
    if (xQueueReceive(xWifiDataQueue, &wifi_data, 0) == pdTRUE) {
        // Update top
        char top_buf[64];
        snprintf(top_buf, sizeof(top_buf), "%" PRIu32 " users on Ch%" PRIu32 "@%" PRIu32 "Mbps\n", wifi_data->client_count, wifi_data->channel, wifi_data->rate);
        lv_label_set_text(lbl_clients, top_buf);
        
        // Sort by RSSI
        qsort(wifi_data->clients, wifi_data->client_count, sizeof(wifi_data->clients[0]), cmp_rssi);

        // Rebuild chart
        uint32_t bars = MIN(wifi_data->client_count, MAX_BARS); // Cap at MAX_BARS
        
        // Resize the chart’s internal point buffer so you never draw empty slots
        lv_chart_set_point_count(chart, bars);
        
        // Get the raw Y-array pointer for your series.
        int32_t *ya = lv_chart_get_y_array(chart, series);
        
        // Copy each client’s latest RSSI into that array in sorted order
        for (uint32_t i = 0; i < bars; ++i) {
            ya[i] = wifi_data->clients[i].rssi;
        }

        // Redraw
        lv_chart_refresh(chart);

        // Rebuild info text
        char buf[512];
        size_t off = 0;
        
        off += snprintf(buf, sizeof(buf), "Unique users (MACs):\n");

        for (uint32_t i = 0; i < wifi_data->client_count && off < sizeof(buf); ++i) {
            const uint8_t *m = wifi_data->clients[i].mac;
            off += snprintf(buf + off, sizeof(buf) - off, "%02X:%02X:%02X:%02X:%02X:%02X @%3d\n",
                    m[0],m[1],m[2],m[3],m[4],m[5],
                    wifi_data->clients[i].rssi);
        }
        lv_label_set_text(lbl_info, buf);
    }

    // Scroll down
    if (ui_btns->down_btn) {
        lv_obj_scroll_by_bounded(cont, 0, -SCROLL_STEP, true);
    } else if (ui_btns->up_btn) { // Scroll up
        lv_obj_scroll_by_bounded(cont, 0,  SCROLL_STEP, true);
    } else if (ui_btns->left_btn) { // Back to beacon
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
         
        // Delete obj
        lv_obj_delete(cont); // Also deletes children
        lv_obj_delete(lbl_beacon); // Parent is ACTIVE_SCR
        
        // Reset statics
        init = false;
        cont = chart = lbl_info = lbl_clients = lbl_scroll = lbl_beacon = NULL;
        series = NULL;
        
        // Switch mask
        sniff_network.mask = WIFI_PROMIS_FILTER_MASK_MGMT;
                
        // Restart
        if (xQueueSend(xWifiSniffQueue, &sniff_network, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed: xWifiSniffQueue back to beacon");
        }
        
        // Show right arrow
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = WIFI_BEACON_PAGE;
    } else if (ui_btns->home_btn) { // Go home
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
         
        // Delete obj
        lv_obj_delete(cont); // Also deletes children
        lv_obj_delete(lbl_beacon); // Parent is ACTIVE_SCR
        
        // Reset statics
        init = false;
        cont = chart = lbl_info = lbl_clients = lbl_scroll = lbl_beacon = NULL;
        series = NULL;
        
        // Turn off Wi-Fi
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
        
        lcd_funcs_transition_back(true, ui_menu); // True = home, false = sleep
    } else if (ui_btns->pwr_btn) { // Power off
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
         
        // Delete obj
        lv_obj_delete(cont); // Also deletes children
        lv_obj_delete(lbl_beacon); // Parent is ACTIVE_SCR
        
        // Reset statics
        init = false;
        cont = chart = lbl_info = lbl_clients = lbl_scroll = lbl_beacon = NULL;
        series = NULL;
        
        // Turn off Wi-Fi
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
        
        lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
    }
}

void lcd_wifi_sync_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    static bool init = false;
    
    static lv_obj_t *lbl_ins;
    
    if (!init) {
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        lbl_ins = lv_label_create(ACTIVE_SCR);
        
        lcd_format_label(lbl_ins, "1. Bring near desired PolyPlug.\n2. Press the top right button\non the PolyPlug.\n3. Confirm LED is showing\nblue on the PolyPlug.\n4. On this device, hit the\nright arrow to confirm.", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_CENTER, 6, 6);
        
        init = true;
    }
    
    // Send via ESP-NOW
    if (ui_btns->right_btn == 1) {
        // Create unique ID
        // TRNG already since Wi-Fi is active
        esp_fill_random(mqtt_key, sizeof(mqtt_key));
        
        // Copy info
        espnow_mqtt_t sync_info;
        memcpy(sync_info.key, mqtt_key, sizeof(mqtt_key));
        strlcpy(sync_info.password, selected_network.password, sizeof(sync_info.password));
        strlcpy(sync_info.ssid, selected_network.ssid, sizeof(sync_info.ssid));
    
        // Transmit via ESP-NOW
        xQueueSend(xEspSendMqttQueue, &sync_info, portMAX_DELAY);
                
        // Exit
        // Put back arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Delete label
        lv_obj_delete(lbl_ins);
        
        // Reset statics
        init = false;
        lbl_ins = NULL;
        
        // Go to name page
        ui_menu->page = WIFI_NAME_PAGE;
    } else if (ui_btns->left_btn == 1) { // Back
        // Put back arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Delete label
        lv_obj_delete(lbl_ins);
        
        // Reset statics
        init = false;
        lbl_ins = NULL;
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Go back
        ui_menu->page = WIFI_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Go home or power off
        // Put back arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Delete label
        lv_obj_delete(lbl_ins);
        
        // Reset statics
        init = false;
        lbl_ins = NULL;
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

static void update_name_label_lcd(lv_obj_t *lbl_display, char cur_char, int cur_pos)
{
    char display[MAX_CUSTOM_NAME_LEN + 2]; // Buffer
    
    int len = cur_pos + 1; // Current length of name
    
    // Cap
    if (len > MAX_CUSTOM_NAME_LEN + 1) {
        len = MAX_CUSTOM_NAME_LEN + 1;
    }
    
    // Copy name into display buffer
    if (cur_pos > 0) {
        memcpy(display, mqtt_name_buf, cur_pos);
    }
    
    // Get current
    display[cur_pos] = cur_char;
    display[len] = '\0';
    
    // Set text and re-center
    lv_label_set_text(lbl_display, display);
    lv_obj_align(lbl_display, LV_ALIGN_CENTER, 0, 30);
}

void lcd_wifi_create_custom_name(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    // Declare statics
    static char saved_name[MAX_CUSTOM_NAME_LEN + 1] = {0};
    static int cur_pos = 0; // User position
    static int row_idx = 0; // Which character row is active
    static int char_idx = 0; // Index within that row
    static char cur_char = '_';
    static lv_obj_t *lbl_dirs = NULL;
    static lv_obj_t *lbl_chars = NULL;
    static lv_obj_t *lbl_user_in = NULL;
    
    // Create initial label
    if (!lbl_user_in) {
        // If renaming, autofill what was there previously
        if (wifi_menu_overwrite) {
            // Copy the old name into buffer
            strncpy(mqtt_name_buf, wifi_menu->options[wifi_menu->index], MAX_CUSTOM_NAME_LEN);

            // Place cursor at the end
            cur_pos = strlen(mqtt_name_buf);
        } else { // Else blank slate
            memset(mqtt_name_buf, 0, sizeof mqtt_name_buf);
            cur_pos = 0;
        }
        
        // Starting char
        row_idx = 0;
        char_idx = 0;
        cur_char = char_rows[row_idx][char_idx];
        
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_dirs, "        Enter plug name:\nPress HOME to cycle chars.", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -31);
                         
        if (wifi_menu_overwrite) {
            lv_label_set_text(lbl_dirs, "    Enter new plug name:\nPress HOME to cycle chars.");
        }
        
        lbl_chars = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_chars, "(Up to 12 characters)", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0);
                         
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    }

    /* User input */
    // Cycle chars
    if (ui_btns->home_btn) {
        // Cycle character row
        row_idx = (row_idx + 1) % NUM_CHAR_ROWS;
        char_idx = 0; // Reset within row
        
        // New current char
        cur_char = char_rows[row_idx][char_idx];
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->up_btn) { // If up, iterate up
        // Increment with wrap
        size_t row_len = strlen(char_rows[row_idx]);
        char_idx = (char_idx + 1) % (int)row_len;
        cur_char = char_rows[row_idx][char_idx];
        
        // Save to array
        mqtt_name_buf[cur_pos] = cur_char;
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->down_btn) { // If down, iterate down
        // Decrement with wrap
        size_t row_len = strlen(char_rows[row_idx]);
        char_idx = (char_idx + (int)row_len - 1) % (int)row_len;
        cur_char = char_rows[row_idx][char_idx];
        
        // Save to array
        mqtt_name_buf[cur_pos] = cur_char;
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->left_btn && cur_pos == 0 && wifi_menu_overwrite) { // Can back out if at start and renaming
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(mqtt_name_buf, 0, sizeof mqtt_name_buf);
        
        wifi_menu_overwrite = false; // Switch back
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
                
        // Show Wi-Fi list
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
         ui_menu->page = WIFI_PAGE;

        return;
    } else if (ui_btns->pwr_btn && wifi_menu_overwrite) { // Go home or power off if ranaming
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(mqtt_name_buf, 0, sizeof mqtt_name_buf);
        
        wifi_menu_overwrite = false;
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    } else if (ui_btns->left_btn && cur_pos != 0) { // If left and not at start
        // Clear the current slot
        mqtt_name_buf[cur_pos] = '\0';
    
        // De-increment left
        if (cur_pos > 0) {
            cur_pos--;
        }
    
        // Reload row/idx from the new slot's char
        char target = mqtt_name_buf[cur_pos] ? mqtt_name_buf[cur_pos] : '_';
        for (row_idx = 0; row_idx < NUM_CHAR_ROWS; row_idx++) {
            const char *row = char_rows[row_idx];
            const char *p = strchr(row, target);
            
            if (p) {
                char_idx = (int)(p - row);
                break;
            }
        }
        cur_char = char_rows[row_idx][char_idx];
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->right_btn) { // If right
        // Handle case where up/down wasn't pressed
        mqtt_name_buf[cur_pos] = cur_char;
        
        // If not yet at end
        if (cur_pos < MAX_CUSTOM_NAME_LEN - 1) {
            cur_pos++;
            mqtt_name_buf[cur_pos] = '\0';
            char_idx = 0;
            cur_char = char_rows[row_idx][char_idx];
        } else {
            mqtt_name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
        }
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->select_btn) { // If save button pressed
        // Save final
        if (cur_pos < MAX_CUSTOM_NAME_LEN) {
            mqtt_name_buf[cur_pos] = cur_char;

            // Terminate one past the last written char if room, else clamp
            size_t term = (cur_pos + 1 <= MAX_CUSTOM_NAME_LEN) ? (cur_pos + 1) : MAX_CUSTOM_NAME_LEN;
            mqtt_name_buf[term] = '\0';
        }
        
        mqtt_name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
        memcpy(saved_name, mqtt_name_buf, MAX_CUSTOM_NAME_LEN + 1);
        
        #ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "%s", saved_name);
        #endif
        
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(mqtt_name_buf, 0, sizeof mqtt_name_buf);

        // Update options
        // If overwriting an existing as a rename
        if (wifi_menu_overwrite) {
            // wifi_menu->index is edit_idx
            // Release old string then reallocate
            free(wifi_menu->options[wifi_menu->index]);
            wifi_menu->options[wifi_menu->index] = strdup(saved_name);

            // Persist to NVS
            lcd_wifi_menu_nvs_save(wifi_menu);

            // Update the button’s label in-place
            lv_obj_t *btn = wifi_menu->btns[wifi_menu->index];
            lv_obj_t *child_lbl = lv_obj_get_child(btn, 0);
            lv_label_set_text(child_lbl, wifi_menu->options[wifi_menu->index]);

            // Reset flag
            wifi_menu_overwrite = false;
        } else { // Else adding a whole new Wi-Fi plug
            // Size one bigger
            wifi_menu->size++;
            
            // Save to options, then to NVS
            char *name_copy = strdup(saved_name);
            wifi_menu->options[wifi_menu->size - 1] = name_copy;
            lcd_wifi_menu_nvs_save(wifi_menu);
            
            // Create new button for new option
            wifi_menu->btns[wifi_menu->size - 1] = lv_list_add_btn(wifi_menu->main_list, NULL, wifi_menu->options[wifi_menu->size - 1]);
            lv_obj_set_size(wifi_menu->btns[wifi_menu->size - 1], 200, 30);
            lv_obj_add_style(wifi_menu->btns[wifi_menu->size - 1], &wifi_menu->btn_style, 0);
            
            // Create and format text label
            lv_obj_t *lbl = lv_obj_get_child(wifi_menu->btns[wifi_menu->size - 1], 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
            
            // Save topic key to NVS
            // wifi_menu->size - (WIFI_MENU_START_SIZE + 1) offsets index by WIFI_MENU_START_SIZE to start saving at index 0
            memcpy(wifi_menu->topic_keys[wifi_menu->size - (WIFI_MENU_START_SIZE + 1)], mqtt_key, sizeof(wifi_menu->topic_keys[wifi_menu->size - (WIFI_MENU_START_SIZE + 1)]));
            lcd_wifi_topic_keys_nvs_save(wifi_menu);
        }
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show Wi-Fi list
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch to Wi-Fi page
        ui_menu->page = WIFI_PAGE;
        return;
    }
}

static void hide_wifi_send_page(wifi_menu_t *wifi_menu)
{
    // Hide everything
    lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_send_ins, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_menu->wifi_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_menu->wifi_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
}

static void prompt_name_or_del(ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
    
    // Create and format ins labels
    lv_obj_t *lbl_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ins, LV_SYMBOL_SETTINGS, user_secondary_color,
                 &lv_font_montserrat_30, LV_ALIGN_CENTER, 0, 0);
                 
    lv_obj_t *lbl_exit = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_exit, "BACK", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_LEFT_MID, 16, -1);
                 
    lv_obj_t *lbl_name = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_name, "RENAME", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 13);
                 
    lv_obj_t *lbl_del = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_del, "DELETE", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_BOTTOM_MID, 0, -13);
                    
    while (1) {
        lv_timer_handler();
        
        // User hit cancel
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            // Hide up and down arrows
            lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
                
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Show wifi send page
            lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_ins, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_cmd, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_send, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_edit, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(wifi_menu->wifi_submenu.arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(wifi_menu->wifi_submenu.arrow_bot, LV_OBJ_FLAG_HIDDEN);
            
            // Switch pages
            ui_menu->page = WIFI_SEND_PAGE;
            
            // Go back
            return;
        }
        // Rename
        else if (xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            wifi_menu_overwrite = true; // Set overwrite flag
            
            // Prompt to enter name
            ui_menu->page = WIFI_NAME_PAGE;
            
            // Go back
            return;
        }
        // Delete
        else if (xSemaphoreTake(xDownButtonSemaphore, 0) == pdTRUE) {
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            
            // Delete the entry
            // Delete entry is wifi_menu->index
            uint8_t del_idx = wifi_menu->index;
            
            // Ensure valid entry to delete
            if (del_idx < WIFI_MENU_START_SIZE || del_idx >= wifi_menu->size) {
                return;
            }
            
            // Get the corresponding topic index
            uint8_t topic_slot = del_idx - WIFI_MENU_START_SIZE;
        
            // Shift all keys after topic_slot down one
            for (uint8_t i = topic_slot; (i + 1) < (wifi_menu->size - WIFI_MENU_START_SIZE); ++i) {
                memcpy(wifi_menu->topic_keys[i], wifi_menu->topic_keys[i + 1], TOPIC_KEY_LEN);
            }
            
            // Zero out the now dangling slot
            memset(wifi_menu->topic_keys[wifi_menu->size - WIFI_MENU_START_SIZE - 1], 0, TOPIC_KEY_LEN);
        
            // Also free the deleted option and associated button
            free(wifi_menu->options[del_idx]);
            lv_obj_delete(wifi_menu->btns[del_idx]);
            
            // Shift all the remaining options and buttons down one
            for (uint8_t i = del_idx; (i + 1) < wifi_menu->size; ++i) {
                wifi_menu->options[i] = wifi_menu->options[i + 1];
                wifi_menu->btns[i] = wifi_menu->btns[i + 1];
            }
            
            // Null out dangling indexs
            wifi_menu->options[wifi_menu->size] = NULL;
            wifi_menu->btns[wifi_menu->size] = NULL;
        
            // Shrink the menu
            wifi_menu->size--;
            
            // Persist both to NVS (single-save helpers will erase the old tail)
            lcd_wifi_menu_nvs_save(wifi_menu);
            lcd_wifi_topic_keys_nvs_save(wifi_menu);
            
            // Adjust if was last
            if (wifi_menu->index >= wifi_menu->size) {
                wifi_menu->index = wifi_menu->size - 1;
            }

            // Refresh the list UI
            lcd_wifi_update_menu(wifi_menu);
            
            // Hide right arrow
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Show Wi-Fi menu
            lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            
            // Switch pages
            ui_menu->page = WIFI_PAGE;
            
            // Go back
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lcd_wifi_send_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu)
{
    #define BUF_SIZE 4
    
    // Define statics
    static bool three_dots = false;
    static bool mqtt_connected = false;
    
    // Update labels on status
    static EventBits_t last_wifi_event_bits = {0};
    EventBits_t wifi_event_bits = xEventGroupGetBits(xWifiEventGroup);
    if (wifi_event_bits != last_wifi_event_bits) { // Only act on changes
        // If Wi-Fi MQTT connected bit transitioned 0 -> 1
        if ((wifi_event_bits & WIFI_MQTT_CONNECTED_BIT) && !(last_wifi_event_bits & WIFI_MQTT_CONNECTED_BIT)) {
            lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_ins, MQTT_READY_TXT);
            mqtt_connected = true;
        }
        // If Wi-Fi MQTT connected bit transitioned 1 -> 0
        if ((last_wifi_event_bits & WIFI_MQTT_CONNECTED_BIT) && !(wifi_event_bits & WIFI_MQTT_CONNECTED_BIT)) {
            lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_ins, MQTT_CONNECTING_TXT);
            mqtt_connected = false;
        }
        // If Wi-Fi MQTT success bit transitioned 0 -> 1
        if ((wifi_event_bits & WIFI_MQTT_SUCCESS_BIT) && !(last_wifi_event_bits & WIFI_MQTT_SUCCESS_BIT)) {
            lv_label_set_text(wifi_menu->wifi_submenu.lbl_receipt, LV_SYMBOL_OK);
            lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
            
            lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_ins, MQTT_READY_TXT);
            
            // Reset for next time
            xEventGroupClearBits(xWifiEventGroup, WIFI_MQTT_SUCCESS_BIT);
        }
        
        last_wifi_event_bits = wifi_event_bits;
    }
    
    // Send via MQTT
    if (ui_btns->right_btn == 1) {
        // Show dots to imply message is sending (change each time ...<->..)
        if (!three_dots) {
            lv_label_set_text(wifi_menu->wifi_submenu.lbl_receipt, "...");
            three_dots = !three_dots;
        } else {
            lv_label_set_text(wifi_menu->wifi_submenu.lbl_receipt, "..");
            three_dots = !three_dots;
        }
        lv_obj_remove_flag(wifi_menu->wifi_submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        
        if (mqtt_connected) {
            lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_ins, MQTT_SENDING_TXT);
            
            wifi_mqtt_t wifi_mqtt;
            
            // Format payload
            snprintf(wifi_mqtt.payload, sizeof(wifi_mqtt.payload), "%u", wifi_menu->wifi_submenu.cmd_to_send);
            
            // Get topic key for given entry (topic_keys is 0-based)
            memcpy(wifi_mqtt.key, wifi_menu->topic_keys[wifi_menu->index - WIFI_MENU_START_SIZE], sizeof(wifi_menu->topic_keys[wifi_menu->index - WIFI_MENU_START_SIZE]));
            
            // Send
            xQueueSend(xWifiMqttCmdQueue, &wifi_mqtt, portMAX_DELAY);
        }
    }
    // Command up
    if (ui_btns->up_btn == 1) {
        // Hide receipt check
        lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        
        wifi_menu->wifi_submenu.cmd_to_send++;
        
        // Format and display new value
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", wifi_menu->wifi_submenu.cmd_to_send);
        lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_cmd, buf);
    }
    // Command down
    if (ui_btns->down_btn == 1) {
        // Hide receipt check
        lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        
        wifi_menu->wifi_submenu.cmd_to_send--;
        
        // Format and display new value
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", wifi_menu->wifi_submenu.cmd_to_send);
        lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_cmd, buf);
    }
    // Command += 3
    if (ui_btns->select_btn == 1) {
        // Hide receipt check
        lv_obj_add_flag(wifi_menu->wifi_submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        
        wifi_menu->wifi_submenu.cmd_to_send += 3;
        
        // Format and display new value
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", wifi_menu->wifi_submenu.cmd_to_send);
        lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_cmd, buf);
    } else if (ui_btns->home_btn == 1) { // Edit
        // Hide current page
        hide_wifi_send_page(wifi_menu);
        
        // Reset default value
        wifi_menu->wifi_submenu.cmd_to_send = 1;
        // Update
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", wifi_menu->wifi_submenu.cmd_to_send);
        lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_cmd, buf);
        
        // Show top and bot arrows
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        
        prompt_name_or_del(ui_menu, wifi_menu);
    } else if (ui_btns->left_btn == 1) { // Back
        // Hide current page
        hide_wifi_send_page(wifi_menu);
        
        // Reset default value
        wifi_menu->wifi_submenu.cmd_to_send = 1;
        // Update
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", wifi_menu->wifi_submenu.cmd_to_send);
        lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_cmd, buf);
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show top and bot arrows
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        
        // Show Wi-Fi menu
        lv_obj_remove_flag(wifi_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Go back
        ui_menu->page = WIFI_PAGE;
    } else if (ui_btns->pwr_btn) { // Power off
        // Hide current page
        hide_wifi_send_page(wifi_menu);
        
        // Reset default value
        wifi_menu->wifi_submenu.cmd_to_send = 1;
        // Update
        char buf[BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", wifi_menu->wifi_submenu.cmd_to_send);
        lv_label_set_text(wifi_menu->wifi_submenu.lbl_send_cmd, buf);
        
        // Show top and bot arrows
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        
        lcd_funcs_transition_back(false, ui_menu); // True = home, false = sleep
    }
}


void lcd_wifi_setup_send_page(wifi_menu_t *wifi_menu)
{
    #define X_POS -38
    wifi_menu->wifi_submenu.cmd_to_send = 1; // Set default
    
    // Create labels
    wifi_menu->wifi_submenu.lbl_send_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.lbl_send_ins, MQTT_CONNECTING_TXT, user_secondary_color,
            &lv_font_montserrat_16, LV_ALIGN_CENTER, X_POS - 10, 48);

    wifi_menu->wifi_submenu.lbl_send_cmd = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.lbl_send_cmd, "1", user_secondary_color,
             &lv_font_montserrat_30, LV_ALIGN_CENTER, X_POS, -20);

    wifi_menu->wifi_submenu.lbl_send_box = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.lbl_send_box, "", user_secondary_color,
            &lv_font_montserrat_24, LV_ALIGN_CENTER, X_POS, -20);
                     
    wifi_menu->wifi_submenu.lbl_send = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.lbl_send, "SEND", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);
    
    wifi_menu->wifi_submenu.lbl_edit = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.lbl_edit, LV_SYMBOL_HOME " EDIT", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_BOTTOM_RIGHT, -5, -4);
                     
    wifi_menu->wifi_submenu.arrow_top = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.arrow_top, LV_SYMBOL_UP, user_secondary_color,
            &lv_font_montserrat_14, LV_ALIGN_CENTER, X_POS, -50);
                     
    wifi_menu->wifi_submenu.arrow_bot = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.arrow_bot, LV_SYMBOL_DOWN, user_secondary_color,
            &lv_font_montserrat_14, LV_ALIGN_CENTER, X_POS, 10);
                     
    wifi_menu->wifi_submenu.lbl_receipt = lv_label_create(ACTIVE_SCR);
    lcd_format_label(wifi_menu->wifi_submenu.lbl_receipt, LV_SYMBOL_OK, user_secondary_color,
            &lv_font_montserrat_30, LV_ALIGN_TOP_RIGHT, -32, 21);

    // Create a style for the send cmd box
    static lv_style_t style_cmd;
    lv_style_init(&style_cmd);

    lv_style_set_radius(&style_cmd, 8);
    lv_style_set_bg_color(&style_cmd, user_primary_color);
    lv_style_set_border_width(&style_cmd, 2);
    lv_style_set_border_color(&style_cmd, user_secondary_color);
    lv_style_set_border_side(&style_cmd, LV_BORDER_SIDE_FULL);
    lv_style_set_text_color(&style_cmd, user_secondary_color);
    
    lv_color_t darker_user_primary_color = lv_color_darken(user_primary_color, 100); // % darker 
    lv_style_set_shadow_spread(&style_cmd, 3);
    lv_style_set_shadow_width(&style_cmd, 6);
    lv_style_set_shadow_offset_x(&style_cmd, 3);
    lv_style_set_shadow_offset_y(&style_cmd, 3);
    lv_style_set_shadow_color(&style_cmd, darker_user_primary_color);
        
    lv_style_set_pad_left(&style_cmd, 55);
    lv_style_set_pad_right(&style_cmd, 55);
    lv_style_set_pad_top(&style_cmd, 25);
    lv_style_set_pad_bottom(&style_cmd, 25);
        
    lv_obj_add_style(wifi_menu->wifi_submenu.lbl_send_box, &style_cmd, 0);
    
    // Create a style for the edit box
    static lv_style_t style_edit;
    lv_style_init(&style_edit);

    lv_style_set_radius(&style_edit, 8);
    lv_style_set_bg_color(&style_edit, user_primary_color);
    lv_style_set_border_width(&style_edit, 2);
    lv_style_set_border_color(&style_edit, user_secondary_color);
    lv_style_set_border_side(&style_edit, LV_BORDER_SIDE_FULL);
    lv_style_set_text_color(&style_edit, user_secondary_color);
    
    lv_style_set_pad_left(&style_edit, 10);
    lv_style_set_pad_right(&style_edit, 10);
    lv_style_set_pad_top(&style_edit, 6);
    lv_style_set_pad_bottom(&style_edit, 6);
    
    lv_obj_add_style(wifi_menu->wifi_submenu.lbl_edit, &style_edit, 0);
    
    // Hide everything for now
    hide_wifi_send_page(wifi_menu);
}

esp_err_t lcd_wifi_menu_nvs_save(const wifi_menu_t *menu)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(WIFI_MENU_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    
    // Number of user options is size - start_size
    uint8_t user_cnt = menu->size - WIFI_MENU_START_SIZE;
    
    // Save user_cnt
    err = nvs_set_u8(h, WIFI_MENU_KEY_COUNT, user_cnt);
    
    // If error, exit
    if (err != ESP_OK)
        goto out;

    // Loop through all and number them: 00, 01, etc.
    for (uint8_t i = 0; i < user_cnt + 1; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), WIFI_MENU_KEY_FMT, i);
        
        // If in range
        if (i < user_cnt) {
            // Store the menu option string at each key starting at index WIFI_MENU_START_SIZE
            err = nvs_set_str(h, key, menu->options[i + WIFI_MENU_START_SIZE]);
        } else { // Not in range: erase
            err = nvs_erase_key(h, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        
        // Exit if error
        if (err != ESP_OK)
            goto out;
    }
    
    // Flush pending writes to flash
    err = nvs_commit(h);

    // Close NVS
    out: nvs_close(h);
    
    return err;
}

esp_err_t lcd_wifi_menu_nvs_load(wifi_menu_t *menu)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(WIFI_MENU_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;

    // Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, WIFI_MENU_KEY_COUNT, &user_cnt);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    menu->size = WIFI_MENU_START_SIZE; // Don't change first 3 options
    menu->index = 0;

    // Loop through all keys
    for (uint8_t i = 0; i < user_cnt; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), WIFI_MENU_KEY_FMT, i);
        size_t len = 0;
        
        // Extract the size of the string
        if (nvs_get_str(h, key, NULL, &len) != ESP_OK) {
            break;
        }

        // Ensure enough memory is available
        char *buf = malloc(len);
        if (!buf)
            break;
        
        // Extract the string
        if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
            free(buf);
            break;
        }

        // Update menu struct
        if (menu->size >= MAX_WIFI_OPTIONS) {
            free(buf);
            break;
        }
        menu->options[menu->size++] = buf;
    }
    
    // Close NVS
    nvs_close(h);
    
    return ESP_OK;
}

esp_err_t lcd_wifi_topic_keys_nvs_save(const wifi_menu_t *menu)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(WIFI_TOPIC_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Save number of topic keys: size - start_size
    uint8_t count = menu->size - WIFI_MENU_START_SIZE;
    err = nvs_set_u8(h, WIFI_TOPIC_KEY_COUNT, count);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    // For every user entry, add the key or erase
    for (int i = 0; i < count + 1; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), WIFI_TOPIC_KEY_FMT, i);
    
        // If in range
        if (i < count) {
            // Save index to NVS
            err = nvs_set_blob(h, key, menu->topic_keys[i], sizeof(menu->topic_keys[i]));
        } else { // Not in range: erase
            err = nvs_erase_key(h, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
    
        // Close NVS on error
        if (err != ESP_OK) {
            nvs_close(h);
            return err;
        }
    }

    // Commit changes
    err = nvs_commit(h);
    
    // Close NVS
    nvs_close(h);
    
    return err;
}

esp_err_t lcd_wifi_topic_keys_nvs_load(wifi_menu_t *menu)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(WIFI_TOPIC_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Get number of topic keys saved
    uint8_t count = 0;
    err = nvs_get_u8(h, WIFI_TOPIC_KEY_COUNT, &count);
    
    // Nothing saved: close
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    // Error: close
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    // Zero out everything first
    memset(menu->topic_keys, 0, sizeof(menu->topic_keys));

    // Pull out each topic key
    for (uint8_t i = 0; i < count; ++i) {
        // Format key
        char key[16];
        snprintf(key, sizeof(key), WIFI_TOPIC_KEY_FMT, i);
        
        size_t len = sizeof(menu->topic_keys[i]);
        
        // Get the topic key and save to index
        err = nvs_get_blob(h, key, menu->topic_keys[i], &len);
        
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            // Hole: leave it zeroed
            continue;
        }
        if (err != ESP_OK || len != sizeof(menu->topic_keys[i])) {
            // Error: close
            nvs_close(h);
            return ESP_ERR_INVALID_STATE;
        }
    }
    
    // Close NVS
    nvs_close(h);

    // Restore menu bookkeeping
    menu->size = count + WIFI_MENU_START_SIZE;
    menu->index = 0;

    return ESP_OK;
}

#ifdef POLYCAST5_WIFI_DUMP_NVS
    void lcd_wifi_dump_menu_nvs(void)
    {
        // Open NVS
        nvs_handle_t h;
        esp_err_t err = nvs_open(WIFI_MENU_NS, NVS_READONLY, &h);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "WIFI_MENU_NS open err %s", esp_err_to_name(err));
            return;
        }
    
        // Get user entries
        uint8_t user_cnt = 0;
        err = nvs_get_u8(h, WIFI_MENU_KEY_COUNT, &user_cnt);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Saved menu entry count = %u", user_cnt);
        } else {
            ESP_LOGW(TAG, "No count key or err=%s", esp_err_to_name(err));
        }
    
        // For every entry
        for (int i = 0; i < user_cnt; ++i) {
            char key[16];
            snprintf(key, sizeof(key), WIFI_MENU_KEY_FMT, i);
    
            // First find out how long the string is
            size_t len = 0;
            err = nvs_get_str(h, key, NULL, &len);
            if (err == ESP_OK && len > 0) {
                // Allocate a buffer and read it back
                char *buf = malloc(len);
                if (buf) {
                    if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
                        ESP_LOGI(TAG, "slot %02d: '%s'", i, buf);
                    } else {
                        ESP_LOGI(TAG, "slot %02d: <read err %s>", i, esp_err_to_name(err));
                    }
                    free(buf);
                } else {
                    ESP_LOGI(TAG, "slot %02d: <malloc failed>", i);
                }
            } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGI(TAG, "slot %02d: <empty>", i);
            } else {
                ESP_LOGI(TAG, "slot %02d: err=%s", i, esp_err_to_name(err));
            }
        }
    
        // Close NVS
        nvs_close(h);
    }


    void lcd_wifi_dump_wifi_topic_nvs(void)
    {
        // Open NVS
        nvs_handle_t h;
        esp_err_t err = nvs_open(WIFI_TOPIC_NS, NVS_READONLY, &h);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "WIFI_TOPIC_NS open err %s", esp_err_to_name(err));
            return;
        }
    
        // Get the saved count
        uint8_t count = 0;
        err = nvs_get_u8(h, WIFI_TOPIC_KEY_COUNT, &count);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Saved topic_key count = %u\n", count);
        } else {
            ESP_LOGW(TAG, "No count key or err=%s\n", esp_err_to_name(err));
        }
    
        // Loop every possible slot
        for (int i = 0; i < MAX_WIFI_OPTIONS; ++i) {
            // Format key
            char key[16];
            snprintf(key, sizeof(key), WIFI_TOPIC_KEY_FMT, i);
    
            size_t len = sizeof(((wifi_menu_t *)0)->topic_keys[0]);
            uint8_t buf[len];
            
            // Get entry
            err = nvs_get_blob(h, key, buf, &len);
            if (err == ESP_OK && len == sizeof(buf)) {
                ESP_LOGI(TAG, "slot %02d: ", i);
                
                for (int b = 0; b < len; b++) {
                    ESP_LOGI(TAG, "%02X", buf[b]);
                }
            } else if (err == ESP_ERR_NVS_NOT_FOUND) {
                 ESP_LOGI(TAG, "slot %02d: <empty>\n", i);
            } else {
                 ESP_LOGI(TAG, "slot %02d: err=%s\n", i, esp_err_to_name(err));
            }
        }
    
        // Close NVS
        nvs_close(h);
    }
#endif

