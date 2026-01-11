#include "esp_log.h"
#include "nvs.h"

#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"
#include "misc/lv_area.h"
#include "polycast5_macros.h"
#include "widgets/label/lv_label.h"
#include "font/lv_symbol_def.h"

#include "lcd_hotkey.h"
#include "lcd_utils.h"

#include "wifi_task.h"

#define TAG "LCD_HOTKEY"

#define HOTKEY_NS "hotkeys" // NVS namespace
#define HOTKEY_KEY "data" // Data blob

hotkey_menu_t hotkey_menu = {
    .options = {"Hot1", "Hot2", "Hot3", "Hot4", "Hot5", "Hot6"},
    .size = MAX_HOTKEY_OPTIONS,
    .index = 0,
    .cont = NULL,
};

hotkey_cmd_t hotkey_cmd;

void lcd_hotkey_setup_page(hotkey_menu_t *menu)
{
    // Create container
    menu->cont = lv_obj_create(ACTIVE_SCR);
    
    // Format
    lv_obj_set_size(menu->cont, 210, 106);
    lv_obj_center(menu->cont);
    lv_obj_set_style_bg_color(menu->cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(menu->cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(menu->cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(menu->cont, LV_DIR_VER);
    
    // Set flow
    lv_obj_set_flex_flow(menu->cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Set gap
    lv_obj_set_style_pad_gap(menu->cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Prepare styles 
    // Normal button style
    lv_style_init(&menu->btn_style);
    lv_style_set_radius(&menu->btn_style, 8);
    lv_style_set_bg_color(&menu->btn_style, user_primary_color);
    lv_style_set_border_width(&menu->btn_style, 2);
    lv_style_set_border_color(&menu->btn_style, user_secondary_color);
    lv_style_set_border_side(&menu->btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&menu->btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->btn_style, user_secondary_color);
    lv_style_set_text_align(&menu->btn_style, LV_TEXT_ALIGN_CENTER);

    // Selected button style
    lv_style_init(&menu->sel_style);
    lv_style_set_radius(&menu->sel_style, 8);
    lv_style_set_bg_color(&menu->sel_style, user_secondary_color);
    lv_style_set_border_width(&menu->sel_style, 2);
    lv_style_set_border_color(&menu->sel_style, user_secondary_color);
    lv_style_set_border_side(&menu->sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&menu->sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->sel_style, user_primary_color);
    lv_style_set_text_align(&menu->sel_style, LV_TEXT_ALIGN_CENTER);

    // Create button per option
    for (int i = 0; i < menu->size; ++i) {
        menu->btns[i] = lv_btn_create(menu->cont);
        lv_obj_set_size(menu->btns[i], 58, 50);
        
        // Add style
        if (i == menu->index)
            lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
        else
            lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
    
        // Create child label
        lv_obj_t *lbl = lv_label_create(menu->btns[i]);
        lv_label_set_text(lbl, menu->options[i]);
        
        // Format
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    }
    
    // Hide for now
    lv_obj_add_flag(menu->cont, LV_OBJ_FLAG_HIDDEN);
}

void lcd_hotkey_update_menu(hotkey_menu_t *menu)
{
    // Reveal
    lv_obj_remove_flag(menu->cont, LV_OBJ_FLAG_HIDDEN);

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
}

void lcd_hotkey_option_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, hotkey_menu_t *hotkey_menu)
{
    #define HOTKEY_Y_OFFSET 40
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
        lv_label_set_text_fmt(title_lbl, "%s Hotkey", hotkey_menu->options[hotkey_menu->index]);
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

        // Set custom text based on hotkey index
        const char *instr_text = "";
        
        /* Set instruction text */
        // Hot1
        if (hotkey_menu->index == 0) {
            instr_text = "How to configure your command for Hot1:\n\nThis hotkey is triggered when SHORT pressing the "
            "HOME button while on the home page.\n\nTo configure this command, click the right button then send any PolyPlug (SEND), "
            "ESP32, or Infrared signal. The " LV_SYMBOL_EYE_OPEN " icon will appear to represent waiting for a command.";
        } else if (hotkey_menu->index == 1) { // Hot2
            instr_text = "How to configure your command for Hot2:\n\nThis hotkey is triggered when LONG pressing the "
            "HOME button while on the home page.\n\nTo configure this command, click the right button then send any PolyPlug (SEND), "
            "ESP32, or Infrared signal. The " LV_SYMBOL_EYE_OPEN " icon will appear to represent waiting for a command.";
        } else if (hotkey_menu->index == 2) { // Hot3
            instr_text = "How to configure your command for Hot3:\n\nThis hotkey is triggered when LONG pressing the "
            "LEFT button while on the home page.\n\nTo configure this command, click the right button then send any PolyPlug (SEND), "
            "ESP32, or Infrared signal. The " LV_SYMBOL_EYE_OPEN " icon will appear to represent waiting for a command.";
        } else if (hotkey_menu->index == 3) { // Hot4
            instr_text = "How to configure your command for Hot4:\n\nThis hotkey is triggered when LONG pressing the "
            "SELECT button while on the home page.\n\nTo configure this command, click the right button then send any PolyPlug (SEND), "
            "ESP32, or Infrared signal. The " LV_SYMBOL_EYE_OPEN " icon will appear to represent waiting for a command.";
        } else if (hotkey_menu->index == 4) { // Hot5
            instr_text = "How to configure your command for Hot5:\n\nThis hotkey is triggered when SHORT pressing the "
            "RIGHT button while on the home page.\n\nTo configure this command, click the right button then send any PolyPlug (SEND), "
            "ESP32, or Infrared signal. The " LV_SYMBOL_EYE_OPEN " icon will appear to represent waiting for a command.";
        } else if (hotkey_menu->index == 5) { // Hot6
            instr_text = "How to configure your command for Hot6:\n\nThis hotkey is triggered when LONG pressing the "
            "RIGHT button while on the home page.\n\nTo configure this command, click the right button then send any PolyPlug (SEND), "
            "ESP32, or Infrared signal. The " LV_SYMBOL_EYE_OPEN " icon will appear to represent waiting for a command.";
        }
        lv_label_set_text(instr_lbl, instr_text);
    
        init = true;
    }
    
    // Exit
    if (ui_btns->left_btn == 1) {
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
        
        // Show hotkey page
        lv_obj_remove_flag(hotkey_menu->cont, LV_OBJ_FLAG_HIDDEN);
        
        // Switch
        ui_menu->page = HOTKEY_PAGE;
    } else if (ui_btns->up_btn == 1) { // Scroll up
        lv_obj_scroll_by_bounded(cont, 0, HOTKEY_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) { // Scroll down
        lv_obj_scroll_by_bounded(cont, 0, -HOTKEY_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->right_btn == 1) { // Confirm and active viewer
        // Save active index
        hotkey_cmd.active_idx = hotkey_menu->index; // Hot1-Hot6 0-based
        
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
        
        // Hide arrows
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show selection menu
        lcd_unhide_selection_widgets(ui_menu);
        
        // Show hotkey icon
        //lv_obj_remove_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN);
        xEventGroupSetBits(xConnectionIconEventGroup, ICON_BIT_HOTKEY_ACTIVE);
        
        // Switch
        ui_menu->page = SELECTION_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off        
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_hotkey_nvs_save(const hotkey_cmd_t *src)
{
     nvs_handle_t h;

    // Open NVS
     esp_err_t err = nvs_open(HOTKEY_NS, NVS_READWRITE, &h);
     if (err != ESP_OK) {
        ESP_LOGW(TAG, "lcd_hotkey_nvs_save nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    // Save the data
    err = nvs_set_blob(h, HOTKEY_KEY, src, sizeof(*src));
    if (err == ESP_OK) {
        // Commit changes if success
        err = nvs_commit(h);
    } else {
        ESP_LOGE(TAG, "lcd_hotkey_nvs_save set_blob failed: %s", esp_err_to_name(err));
    }

    // Close NVS
     nvs_close(h);
}

void lcd_hotkey_nvs_load(hotkey_cmd_t *dst)
{
     nvs_handle_t h;

    // Open NVS
     esp_err_t err = nvs_open(HOTKEY_NS, NVS_READONLY, &h);
     if (err != ESP_OK) {
        #ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "lcd_hotkey_nvs_load nvs_open failed: %s", esp_err_to_name(err));
        #endif

        memset(dst, 0, sizeof(*dst)); // Make state deterministic
        return;
    }

     size_t sz = sizeof(*dst);

    // Get data blob
     err = nvs_get_blob(h, HOTKEY_KEY, dst, &sz);

    // Check if first boot
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(dst, 0, sizeof(*dst)); // Zero out for first-boot default
    } else if (err != ESP_OK) { // Other error
        ESP_LOGE(TAG, "lcd_hotkey_nvs_load get_blob failed: %s", esp_err_to_name(err));
        memset(dst, 0, sizeof(*dst));
    }

    // Close NVS
     nvs_close(h);
}

