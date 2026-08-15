#include "polycast5_macros.h"
#include "polycast5_fonts.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#include "core/lv_obj_scroll.h"
#include "font/lv_symbol_def.h"
#include "core/lv_obj_pos.h"
#include "core/lv_obj.h"
#include "misc/lv_style.h"
#include "misc/lv_area.h"
#include "misc/lv_anim.h"
#include "widgets/label/lv_label.h"

#include "tca9535.h"
#include "gpio_task.h"
#include "claw_link.h"
#include "wifi_utils.h"
#include "wifi_task.h"
#include "ai_utils.h"
#include "ai_task.h"
#include "lcd_utils.h"
#include "lcd_anim.h"
#include "lcd_hotkey.h"
#include "lcd_gpio.h"

#include "img_ai_orb_1.h"
#include "img_ai_orb_2.h"
#include "img_ai_orb_3.h"

#define TAG "LCD_GPIO"

extern volatile bool gpio_select_btn_held; // gpio_task.c
extern volatile bool mic_recording; // ai_task.c

gpio_menu_t gpio_menu = {
    .options = {"How It Works", "Terminal", "PolyCast5-Claw", "I2C Scanner"},
    .size = 4,
    .index = 1,
    .cont = NULL,
};

void lcd_gpio_setup_page(gpio_menu_t *menu)
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
    lv_obj_set_flex_flow (menu->cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(menu->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing
    
    // Hide for now
    lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_gpio_update_menu(gpio_menu_t *menu)
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

void lcd_gpio_how_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define HOW_Y_OFFSET 40
    
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
        lv_label_set_text(title_lbl, "How It Works:");
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

        // Set custom text to explain to the user
        const char *instr_text =
                                "PolyCast5 is expandable with external hardware to allow for infinite control possibilities.\n\n"
                                "This device will communicate with the external hardware via I2C through the 'Terminal' option.\n\n"
                                "In the terminal, simply enter a command for the connected hardware to receive and execute.\n\n"
                                "From there, PolyCast5 will also print any responses from the connected hardware for your convenience. "
                                "(e.g. status, errors, etc.)\n\n"
                                "To get started building your own connectible hardware, please visit:\n\n"
                                "polycast5.com/blogs /tutorials/create-external-hardware";
        
        lv_label_set_text(instr_lbl, instr_text);

        lv_timer_handler();

        init = true;
    }
    
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, HOW_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -HOW_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->left_btn) { // Go back
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
            
        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

// Helper function to perform the I2C scan and return found addresses
static void i2c_scan(uint8_t found_addrs[], int max, int *found_count)
{
    *found_count = 0;

    // Wait for then lock I2C bus
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take I2C mutex");
        return;
    }

    // For all standard I2C addresses
    for (uint8_t addr = 0x03; addr < 0x78; ++addr) {
        // Probe the address on the shared bus handle
        esp_err_t ret = i2c_master_probe(i2c_bus_handle, addr, 50);

        // If slave at addr ACKed the address byte
        if (ret == ESP_OK) {
            // Stop once the caller's buffer is full
            if (*found_count >= max) {
                ESP_LOGW(TAG, "I2C scan found more than %d devices, some addresses not reported", max);
                break;
            }
            found_addrs[*found_count] = addr;
            (*found_count)++;

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
#endif
        }
    }

    xSemaphoreGive(xI2CBusMutex); // Release I2C bus
}

void lcd_gpio_scanner_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define I2C_MAX_DEVICES 32 // Max I2C devices
    #define I2C_SCROLL_OFFSET 20 // Scroll per button press

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *status_lbl = NULL;
    static lv_obj_t *addrs_lbl = NULL;

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
        lv_label_set_text(title_lbl, "I2C Scanner");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Status label
        status_lbl = lv_label_create(cont);
        lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(status_lbl, lv_pct(100));
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(status_lbl, user_secondary_color, 0);
        lv_obj_align_to(status_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text(status_lbl, "Press SELECT to scan.\nNote: 0x20 and 0x19 are internal.");

        // Addresses label: hidden initially
        addrs_lbl = lv_label_create(cont);
        lv_label_set_long_mode(addrs_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(addrs_lbl, lv_pct(100));
        lv_obj_set_style_text_font(addrs_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(addrs_lbl, user_secondary_color, 0);
        lv_obj_align_to(addrs_lbl, status_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, -13);
        lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Hide until scan complete

        init = true;
    }

    /* User input */
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, I2C_SCROLL_OFFSET, LV_ANIM_ON); // Scroll up
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -I2C_SCROLL_OFFSET, LV_ANIM_ON); // Scroll down
    } else if (ui_btns->select_btn == 1) {
        lv_label_set_text(status_lbl, "Scanning...");
        lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN);
        
        lv_timer_handler(); // Force update
        
        // Perform scan
        uint8_t found_addrs[I2C_MAX_DEVICES];
        int found_count = 0;
        i2c_scan(found_addrs, I2C_MAX_DEVICES, &found_count);

        // Build concatenated string of devices found
        if (found_count > 0) { // If any
            lv_label_set_text(status_lbl, "Found devices:");
            lv_obj_remove_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Show

            // Build the comma-separated string
            POLYCAST5_USE_PSRAM_BSS static char i2c_addr_str[256] = {0}; // String buffer: 0-out
            memset(i2c_addr_str, 0, sizeof(i2c_addr_str));
            for (int i = 0; i < found_count; ++i) {
                char buf[5];
                snprintf(buf, sizeof(buf), "0x%02X", found_addrs[i]);
                
                // Append to i2c_addr_str
                strcat(i2c_addr_str, buf);
                
                // If not last, add comma
                if (i < found_count - 1) {
                    strcat(i2c_addr_str, ", ");
                }
            }
            
            // Update text
            lv_label_set_text(addrs_lbl, i2c_addr_str);
        } else { // Else no devices found
            lv_label_set_text(status_lbl, "No devices found.");
            lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Hide
        }

        lv_timer_handler(); // Force update
    } else if (ui_btns->left_btn == 1) { // Exit
        // Clean up
        lv_obj_delete(cont); // Deletes children
        cont = title_lbl = status_lbl = addrs_lbl = NULL;
        init = false;

        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        // Clean up
        lv_obj_delete(cont);
        cont = title_lbl = status_lbl = addrs_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu);  // True = home, false = sleep
    }
}

// Safe log append
static void term_log_append(char *dst, size_t cap, const char *line)
{
    // Exit early if bad
    if (!dst || !line || cap == 0) {
        return;
    }
    
    strlcat(dst, line, cap);
}

void lcd_gpio_terminal_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define TERMINAL_SLAVE_ADDR 0x2A // Assumed external slave address
    #define TERMINAL_MAX_CMD 255 // Max command value (uint8_t)
 
    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *log_lbl = NULL;
    static uint8_t current_cmd = 0; // Current command number (0-255)
    POLYCAST5_USE_PSRAM_BSS static char log_buffer[2048] = {0}; // Buffer for terminal log (append-only)
 
     // Do once
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
        lv_label_set_text(title_lbl, "I2C Terminal");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instructions label (scrollable if text is long)
        log_lbl = lv_label_create(cont);
        lv_label_set_long_mode(log_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(log_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(log_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(log_lbl, user_secondary_color, 0);
        lv_obj_align_to(log_lbl, title_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        
        lv_label_set_text(log_lbl, "Use up/down to adjust.\nPress select to send.");

        lv_timer_handler();
 
        init = true;
    }

    /* User input */
    if (ui_btns->up_btn == 1) {
        // Increment cmd with wrap
        current_cmd = (current_cmd < TERMINAL_MAX_CMD) ? current_cmd + 1 : 0;
        
        // Show updated
        char msg[64];
        snprintf(msg, sizeof(msg), "Command: %d\n", current_cmd);
        term_log_append(log_buffer, sizeof(log_buffer), msg);

        lv_label_set_text(log_lbl, log_buffer);
        lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        // Decrement cmd with wrap
        current_cmd = (current_cmd > 0) ? current_cmd - 1 : TERMINAL_MAX_CMD;
        
        // Show updated
        char msg[64];
        snprintf(msg, sizeof(msg), "Command: %d\n", current_cmd);
        term_log_append(log_buffer, sizeof(log_buffer), msg);
        
        lv_label_set_text(log_lbl, log_buffer);

        lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
    } else if (ui_btns->select_btn == 1) {
        #define TERMINAL_MAX_RESPONSE_LEN 126 // Match slave
        #define TERMINAL_FIXED_LEN (1 + TERMINAL_MAX_RESPONSE_LEN) // Fixed read size
    
        // Take mutex for I2C access
        if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            esp_err_t ret = ESP_OK;
            POLYCAST5_USE_PSRAM_BSS static char msg[256]; // Buffer for logging
            memset(msg, 0, sizeof(msg));

            // Add terminal slave device to bus
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = TERMINAL_SLAVE_ADDR,
                .scl_speed_hz = I2C_MASTER_FREQ_HZ,
            };
            i2c_master_dev_handle_t term_dev = NULL;
            ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &term_dev);
            if (ret != ESP_OK) {
                snprintf(msg, sizeof(msg), "Add device failed: %s\n\n", esp_err_to_name(ret));
                term_log_append(log_buffer, sizeof(log_buffer), msg);
                xSemaphoreGive(xI2CBusMutex);
                lv_label_set_text(log_lbl, log_buffer);
                lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
                return;
            }

            // WRITE transaction (send command)
            uint8_t cmd_byte = current_cmd;
            ret = i2c_master_transmit(term_dev, &cmd_byte, 1, 150);

            // Log confirmation
            snprintf(msg, sizeof(msg), "\nSent: %u (0x%02X) to 0x%X\n", current_cmd, current_cmd, TERMINAL_SLAVE_ADDR);
            term_log_append(log_buffer, sizeof(log_buffer), msg);

            if (ret != ESP_OK) {
                snprintf(msg, sizeof(msg), "Write failed: %s\n\n", esp_err_to_name(ret));
                term_log_append(log_buffer, sizeof(log_buffer), msg);

                // Early exit on write error
                i2c_master_bus_rm_device(term_dev);
                xSemaphoreGive(xI2CBusMutex); // Release I2C bus
                lv_label_set_text(log_lbl, log_buffer);
                lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
                return;
            }

            // Delay to allow slave processing time
            vTaskDelay(pdMS_TO_TICKS(50));

            uint8_t response_buf[TERMINAL_FIXED_LEN] = {0};

            // Single READ transaction (exactly TERMINAL_FIXED_LEN bytes)
            ret = i2c_master_receive(term_dev, response_buf, TERMINAL_FIXED_LEN, 150);

            // If read something
            if (ret == ESP_OK) {
                uint8_t response_len = response_buf[0]; // First byte is length (from receiver example)

                // len == 0 -> empty
                if (response_len == 0) {
                    snprintf(msg, sizeof(msg), "Empty response.\n\n");
                } else if (response_len > TERMINAL_MAX_RESPONSE_LEN) { // Response too big
                    snprintf(msg, sizeof(msg), "Invalid response length: %u\n\n", response_len);
                } else { // Else valid response
                    char response_str[TERMINAL_MAX_RESPONSE_LEN + 1] = {0};
                    memcpy(response_str, &response_buf[1], response_len); // Copy exactly len bytes
                    response_str[response_len] = '\0'; // NUL-terminate
                    snprintf(msg, sizeof(msg), "Response: %s\n\n", response_str);
                }
            } else {
                snprintf(msg, sizeof(msg), "Read failed: %s\n\n", esp_err_to_name(ret));
            }

            // Append to log
            term_log_append(log_buffer, sizeof(log_buffer), msg);

            // Remove terminal device from bus
            i2c_master_bus_rm_device(term_dev);
            xSemaphoreGive(xI2CBusMutex); // Release I2C bus

            // Show and scroll to bottom
            lv_label_set_text(log_lbl, log_buffer);
            lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
        } else {
            // Mutex timeout - append error
            term_log_append(log_buffer, sizeof(log_buffer), "\nI2C bus busy - timed out.\n\n");

            // Show and scroll to bottom
            lv_label_set_text(log_lbl, log_buffer);
            lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
        }
    } else if (ui_btns->left_btn == 1) { // Exit
        // Clean up
        lv_obj_delete(cont); // Deletes children
        cont = title_lbl = log_lbl = NULL;
        init = false;
        memset(log_buffer, 0, sizeof(log_buffer)); // Clear log for next entry
        current_cmd = 0;
 
        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
 
        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        // Clean up
        lv_obj_delete(cont); // Deletes children
        cont = title_lbl = log_lbl = NULL;
        init = false;
        memset(log_buffer, 0, sizeof(log_buffer)); // Clear log for next entry
        current_cmd = 0;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_gpio_claw_how_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define CLAW_HOW_Y_OFFSET 40

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
        lv_label_set_text(title_lbl, "PolyCast5-Claw:");
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

        // Set custom text to explain to the user
        const char *instr_text =
                                "Press RIGHT to skip.\n\n"
                                "PolyCast5-Claw is a separate hardware expansion that plugs into this device. "
                                "It is REQUIRED for this page to do anything.\n\n"
                                "Hold SELECT and speak. PolyCast5 transcribes what you said, then sends the text "
                                "to the expansion over I2C.\n\n"
                                "The expansion runs its own on-device AI agent, which writes and executes a script "
                                "to actually carry the command out. "
                                "(e.g. \"blink the LED on GPIO 36 ten times\")\n\n"
                                "Nothing needs setting up on the expansion. It joins the same Wi-Fi network as "
                                "PolyCast5 and reuses the same API key automatically.\n\n"
                                "To get started, please visit:\n\n"
                                "polycast5.com/blogs /tutorials/what-is-polycast5-claw";

        lv_label_set_text(instr_lbl, instr_text);

        lv_timer_handler();

        init = true;
    }

    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, CLAW_HOW_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -CLAW_HOW_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->right_btn == 1) { // Skip to GPIO_CLAW_PAGE
        // Hide right arrow: the Claw page itself has nothing to the right
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        // Switch pages
        ui_menu->page = GPIO_CLAW_PAGE;
    } else if (ui_btns->left_btn) { // Go back
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

/* PolyCast5-Claw page.
 *
 * Same shape as the Bluetooth AI keyboard page: hold SELECT, the orb spins and pulses
 * while the mic records, then the transcript is handed off. Here it goes to the Claw
 * expansion over I2C instead of being typed over BLE.
 *
 * Unlike the other pages in this file the widgets and state live at file scope, so the
 * exit paths can share one teardown helper rather than repeating it three times.
 */
typedef enum {
    CLAW_STATE_NONE = 0,
    CLAW_STATE_NO_HW, // No expansion answered on the bus
    CLAW_STATE_PROVISIONING, // Credentials sent, waiting for the expansion to come up
    CLAW_STATE_IDLE, // Ready for a command
    CLAW_STATE_RECORDING, // Mic capturing while SELECT is held
    CLAW_STATE_STT_WAITING, // Transcribing
    CLAW_STATE_RUNNING, // Expansion working; polling it for progress
    CLAW_STATE_DONE, // Final answer on screen
    CLAW_STATE_ERROR, // Unrecoverable for this page visit
} claw_page_state_t;

static bool claw_do_once = false;
static lv_obj_t *claw_lbl_ins = NULL; // Centered short status text
static lv_obj_t *claw_orb = NULL;
static lv_obj_t *claw_cont = NULL; // Scrollable container for expansion output
static lv_obj_t *claw_lbl_out = NULL;
static int16_t claw_orb_angle = 0; // 0.1 degree units
static claw_page_state_t claw_state = CLAW_STATE_NONE;
static bool claw_last_select = false;
static uint8_t claw_last_seq = 0; // Last display-text revision seen from the expansion
static uint32_t claw_wait_ms = 0; // Time spent in the current waiting state

POLYCAST5_USE_PSRAM_BSS static char claw_raw_buf[CLAW_LINK_TEXT_MAX + 1];
POLYCAST5_USE_PSRAM_BSS static char claw_out_buf[CLAW_LINK_TEXT_MAX + 1];

// Strip anything non-ASCII out of expansion text. The agent prefixes its progress
// messages with emoji, which the montserrat fonts have no glyphs for and would draw as
// placeholder boxes. Runs of dropped bytes collapse into a single space.
static void claw_text_to_ascii(char *dst, size_t cap, const char *src)
{
    size_t out = 0;
    bool dropped = false;

    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] != '\0' && out + 1 < cap; ++i) {
        unsigned char c = (unsigned char)src[i];

        // Drop every byte of a multi-byte sequence, and any stray control character
        if (c >= 0x80 || (c < 0x20 && c != '\n')) {
            dropped = true;
            continue;
        }

        // Collapse whatever was dropped into one space, but never lead with it
        if (dropped) {
            dropped = false;
            if (out > 0 && c != ' ' && c != '\n' && dst[out - 1] != ' ' && dst[out - 1] != '\n') {
                dst[out++] = ' ';
                if (out + 1 >= cap) {
                    break;
                }
            }
        }

        dst[out++] = (char)c;
    }

    dst[out] = '\0';
}

// Show a short centered message and hide the orb and output container
static void claw_show_status(const char *txt, const lv_font_t *font)
{
    lv_obj_add_flag(claw_orb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(claw_cont, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(claw_lbl_ins, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(claw_lbl_ins, font, 0);
    lv_label_set_text(claw_lbl_ins, txt);
    lv_obj_align(claw_lbl_ins, LV_ALIGN_CENTER, 0, 0);
}

// Show scrollable text that came back from the expansion
static void claw_show_output(const char *txt)
{
    lv_obj_add_flag(claw_orb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(claw_lbl_ins, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(claw_cont, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(claw_lbl_out, txt);
    lv_obj_scroll_to_y(claw_cont, 0, LV_ANIM_OFF); // New text starts at the top
}

// Shared exit path: stop anything in flight, drop the widgets and reset every static.
// disconnect_wifi is false when the page is only re-initializing itself and would just
// have to pay the reconnect cost again.
static void claw_page_teardown(ui_menu_t *ui_menu, bool disconnect_wifi)
{
    // Stop mic_recording; if a capture is in progress, have ai_task tear down the mic and free the abandoned recording (no STT)
    mic_recording = false;
    if (claw_state == CLAW_STATE_RECORDING) {
        ai_cmd_t abort_cmd = {
            .type = AI_CMD_CLAW_ABORT_REC,
        };
        if (xQueueSend(xAiCmdQueue, &abort_cmd, pdMS_TO_TICKS(100)) != pdPASS) {
            ESP_LOGE(TAG, "Failed: xAiCmdQueue AI_CMD_CLAW_ABORT_REC");
        }
    }

    // Let the expansion drop whatever it was still working on for us
    if (claw_state == CLAW_STATE_RUNNING) {
        claw_link_abort();
    }

    // Disconnect from Wi-Fi
    if (disconnect_wifi) {
        xEventGroupSetBits(xWifiEventGroup, WIFI_DISCONNECT_BIT);
    }

    lcd_anim_loading_stop();

    // Delete objects
    if (claw_lbl_ins != NULL) {
        lv_obj_delete(claw_lbl_ins);
    }
    if (claw_orb != NULL) {
        lv_obj_delete(claw_orb);
    }
    if (claw_cont != NULL) {
        lv_obj_delete(claw_cont); // Deletes children
    }

    // Reset statics
    claw_lbl_ins = claw_orb = claw_cont = claw_lbl_out = NULL;
    claw_do_once = false;
    claw_state = CLAW_STATE_NONE;
    claw_last_select = false;
    claw_orb_angle = 0;
    claw_last_seq = 0;
    claw_wait_ms = 0;

    // Hide right arrow
    lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
}

void lcd_gpio_claw_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    #define CLAW_NO_HW_TXT "Claw not detected!\n\nConnect the expansion,\nthen press SELECT to\nretry."
    #define CLAW_NO_KEY_TXT "No xAI API key set!\n\nAdd one under\nBluetooth > AI Keyboard\nsettings."
    #define CLAW_WIFI_FAILED_TXT "Connection failed!\nPlease connect to your\nWi-Fi network at least\nonce in the 'Wi-Fi'\nmenu and make sure\nyou are in range."
    #define CLAW_LINK_FAILED_TXT "Lost the expansion!\nCheck the connection."
    #define CLAW_SETUP_FAILED_TXT "Claw setup failed!\nIt could not join\nyour Wi-Fi network."
    #define CLAW_TIMEOUT_TXT "Claw timed out!\nPlease try again."
    #define CLAW_CREDITS_TXT "Out of API credits!\nCheck your usage:\nconsole.x.ai"
    #define CLAW_STT_FAILED_TXT "Thinking failed!\nPlease try again."
    #define CLAW_WIFI_CONNECTING_TXT "Connecting to Wi-Fi..."
    #define CLAW_HOLD_TALK_TXT "Hold & talk!"
    #define CLAW_THINKING_TXT "Thinking..."

    #define CLAW_TICK_MS 200 // lcd_task polls pages at this rate
    #define CLAW_PROVISION_TIMEOUT_MS 30000 // Expansion has this long to join Wi-Fi and take the key
    #define CLAW_RUN_TIMEOUT_MS 180000 // A full agent turn with tool calls can take a while
    #define CLAW_SCROLL_OFFSET 20 // Scroll per button press

    // Only execute once
    if (!claw_do_once) {
        // If picking this page as a hotkey
        if (!lv_obj_has_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN)) {
            lcd_hotkey_save_page_as_hotkey(ui_menu); // Save as a hotkey
        }

        lv_timer_handler(); // Update

        // Clear any previous states
        xEventGroupClearBits(xAiEventGroup, AI_DONE_THINKING_BIT | AI_THINKING_FAILED_BIT |
                AI_RATE_LIMITED_BIT | AI_NO_MATCH_BIT | AI_CLAW_SENT_BIT | AI_CLAW_LINK_FAILED_BIT);
        xQueueReset(xAiSoundHeardSemaphore);

        lcd_anim_label_x_animate_reset();
        lcd_anim_label_y_animate_reset();

        LCD_LOADING_ANIM_START_DEFAULT();

        // Centered short status text
        claw_lbl_ins = lv_label_create(ACTIVE_SCR);
        lv_obj_set_style_text_align(claw_lbl_ins, LV_TEXT_ALIGN_CENTER, 0); // Centered text style
        lcd_format_label(claw_lbl_ins, CLAW_WIFI_CONNECTING_TXT, user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 0);

        claw_orb = lv_image_create(ACTIVE_SCR);
        lv_image_set_src(claw_orb, &img_ai_orb_1);
        lv_obj_align(claw_orb, LV_ALIGN_CENTER, 0, 0);

        lv_obj_update_layout(claw_orb); // Save current layout

        // Set pivot to center so it spins around its middle
        int w = lv_obj_get_width(claw_orb);
        int h = lv_obj_get_height(claw_orb);
        lv_obj_set_style_transform_pivot_x(claw_orb, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(claw_orb, h / 2, 0);

        // Give some extra draw area so rotation isn't clipped
        lv_obj_set_style_transform_width(claw_orb, 8, 0);
        lv_obj_set_style_transform_height(claw_orb, 8, 0);

        // Hide orb for now
        lv_obj_add_flag(claw_orb, LV_OBJ_FLAG_HIDDEN);

        // Scrollable container for whatever the expansion sends back
        claw_cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(claw_cont, 210, 106);
        lv_obj_center(claw_cont);
        lv_obj_set_style_bg_color(claw_cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(claw_cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(claw_cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(claw_cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Rounded corners for appeal
        lv_obj_set_style_shadow_width(claw_cont, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(claw_cont, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_scrollbar_mode(claw_cont, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(claw_cont, LV_DIR_VER);
        lv_obj_set_style_pad_all(claw_cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT); // Padding for content
        lv_obj_add_flag(claw_cont, LV_OBJ_FLAG_HIDDEN);

        claw_lbl_out = lv_label_create(claw_cont);
        lv_label_set_long_mode(claw_lbl_out, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(claw_lbl_out, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(claw_lbl_out, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(claw_lbl_out, user_secondary_color, 0);
        lv_obj_align(claw_lbl_out, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(50)); // Allow time to render

        // Default states
        claw_state = CLAW_STATE_NONE;
        claw_last_select = false;
        claw_last_seq = 0;
        claw_wait_ms = 0;
        claw_do_once = true;

        // Is the expansion actually plugged in?
        if (claw_link_probe() != ESP_OK) {
            claw_show_status(CLAW_NO_HW_TXT, &lv_font_montserrat_14);
            claw_state = CLAW_STATE_NO_HW;
        }

        // PolyCast5 needs Wi-Fi for STT regardless, and the expansion inherits these credentials
        if (claw_state == CLAW_STATE_NONE && !(xEventGroupGetBits(xWifiEventGroup) & WIFI_CONNECTED_BIT)) {
            xEventGroupSetBits(xWifiEventGroup, WIFI_RECONNECT_BIT);

            // Wait up to WIFI_CONN_TIMEOUT_MS for Wi-Fi to connect
            uint8_t status = lcd_wait_for_bit_better(xWifiEventGroup, WIFI_CONNECTED_BIT, WIFI_CONN_TIMEOUT_MS);
            if (status == LCD_WAIT_FOR_BIT_BETTER_TIMEOUT) { // Timeout
                claw_show_status(CLAW_WIFI_FAILED_TXT, &lv_font_montserrat_14);
                claw_state = CLAW_STATE_ERROR;
            } else if (status == LCD_WAIT_FOR_BIT_BETTER_EXIT) { // Exit
                ui_btns->left_btn = 1; // Simulate left press to go back
                claw_state = CLAW_STATE_ERROR;
            }
        }

        // Hand over Wi-Fi and API credentials so nothing has to be configured on the expansion
        if (claw_state == CLAW_STATE_NONE) {
            // Kept off the stack: the LCD task already carries LVGL
            POLYCAST5_USE_PSRAM_BSS static char api_key[AI_API_KEY_MAX_LEN];
            memset(api_key, 0, sizeof(api_key));

            // Without a key STT would fail here anyway, so say so plainly
            if (ai_utils_load_api_key_nvs(api_key, sizeof(api_key)) != ESP_OK || api_key[0] == '\0') {
                claw_show_status(CLAW_NO_KEY_TXT, &lv_font_montserrat_14);
                claw_state = CLAW_STATE_ERROR;
            } else {
                // Credentials of the network we just confirmed we are on
                wifi_login_t net = wifi_utils_get_prev();

                // Wi-Fi first: the expansion needs a network before the key is any use
                esp_err_t err = claw_link_send_wifi(net.ssid, net.password);
                if (err == ESP_OK) {
                    err = claw_link_send_llm(api_key, XAI_MODEL, XAI_BASE_URL);
                }

                // Sent on every page entry, so switching networks or rotating the key
                // carries over without touching the expansion
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Claw provisioning failed: %s", esp_err_to_name(err));
                    claw_show_status(CLAW_LINK_FAILED_TXT, &lv_font_montserrat_16);
                    claw_state = CLAW_STATE_ERROR;
                } else {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Linking Claw to\n%s...", net.ssid);
                    claw_show_status(msg, &lv_font_montserrat_16);

                    claw_state = CLAW_STATE_PROVISIONING;
                    claw_wait_ms = 0;
                }
            }
        }
    }

    // Get physical held select button state
    bool select_now = gpio_select_btn_held;
    bool select_pressed = (select_now && !claw_last_select);
    bool select_released = (!select_now && claw_last_select);
    claw_last_select = select_now;

    /* Errors surfaced by ai_task: show them, then let the user simply try again.
     * Each drops back to IDLE, so the message stays up until the next hold */
    if (xEventGroupGetBits(xAiEventGroup) & AI_RATE_LIMITED_BIT) {
        xEventGroupClearBits(xAiEventGroup, AI_RATE_LIMITED_BIT);
        claw_show_status(CLAW_CREDITS_TXT, &lv_font_montserrat_16);
        claw_state = CLAW_STATE_IDLE;
    } else if (xEventGroupGetBits(xAiEventGroup) & AI_THINKING_FAILED_BIT) { // STT failed
        xEventGroupClearBits(xAiEventGroup, AI_THINKING_FAILED_BIT);
        claw_show_status(CLAW_STT_FAILED_TXT, &lv_font_montserrat_16);
        claw_state = CLAW_STATE_IDLE;
    } else if (xEventGroupGetBits(xAiEventGroup) & AI_CLAW_LINK_FAILED_BIT) { // Transcribed, but the I2C hand-off failed
        xEventGroupClearBits(xAiEventGroup, AI_CLAW_LINK_FAILED_BIT);
        claw_show_status(CLAW_LINK_FAILED_TXT, &lv_font_montserrat_16);
        claw_state = CLAW_STATE_IDLE;
    } else if ((xEventGroupGetBits(xAiEventGroup) & AI_CLAW_SENT_BIT) && claw_state == CLAW_STATE_STT_WAITING) {
        xEventGroupClearBits(xAiEventGroup, AI_CLAW_SENT_BIT);

        // Echo what was heard until the expansion starts reporting progress of its own
        claw_text_to_ascii(claw_out_buf, sizeof(claw_out_buf), claw_link_last_command());
        claw_show_output(claw_out_buf);

        // Seq 0 forces a redraw on the expansion's first real update
        claw_last_seq = 0;
        claw_wait_ms = 0;
        claw_state = CLAW_STATE_RUNNING;
    } else if ((claw_state == CLAW_STATE_IDLE || claw_state == CLAW_STATE_DONE) && select_pressed) { // Initial press
        // Start mic_recording
        mic_recording = true;
        ai_cmd_t cmd = {
            .type = AI_CMD_CLAW_START_REC,
        };

        // Actually send it
        if (xQueueSend(xAiCmdQueue, &cmd, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed: xAiCmdQueue AI_CMD_CLAW_START_REC");
            mic_recording = false;
            claw_state = CLAW_STATE_IDLE;
        } else {
            // Show orb, hide everything else
            lv_obj_add_flag(claw_lbl_ins, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(claw_cont, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(claw_orb, LV_OBJ_FLAG_HIDDEN);

            claw_state = CLAW_STATE_RECORDING;
        }
    }

    // While held: spin the orb - stop only on release
    if (claw_state == CLAW_STATE_RECORDING) {
        // Rotate the orb
        claw_orb_angle = (claw_orb_angle + 50) % 3600; // 5 degrees per frame
        lv_obj_set_style_transform_rotation(claw_orb, claw_orb_angle, 0);

        // When done capturing
        if (select_released) {
            // Stop mic_recording
            mic_recording = false;
            ai_cmd_t cmd = {
                .type = AI_CMD_CLAW_DONE_REC,
            };

            // Actually send it
            if (xQueueSend(xAiCmdQueue, &cmd, portMAX_DELAY) != pdPASS) {
                ESP_LOGE(TAG, "Failed: xAiCmdQueue AI_CMD_CLAW_DONE_REC");
                claw_state = CLAW_STATE_IDLE;
            } else {
                claw_show_status(CLAW_THINKING_TXT, &lv_font_montserrat_22);
                claw_state = CLAW_STATE_STT_WAITING; // Waiting for the transcript to be handed over
            }
        }
    }

    // Pulse orb on sound heard
    if ((xSemaphoreTake(xAiSoundHeardSemaphore, 0) == pdPASS) && claw_state == CLAW_STATE_RECORDING) {
        for (int i = 0; i < 5; ++i) {
            if      (i == 0) lv_image_set_src(claw_orb, &img_ai_orb_1);
            else if (i == 1) lv_image_set_src(claw_orb, &img_ai_orb_2);
            else if (i == 2) lv_image_set_src(claw_orb, &img_ai_orb_3);
            else if (i == 3) lv_image_set_src(claw_orb, &img_ai_orb_2);
            else if (i == 4) lv_image_set_src(claw_orb, &img_ai_orb_1);
            lv_obj_update_layout(claw_orb);
            int w = lv_obj_get_width(claw_orb);
            int h = lv_obj_get_height(claw_orb);
            lv_obj_set_style_transform_pivot_x(claw_orb, w / 2, 0);
            lv_obj_set_style_transform_pivot_y(claw_orb, h / 2, 0);
            lv_obj_align(claw_orb, LV_ALIGN_CENTER, 0, 0);
            lv_refr_now(NULL);
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }
    }

    // Waiting for the expansion to come up on the host's network with the host's API key
    if (claw_state == CLAW_STATE_PROVISIONING) {
        claw_link_status_t status = {0};

        // Both flags: a network alone isn't enough to answer a command
        if (claw_link_poll(&status) == ESP_OK &&
                (status.flags & CLAW_LINK_FLAG_WIFI_CONNECTED) &&
                (status.flags & CLAW_LINK_FLAG_LLM_CONFIGURED)) {
            claw_show_status(CLAW_HOLD_TALK_TXT, &lv_font_montserrat_22);
            claw_state = CLAW_STATE_IDLE;
        } else {
            // Joining a network takes a few seconds, so count ticks rather than give up
            claw_wait_ms += CLAW_TICK_MS;
            if (claw_wait_ms >= CLAW_PROVISION_TIMEOUT_MS) {
                claw_show_status(CLAW_SETUP_FAILED_TXT, &lv_font_montserrat_14);
                claw_state = CLAW_STATE_ERROR;
            }
        }
    }

    // Poll the expansion for live progress, then the final answer
    if (claw_state == CLAW_STATE_RUNNING) {
        claw_link_status_t status = {0};

        // A failed poll here usually means the expansion was unplugged mid-run
        if (claw_link_poll(&status) != ESP_OK) {
            claw_show_status(CLAW_LINK_FAILED_TXT, &lv_font_montserrat_16);
            claw_state = CLAW_STATE_IDLE;
        } else {
            // Only redraw when the expansion says its text actually changed
            if (status.seq != claw_last_seq) {
                claw_last_seq = status.seq;

                // Multi-page read, so this is worth doing only on a real change
                if (claw_link_read_result(claw_raw_buf, sizeof(claw_raw_buf)) == ESP_OK) {
                    claw_text_to_ascii(claw_out_buf, sizeof(claw_out_buf), claw_raw_buf);
                    claw_show_output(claw_out_buf);
                }
            }

            if (status.state == CLAW_LINK_STATE_DONE || status.state == CLAW_LINK_STATE_ERROR) {
                // Whatever it ended with is already on screen
                claw_state = CLAW_STATE_DONE;
            } else {
                // Tool-calling turns are slow, so this only catches a truly wedged agent
                claw_wait_ms += CLAW_TICK_MS;
                if (claw_wait_ms >= CLAW_RUN_TIMEOUT_MS) {
                    claw_link_abort();
                    claw_show_status(CLAW_TIMEOUT_TXT, &lv_font_montserrat_16);
                    claw_state = CLAW_STATE_IDLE;
                }
            }
        }
    }

    /* User input */
    // Up/down only scroll when the expansion's output is the thing on screen
    if (ui_btns->up_btn == 1 && !lv_obj_has_flag(claw_cont, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_scroll_by_bounded(claw_cont, 0, CLAW_SCROLL_OFFSET, LV_ANIM_ON); // Scroll up
    } else if (ui_btns->down_btn == 1 && !lv_obj_has_flag(claw_cont, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_scroll_by_bounded(claw_cont, 0, -CLAW_SCROLL_OFFSET, LV_ANIM_ON); // Scroll down
    } else if (ui_btns->select_btn == 1 && claw_state == CLAW_STATE_NO_HW) { // Retry the probe
        if (claw_link_probe() == ESP_OK) {
            // Tear down so the next tick re-runs init and provisions the expansion.
            // Wi-Fi is kept: re-init would only have to pay the reconnect cost again
            claw_page_teardown(ui_menu, false);
        }
    } else if (ui_btns->left_btn == 1) { // Back selected
        claw_page_teardown(ui_menu, true);

        // Show GPIO menu
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        gpio_menu->index = 2; // Set to PolyCast5-Claw index
        lcd_gpio_update_menu(gpio_menu); // Update

        // Switch back
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        claw_page_teardown(ui_menu, true);

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}
