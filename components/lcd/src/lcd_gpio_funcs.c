#include "polycast5_macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"

#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c.h"

#include "core/lv_obj_scroll.h"
#include "font/lv_symbol_def.h"
#include "core/lv_obj_pos.h"
#include "core/lv_obj.h"
#include "misc/lv_style.h"
#include "misc/lv_area.h"
#include "widgets/label/lv_label.h"

#include "tca9535.h"
#include "gpio_task.h"
#include "lcd_utils.h"
#include "lcd_gpio_funcs.h"

#define TAG "LCD_GPIO"

#define I2C_MASTER_FREQ_HZ 100000  // Standard 100kHz; match your existing init
#define TCA9535_ADDR 0x20  // Adjust to your TCA9535's actual address (exclude from scan if desired)

gpio_menu_t gpio_menu = {
	.options = {"How It Works", "Terminal", "I2C Scanner"},
	.size = 3,
	.index = 1,
	.cont = NULL,
};

void lcd_gpio_setup_page(gpio_menu_t *menu)
{
	// Create list
	menu->main_list = lv_list_create(ACTIVE_SCR);
	lv_obj_set_size(menu->main_list, 210, 106);
	
	// Format
	lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF); // Never draw bars
	lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_scrollbar_mode(menu->main_list, LV_SCROLLBAR_MODE_OFF);
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
	}
	else if (menu->index < 0) {
		menu->index = menu->size - 1;
	}
	
	// Create button for each option
	for (int i = 0; i < menu->size; ++i) {

		menu->btns[i] = lv_list_add_btn(menu->main_list, NULL, menu->options[i]);
		lv_obj_set_size(menu->btns[i], 200, 30);

		// Style selected
		if (i == menu->index) {
			lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
		}
		else {
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
	}
	else if (menu->index < 0) {
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
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -HOW_Y_OFFSET, LV_ANIM_ON);
	}
	// Go back
	else if (ui_btns->left_btn) {
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
	}
	// Home or power off
	else if (ui_btns->home_btn || ui_btns->pwr_btn) {
		// Delete objects
		lv_obj_delete(cont); // Deletes children
		
		// Reset statics
		cont = NULL;
		title_lbl = instr_lbl = NULL;
		init = false;
		
 		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}

// Helper function to perform the I2C scan and return found addresses
static void i2c_scan(uint8_t found_addrs[], int *found_count)
{
	*found_count = 0;
	i2c_cmd_handle_t cmd = NULL;

	// Wait for then lock I2C bus
	if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
		ESP_LOGE(TAG, "Failed to take I2C mutex");
		return;
	}

	// For all standard I2C addresses
	for (uint8_t addr = 0x03; addr < 0x78; ++addr) {
		/*
		// Skip internal TCA9535 address
		if (addr == TCA9535_ADDR) {
			continue;
		}
		*/
		
		// Prepare a custom I2C transaction
		cmd = i2c_cmd_link_create();
		
		i2c_master_start(cmd); // START command: begin a transaction and alert all slaves on the bus to listen
		i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true); // Probe the address, expect ACK
		i2c_master_stop(cmd); // STOP command -> no further data

		// Execute the entire command link
		esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(50));
		i2c_cmd_link_delete(cmd); // Done

		// If slave at addr ACKed the address byte
		if (ret == ESP_OK) {
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
		lv_label_set_text(status_lbl, "Press SELECT to scan.\nNote: 0x20 is internal IO.");

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
	}
	else if (ui_btns->down_btn == 1) {
		lv_obj_scroll_by_bounded(cont, 0, -I2C_SCROLL_OFFSET, LV_ANIM_ON); // Scroll down
	}
	else if (ui_btns->select_btn == 1) {
		lv_label_set_text(status_lbl, "Scanning...");
		lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN);
		
		lv_timer_handler(); // Force update
		
		// Perform scan
		uint8_t found_addrs[I2C_MAX_DEVICES];
		int found_count = 0;
		i2c_scan(found_addrs, &found_count);

		// Build concatenated string of devices found
		if (found_count > 0) { // If any
			lv_label_set_text(status_lbl, "Found devices:");
			lv_obj_remove_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Show

			// Build the comma-separated string
			char i2c_addr_str[256] = {0}; // String buffer: 0-out
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
		}
		// Else no devices found
		else {
			lv_label_set_text(status_lbl, "No devices found.");
			lv_obj_add_flag(addrs_lbl, LV_OBJ_FLAG_HIDDEN); // Hide
		}

		lv_timer_handler(); // Force update
	}
	// Exit
	else if (ui_btns->left_btn == 1) {
		// Clean up
		lv_obj_delete(cont); // Deletes children
		cont = title_lbl = status_lbl = addrs_lbl = NULL;
		init = false;

		// Show GPIO menu
		lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);

		// Switch back
		ui_menu->page = GPIO_PAGE;
	}
	// Home or power off
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Clean up
		lv_obj_delete(cont);
		cont = title_lbl = status_lbl = addrs_lbl = NULL;
		init = false;

		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu);  // True = home, false = sleep
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
	static char log_buffer[2048] = {0}; // Buffer for terminal log (append-only)
 
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
	}
	else if (ui_btns->down_btn == 1) {
		// Decrement cmd with wrap
		current_cmd = (current_cmd > 0) ? current_cmd - 1 : TERMINAL_MAX_CMD;
		
		// Show updated
		char msg[64];
		snprintf(msg, sizeof(msg), "Command: %d\n", current_cmd);
		term_log_append(log_buffer, sizeof(log_buffer), msg);
		
		lv_label_set_text(log_lbl, log_buffer);

		lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
	}
	else if (ui_btns->select_btn == 1) {
		#define TERMINAL_MAX_RESPONSE_LEN 126  // Match slave
		#define TERMINAL_FIXED_LEN (1 + TERMINAL_MAX_RESPONSE_LEN)  // Fixed read size
	
		// Take mutex for I2C access
		if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
			esp_err_t ret = ESP_OK;
			char msg[256]; // Buffer for logging
	
			// WRITE transaction (send command, end with STOP)
			i2c_cmd_handle_t cmd_write = i2c_cmd_link_create();
			i2c_master_start(cmd_write);
			i2c_master_write_byte(cmd_write, (TERMINAL_SLAVE_ADDR << 1) | I2C_MASTER_WRITE, true);
			i2c_master_write_byte(cmd_write, current_cmd, true); // Send the command number
			i2c_master_stop(cmd_write);
	
			// Execute write
			ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_write, pdMS_TO_TICKS(150));
			i2c_cmd_link_delete(cmd_write);
	
			// Log confirmation
			snprintf(msg, sizeof(msg), "\nSent: %u (0x%02X) to 0x%X\n", current_cmd, current_cmd, TERMINAL_SLAVE_ADDR);
			term_log_append(log_buffer, sizeof(log_buffer), msg);
	
			if (ret != ESP_OK) {
				snprintf(msg, sizeof(msg), "Write failed: %s\n\n", esp_err_to_name(ret));
				term_log_append(log_buffer, sizeof(log_buffer), msg);
				
				// Early exit on write error
				xSemaphoreGive(xI2CBusMutex); // Release I2C bus
				lv_label_set_text(log_lbl, log_buffer);
				lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
				return;
			}
	
			// Delay to allow slave processing time
			vTaskDelay(pdMS_TO_TICKS(50));
			
			uint8_t response_buf[TERMINAL_FIXED_LEN] = {0};
			
			// Single READ transaction (exactly TERMINAL_FIXED_LEN bytes)
			i2c_cmd_handle_t cmd_read = i2c_cmd_link_create();
			i2c_master_start(cmd_read);
			i2c_master_write_byte(cmd_read, (TERMINAL_SLAVE_ADDR << 1) | I2C_MASTER_READ, true);
			i2c_master_read(cmd_read, response_buf, TERMINAL_FIXED_LEN, I2C_MASTER_LAST_NACK);
			i2c_master_stop(cmd_read);
	
			// Execute read
			ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd_read, pdMS_TO_TICKS(150));
			i2c_cmd_link_delete(cmd_read);
	
			// If read something
			if (ret == ESP_OK) {
				uint8_t response_len = response_buf[0]; // First byte is length (from receiver example)
				
				// len == 0 -> empty
				if (response_len == 0) {
					snprintf(msg, sizeof(msg), "Empty response.\n\n");
				}
				// Response too big
				else if (response_len > TERMINAL_MAX_RESPONSE_LEN) {
					snprintf(msg, sizeof(msg), "Invalid response length: %u\n\n", response_len);
				}
				// Else valid response
				else {
					char response_str[TERMINAL_MAX_RESPONSE_LEN + 1] = {0};
					memcpy(response_str, &response_buf[1], response_len); // Copy exactly len bytes
					response_str[response_len] = '\0'; // NUL-terminate
					snprintf(msg, sizeof(msg), "Response: %s\n\n", response_str);
				}
			}
			else {
				snprintf(msg, sizeof(msg), "Read failed: %s\n\n", esp_err_to_name(ret));
			}
			
			// Append to log
			term_log_append(log_buffer, sizeof(log_buffer), msg);
	
			xSemaphoreGive(xI2CBusMutex); // Release I2C bus
	
			// Show and scroll to bottom
			lv_label_set_text(log_lbl, log_buffer);
			lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
		}
		else {
			// Mutex timeout - append error
			term_log_append(log_buffer, sizeof(log_buffer), "\nI2C bus busy - timed out.\n\n");
	
			// Show and scroll to bottom
			lv_label_set_text(log_lbl, log_buffer);
			lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_ON);
		}
	}
	// Exit
	else if (ui_btns->left_btn == 1) {
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
	}
	// Home or power off
	else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
		// Clean up
		lv_obj_delete(cont); // Deletes children
		cont = title_lbl = log_lbl = NULL;
		init = false;
		memset(log_buffer, 0, sizeof(log_buffer)); // Clear log for next entry
		current_cmd = 0;
 
		lcd_funcs_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
	}
}



