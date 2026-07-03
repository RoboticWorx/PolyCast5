#include "core/lv_obj.h"
#include "misc/lv_area.h"
#include "polycast5_macros.h"
#include "polycast5_fonts.h"

#include "core/lv_obj_pos.h"
#include "core/lv_obj_tree.h"
#include "font/lv_symbol_def.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"

#include "misc/lv_timer.h"
#include "portmacro.h"
#include "widgets/label/lv_label.h"
#include "widgets/switch/lv_switch.h"

#include "nvs.h"
#include "esp_log.h"

#include "lcd_lora.h"
#include "lora_task.h"
#include "lora_pcp.h"
#include "lora_meshtastic_portal.h"
#include "gpio_task.h"
#include "lcd_utils.h"
#include "wifi_task.h"

#include "espnow_task.h"

//#include "gpio_task.h"

#define TAG "LCD_LORA"

// LoRa menu options
#define LORA_OPTIONS_NS "lora_menu" // NVS namespace
#define LORA_OPTIONS_KEY_COUNT "count" // u8: number of options
#define LORA_OPTIONS_KEY_FMT "item_%02d" // e.g. "item_00", "item_01", ...

// LoRa encryption keys
#define LORA_ENC_NS "lora_enc" // NVS namespace
#define LORA_ENC_KEY_COUNT "count" // u8: number of keys
#define LORA_ENC_KEY_FMT "enc_%02d" // e.g. "enc_00", "enc_01", ...

#define LORA_PLAN_SEL_INS "Select day(s)"

#define LORA_NUM_CHAR_ROWS 4

#define LORA_PAIR_KEY_TIMEOUT_MS 10000 // Max wait for the pairing key result
#define LORA_PAIR_FAIL_SHOW_MS 2500 // How long the 'Pairing failed' notice shows

lora_menu_t lora_menu = {
    .options = {"Add PolyPlug", "Meshtastic: OFF"},
    .keys = {},
    .size = LORA_NUM_STATIC_OPTS,
    .index = 0,
    .cont = NULL,
};

lora_plan_menu_t lora_plan_menu = {0};

static const char *submenu_options[] = {
    LV_SYMBOL_UPLOAD "\nSEND",
    LV_SYMBOL_LOOP "\nLOOP",
    LV_SYMBOL_HOME "\nPLAN",
    LV_SYMBOL_WARNING "\nAWAY",
    LV_SYMBOL_USB "\nGPIO",
    LV_SYMBOL_SETTINGS "\nEDIT",
};

static const int submenu_count = sizeof(submenu_options)/sizeof(submenu_options[0]);

static char plan_selected_days[8]; // Up to 7 days + NULL

static char name_buf[MAX_CUSTOM_NAME_LEN + 1] = {0};
static bool lora_menu_overwrite = false;

static const char *lora_char_rows[LORA_NUM_CHAR_ROWS] = {
    "_ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "abcdefghijklmnopqrstuvwxyz",
    "0123456789",
    "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~"
};


void lcd_lora_setup_page(ui_menu_t *ui_menu, lora_menu_t *menu)
{
    // Update Meshtastic option text if not rebuilding for away subpage
    if (ui_menu->page != LORA_AWAY_SUBPAGE) {
        bool meshtastic_enabled = lora_meshtastic_portal_enabled_load_nvs();
        if (meshtastic_enabled) {
            menu->options[1] = "Meshtastic: ON";
        } else {
            menu->options[1] = "Meshtastic: OFF";
        }
    }

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
    
    if (menu->size > LORA_NUM_STATIC_OPTS) {
        menu->index = LORA_NUM_STATIC_OPTS; // Land on the first user plug if any exist
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

void lcd_lora_setup_subpage(lora_menu_t *menu)
{
    // Create receipt label (check/x) for send confirmation
    menu->submenu.lbl_receipt = lv_label_create(ACTIVE_SCR);
    lcd_format_label(menu->submenu.lbl_receipt, "", user_secondary_color,
                     &lv_font_montserrat_18, LV_ALIGN_TOP_LEFT, 6, 2);
    
    // Initialize submenu struct
    menu->submenu.size = submenu_count;
    menu->submenu.index = 0;
    for (int i = 0; i < submenu_count; ++i) {
        menu->submenu.options[i] = (char*)submenu_options[i];
    }
    
    // Create container
    menu->submenu.cont = lv_obj_create(ACTIVE_SCR);
    
    // Format
    lv_obj_set_size(menu->submenu.cont, 210, 106);
    lv_obj_center(menu->submenu.cont);
    lv_obj_set_style_bg_color(menu->submenu.cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(menu->submenu.cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(menu->submenu.cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(menu->submenu.cont, LV_DIR_VER);
    
    // Set flow
    lv_obj_set_flex_flow(menu->submenu.cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(menu->submenu.cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Set gap
    lv_obj_set_style_pad_gap(menu->submenu.cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Prepare styles 
    // Normal button style
    lv_style_init(&menu->submenu.btn_style);
    lv_style_set_radius(&menu->submenu.btn_style, 8);
    lv_style_set_bg_color(&menu->submenu.btn_style, user_primary_color);
    lv_style_set_border_width(&menu->submenu.btn_style, 2);
    lv_style_set_border_color(&menu->submenu.btn_style, user_secondary_color);
    lv_style_set_border_side(&menu->submenu.btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&menu->submenu.btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->submenu.btn_style, user_secondary_color);
    lv_style_set_text_align(&menu->submenu.btn_style, LV_TEXT_ALIGN_CENTER);

    // Selected button style
    lv_style_init(&menu->submenu.sel_style);
    lv_style_set_radius(&menu->submenu.sel_style, 8);
    lv_style_set_bg_color(&menu->submenu.sel_style, user_secondary_color);
    lv_style_set_border_width(&menu->submenu.sel_style, 2);
    lv_style_set_border_color(&menu->submenu.sel_style, user_secondary_color);
    lv_style_set_border_side(&menu->submenu.sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&menu->submenu.sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->submenu.sel_style, user_primary_color);
    lv_style_set_text_align(&menu->submenu.sel_style, LV_TEXT_ALIGN_CENTER);

    // Create button per option
    for (int i = 0; i < menu->submenu.size; ++i) {
        menu->submenu.btns[i] = lv_btn_create(menu->submenu.cont);
        lv_obj_set_size(menu->submenu.btns[i], 58, 50);
        
        // Add style
        if (i == menu->submenu.index)
            lv_obj_add_style(menu->submenu.btns[i], &menu->submenu.sel_style, 0);
        else
            lv_obj_add_style(menu->submenu.btns[i], &menu->submenu.btn_style, 0);
    
        // Create child label
        lv_obj_t *lbl = lv_label_create(menu->submenu.btns[i]);
        lv_label_set_text(lbl, menu->submenu.options[i]);
        
        // Format
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    }
    
    // Hide for now
    lv_obj_add_flag(menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
}

void lcd_lora_update_menu(lora_menu_t *menu)
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

void lcd_lora_update_submenu(lora_menu_t *menu)
{
    // Hide and reset receipt label
    lv_obj_add_flag(menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(menu->submenu.lbl_receipt, "");
    xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drop any receipt latched while away (e.g. a hotkey command's ACK)

    // Reveal
    lv_obj_remove_flag(menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

    // Wrap index
    if (menu->submenu.index >= menu->submenu.size) {
        menu->submenu.index = 0;
    } else if (menu->submenu.index < 0) {
        menu->submenu.index = menu->submenu.size - 1;
    }

    // Reset every button to unselected
    for (int i = 0; i < menu->submenu.size; ++i) {
        lv_obj_remove_style(menu->submenu.btns[i], &menu->submenu.sel_style, 0);
        lv_obj_add_style(menu->submenu.btns[i], &menu->submenu.btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->submenu.btns[menu->submenu.index], &menu->submenu.btn_style, 0);
    lv_obj_add_style(menu->submenu.btns[menu->submenu.index], &menu->submenu.sel_style, 0);
    
    // Enable scrolling if list gets too long
    //lv_obj_scroll_to_view(menu->submenu.btns[menu->submenu.index], LV_ANIM_ON); // LV_ANIM_OFF
}

void lcd_lora_add_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
    #define LORA_ADD_Y_OFFSET 40
    
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
        lv_label_set_text(title_lbl, "Adding a PolyPlug:");
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
        const char *instr_text = "polycast5.com/blogs/ tutorials/using-polyplugs\n\nHow to quickly add a new PolyPlug:"
            "\n\nFirst, walk toward the PolyPlug you want to add so that you can see it."
            "\n\nThen, with the outlet part facing toward you (power side facing away), press the right-most button on the top of the PolyPlug."
            "\n\nOnce pressed, a light should turn blue to indicate it is ready to pair.When it does, press the right button on this "
            "device (PolyCast5) to pair.\n\nIf it doesn't turn blue, wait a few seconds then press it again or try power cycling.";
        
        lv_label_set_text(instr_lbl, instr_text);
    
        init = true;
    }
    
    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, LORA_ADD_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -LORA_ADD_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->left_btn) { // Go back
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
            
        // Show LoRa menu
        lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch back
        ui_menu->page = LORA_PAGE;
    } else if (ui_btns->right_btn == 1) { // Confirm
        // Delete objects
        lv_obj_delete(cont); // Deletes children
        
        // Reset statics
        cont = NULL;
        title_lbl = instr_lbl = NULL;
        init = false;
        
        // Drop any stale request/result from a previous pairing attempt
        xQueueReset(xEspSendEncKeyQueue);
        xQueueReset(xEspSendEncKeyQueueNVS);
        xSemaphoreGive(xLoraGenerateEncKeySemaphore); // Request a fresh encryption key

        // Prompt to enter name
        ui_menu->page = LORA_NAME_PAGE;
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

void lcd_lora_meshtastic_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
    #define MESHTASTIC_ADD_Y_OFFSET 40

    // Statics
    static bool init = false;
    static bool meshtastic_enabled = false; // Toggle state (visual only for now); persists across page visits
    static bool meshtastic_was_loaded = false; // Flag for if meshtastic was loaded
    static lv_obj_t *cont = NULL;
    static lv_obj_t *toggle_hint_lbl = NULL;
    static lv_obj_t *toggle_row = NULL;
    static lv_obj_t *toggle_sw = NULL;
    static lv_obj_t *toggle_state_lbl = NULL;
    static lv_obj_t *intro_lbl = NULL;
    static lv_obj_t *join_lbl = NULL;
    static lv_obj_t *wifi_creds_lbl = NULL;
    static lv_obj_t *middle_lbl = NULL;
    static lv_obj_t *wifi_ip_lbl = NULL;
    static lv_obj_t *ending_lbl = NULL;

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

        // "Press select to toggle" hint (replaces the old title)
        toggle_hint_lbl = lv_label_create(cont);
        lv_label_set_text(toggle_hint_lbl, "Press select to toggle.");
        lv_obj_set_style_text_font(toggle_hint_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(toggle_hint_lbl, user_secondary_color, 0);
        lv_obj_align(toggle_hint_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Centered row holding the toggle switch and its ON/OFF label
        toggle_row = lv_obj_create(cont);
        lv_obj_set_size(toggle_row, 150, 28);
        lv_obj_set_style_bg_color(toggle_row, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(toggle_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(toggle_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_column(toggle_row, 0, LV_PART_MAIN | LV_STATE_DEFAULT); // No default flex gap; spacing set via switch margin
        lv_obj_set_scrollbar_mode(toggle_row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(toggle_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(toggle_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align_to(toggle_row, toggle_hint_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

        // Toggle switch (styled by the default LVGL theme, like the settings toggles)
        toggle_sw = lv_switch_create(toggle_row);
        lv_obj_set_size(toggle_sw, 44, 24);
        lv_obj_set_style_margin_right(toggle_sw, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

        // ON/OFF state label next to the switch
        toggle_state_lbl = lv_label_create(toggle_row);
        lv_obj_set_style_text_font(toggle_state_lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(toggle_state_lbl, user_secondary_color, 0);

        // Sync the switch and label to the persisted state
        meshtastic_enabled = lora_meshtastic_portal_enabled_load_nvs();
        if (meshtastic_enabled) {
            lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
            lv_label_set_text(toggle_state_lbl, "ON");
            meshtastic_was_loaded = true; // Loaded meshtastic
        } else {
            lv_obj_remove_state(toggle_sw, LV_STATE_CHECKED);
            lv_label_set_text(toggle_state_lbl, "OFF");
            meshtastic_was_loaded = false;
        }

        // Intro / explanation label (scrollable if text is long)
        intro_lbl = lv_label_create(cont);
        lv_label_set_long_mode(intro_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(intro_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(intro_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(intro_lbl, user_secondary_color, 0);
        lv_obj_align_to(intro_lbl, toggle_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        if (meshtastic_enabled) {
            const char *intro_text =
                "Press the down arrow to scroll.\n\nMeshtastic is an open-source, long-range mesh network that lets devices "
                "text and share data over LoRa radio, no internet needed.";
            lv_label_set_text(intro_lbl, intro_text);
        } else {
            const char *intro_text =
                "Enable Meshtastic, press RIGHT, then come back.";
            lv_label_set_text(intro_lbl, intro_text);
        }

        // Join instructions label
        join_lbl = lv_label_create(cont);
        lv_label_set_long_mode(join_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(join_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(join_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(join_lbl, user_secondary_color, 0);
        lv_obj_align_to(join_lbl, intro_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text(join_lbl, "To create messages, join the following Wi-Fi network using your phone/PC:");

        // Wi-Fi credentials label
        wifi_creds_lbl = lv_label_create(cont);
        lv_label_set_long_mode(wifi_creds_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(wifi_creds_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(wifi_creds_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(wifi_creds_lbl, user_secondary_color, 0);
        lv_obj_align_to(wifi_creds_lbl, join_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text_fmt(wifi_creds_lbl, "%s\nPass: %s",
                lora_meshtastic_portal_get_ssid(), lora_meshtastic_portal_get_pass());

        // Middle label
        middle_lbl = lv_label_create(cont);
        lv_label_set_long_mode(middle_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(middle_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(middle_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(middle_lbl, user_secondary_color, 0);
        lv_obj_align_to(middle_lbl, wifi_creds_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text(middle_lbl,
                "Once connected, open your internet browser of choice and search:");

        // IP address label
        wifi_ip_lbl = lv_label_create(cont);
        lv_label_set_long_mode(wifi_ip_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(wifi_ip_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(wifi_ip_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(wifi_ip_lbl, user_secondary_color, 0);
        lv_obj_align_to(wifi_ip_lbl, middle_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text_fmt(wifi_ip_lbl, "%s", lora_meshtastic_portal_get_ip());

        // Ending label
        ending_lbl = lv_label_create(cont);
        lv_label_set_long_mode(ending_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ending_lbl, lv_pct(100)); // Full width for wrapping
        lv_obj_set_style_text_font(ending_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ending_lbl, user_secondary_color, 0);
        lv_obj_align_to(ending_lbl, wifi_ip_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_label_set_text(ending_lbl,
                "Then follow the on-screen instructions. "
                "DO NOT exit this page until you're done!");

        // Hide join instructions until Meshtastic is enabled
        if (!meshtastic_enabled) {
            lv_obj_add_flag(join_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(wifi_creds_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(middle_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(wifi_ip_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ending_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_timer_handler();
            // Start SoftAP and web portal
            xEventGroupSetBits(xWiFiPortalEventGroup, WIFI_PORTAL_MESHTASTIC_START_BIT);
        }

        lv_timer_handler();

        init = true;
    }

    if (ui_btns->up_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, MESHTASTIC_ADD_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->down_btn == 1) {
        lv_obj_scroll_by_bounded(cont, 0, -MESHTASTIC_ADD_Y_OFFSET, LV_ANIM_ON);
    } else if (ui_btns->select_btn == 1) { // Toggle Meshtastic on/off (visual only for now)
        meshtastic_enabled = !meshtastic_enabled;
        if (meshtastic_enabled) {
            lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);

            lv_label_set_text(toggle_state_lbl, "ON");
            if (meshtastic_was_loaded) {
                lv_label_set_text(toggle_hint_lbl, "Press select to toggle.");
                lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Hide right arrow
            } else {
                lv_label_set_text(toggle_hint_lbl, "PRESS RIGHT to confirm.");
                lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Show right arrow
            }
        } else {
            lv_obj_remove_state(toggle_sw, LV_STATE_CHECKED);

            lv_label_set_text(toggle_state_lbl, "OFF");
            if (meshtastic_was_loaded) {
                lv_label_set_text(toggle_hint_lbl, "PRESS RIGHT to confirm.");
                lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Show right arrow
            } else {
                lv_label_set_text(toggle_hint_lbl, "Press select to toggle.");
                lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Hide right arrow
            }
        }
    } else if (ui_btns->right_btn && ((meshtastic_enabled && !meshtastic_was_loaded) || 
            (!meshtastic_enabled && meshtastic_was_loaded))) { // When right arrow showing
        // Turn off web portal
        xEventGroupClearBits(xWiFiPortalEventGroup, WIFI_PORTAL_MESHTASTIC_START_BIT);

        // Hide arrows
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        toggle_hint_lbl = toggle_row = toggle_sw = toggle_state_lbl = NULL;
        intro_lbl = join_lbl = wifi_creds_lbl = middle_lbl = wifi_ip_lbl = ending_lbl = NULL;
        init = false;

        // Persist the toggle state so it survives a reboot
        lora_meshtastic_portal_enabled_save_nvs(meshtastic_enabled);

        // Confirmation text
        lv_obj_t *lbl_rst = lv_label_create(ACTIVE_SCR);
        lv_obj_set_style_text_align(lbl_rst, LV_TEXT_ALIGN_CENTER, 0);
        lcd_format_label(lbl_rst, "Saving config...\nDevice will restart.", user_secondary_color,
                &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, 0);
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        esp_restart();
    } else if (ui_btns->left_btn) { // Go back
        // Turn off web portal
        xEventGroupClearBits(xWiFiPortalEventGroup, WIFI_PORTAL_MESHTASTIC_START_BIT);

        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        toggle_hint_lbl = toggle_row = toggle_sw = toggle_state_lbl = NULL;
        intro_lbl = join_lbl = wifi_creds_lbl = middle_lbl = wifi_ip_lbl = ending_lbl = NULL;
        init = false;

        // Show LoRa menu
        lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Switch back
        ui_menu->page = LORA_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        // Turn off web portal
        xEventGroupClearBits(xWiFiPortalEventGroup, WIFI_PORTAL_MESHTASTIC_START_BIT);

        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Delete objects
        lv_obj_delete(cont); // Deletes children

        // Reset statics
        cont = NULL;
        toggle_hint_lbl = toggle_row = toggle_sw = toggle_state_lbl = NULL;
        intro_lbl = join_lbl = wifi_creds_lbl = middle_lbl = wifi_ip_lbl = ending_lbl = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
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
        memcpy(display, name_buf, cur_pos);
    }
    
    // Get current
    display[cur_pos] = cur_char;
    display[len] = '\0';
    
    // Set text and re-center
    lv_label_set_text(lbl_display, display);
    lv_obj_align(lbl_display, LV_ALIGN_CENTER, 0, 30);
}

void lcd_lora_create_custom_name(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
    static espnow_enc_key_result_t enc_key_result;

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
        if (lora_menu_overwrite) {
            // Copy the old name into buffer
            strncpy(name_buf, lora_menu->options[lora_menu->index], MAX_CUSTOM_NAME_LEN);

            // Place cursor at the end
            cur_pos = strlen(name_buf);
        } else { // Else blank slate
            memset(name_buf, 0, sizeof name_buf);
            cur_pos = 0;
        }
        
        // Starting char
        row_idx = 0;
        char_idx = 0;
        cur_char = lora_char_rows[row_idx][char_idx];
        
        lbl_user_in = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_user_in, "", user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 30);
                         
        lbl_dirs = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_dirs, "        Enter plug name:\nPress HOME to cycle chars.", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, -31);
                         
        if (lora_menu_overwrite) {
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
        row_idx = (row_idx + 1) % LORA_NUM_CHAR_ROWS;
        char_idx = 0; // Reset within row
        
        // New current char
        cur_char = lora_char_rows[row_idx][char_idx];
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->up_btn) { // If up, iterate up
        // Increment with wrap
        size_t row_len = strlen(lora_char_rows[row_idx]);
        char_idx = (char_idx + 1) % (int)row_len;
        cur_char = lora_char_rows[row_idx][char_idx];
        
        // Save to array
        name_buf[cur_pos] = cur_char;
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->down_btn) { // If down, iterate down
        // Decrement with wrap
        size_t row_len = strlen(lora_char_rows[row_idx]);
        char_idx = (char_idx + (int)row_len - 1) % (int)row_len;
        cur_char = lora_char_rows[row_idx][char_idx];
        
        // Save to array
        name_buf[cur_pos] = cur_char;
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->left_btn && cur_pos == 0 && lora_menu_overwrite) { // Can back out if at start and renaming
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(name_buf, 0, sizeof name_buf);
        
        lora_menu_overwrite = false; // Switch back
        
        // Reset submenu to first index
        lora_menu->submenu.index = 0;
        lcd_lora_update_submenu(lora_menu);
        
         ui_menu->page = LORA_SUBPAGE;
        return;
    } else if (ui_btns->pwr_btn && lora_menu_overwrite) { // Go home or power off if ranaming
        // Delete labels since no longer used
        lv_obj_delete(lbl_user_in);
        lv_obj_delete(lbl_dirs);
        lv_obj_delete(lbl_chars);
        
        // Reset statics for next time
        lbl_user_in = lbl_chars = lbl_dirs = NULL;
        cur_pos = row_idx = char_idx = 0;
        cur_char = '_';
        memset(name_buf, 0, sizeof name_buf);
        
        lora_menu_overwrite = false; // Switch back
        
        // Reset submenu to first index
        lora_menu->submenu.index = 0;
        lcd_lora_update_submenu(lora_menu);
        
        // Hide
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
         lcd_transition_back(false, ui_menu); // True = home, false = sleep
    } else if (ui_btns->left_btn && cur_pos != 0) { // If left and not at start
        // Clear the current slot
        name_buf[cur_pos] = '\0';
    
        // De-increment left
        if (cur_pos > 0) {
            cur_pos--;
        }
    
        // Reload row/idx from the new slot's char
        char target = name_buf[cur_pos] ? name_buf[cur_pos] : '_';
        for (row_idx = 0; row_idx < LORA_NUM_CHAR_ROWS; row_idx++) {
            const char *row = lora_char_rows[row_idx];
            const char *p = strchr(row, target);
            
            if (p) {
                char_idx = (int)(p - row);
                break;
            }
        }
        // Char not found in any row; fall back to first row/char
        if (row_idx >= LORA_NUM_CHAR_ROWS) {
            row_idx = 0;
            char_idx = 0;
        }
        cur_char = lora_char_rows[row_idx][char_idx];
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->right_btn) { // If right
        // Handle case where up/down wasn't pressed
        name_buf[cur_pos] = cur_char;
        
        // If not yet at end
        if (cur_pos < MAX_CUSTOM_NAME_LEN - 1) {
            cur_pos++;
            name_buf[cur_pos] = '\0';
            char_idx = 0;
            cur_char = lora_char_rows[row_idx][char_idx];
        } else {
            name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
        }
        
        update_name_label_lcd(lbl_user_in, cur_char, cur_pos);
    } else if (ui_btns->select_btn) { // If save button pressed
        // Save final
        if (cur_pos < MAX_CUSTOM_NAME_LEN) {
            name_buf[cur_pos] = cur_char;

            // Terminate one past the last written char if room, else clamp
            size_t term = (cur_pos + 1 <= MAX_CUSTOM_NAME_LEN) ? (cur_pos + 1) : MAX_CUSTOM_NAME_LEN;
            name_buf[term] = '\0';
        }
        
        name_buf[MAX_CUSTOM_NAME_LEN] = '\0';
        memcpy(saved_name, name_buf, MAX_CUSTOM_NAME_LEN + 1);
        
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
        memset(name_buf, 0, sizeof name_buf);

        // Update options
        // If overwriting an existing as a rename
        if (lora_menu_overwrite) {
            // lora_menu->index is edit_idx
            // Release old string then reallocate
            free(lora_menu->options[lora_menu->index]);
            lora_menu->options[lora_menu->index] = strdup(saved_name);
            if (!lora_menu->options[lora_menu->index]) {
                ESP_LOGE(TAG, "strdup failed for rename");
                lora_menu->options[lora_menu->index] = strdup("???");
            }

            // Persist to NVS
            lcd_lora_menu_nvs_save(lora_menu);

            // Update the button’s label in-place
            lv_obj_t *btn = lora_menu->btns[lora_menu->index];
            lv_obj_t *child_lbl = lv_obj_get_child(btn, 0);
            lv_label_set_text(child_lbl, lora_menu->options[lora_menu->index]);

            // Reset flag
            lora_menu_overwrite = false;
            
            // Reset submenu to first index
            lora_menu->submenu.index = 0;
            lcd_lora_update_submenu(lora_menu);
            lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN); // Hide
        } else { // Else adding a whole new remote
            // Show status while waiting on key distribution
            lv_obj_t *lbl_pairing = lv_label_create(ACTIVE_SCR);
            lcd_format_label(lbl_pairing, "Pairing...", user_secondary_color,
                    &lv_font_montserrat_24, LV_ALIGN_CENTER, 0, 0);

            // Wait (bounded) for the shared encryption key from espnow_task
            // espnow_task always posts a result; UI can never deadlock here
            bool got_result = false;
            for (int elapsed = 0; elapsed < LORA_PAIR_KEY_TIMEOUT_MS; elapsed += 10) {
                if (xQueueReceive(xEspSendEncKeyQueueNVS, &enc_key_result, 0) == pdPASS) {
                    got_result = true;
                    break;
                }
                lv_timer_handler();
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            // Only commit the new entry once the key actually arrived,
            // so NVS can never hold a plug name without its key
            bool paired = false;
            if (got_result && enc_key_result.success) {
                // Allocate fresh buffers for this entry
                uint8_t *slot = malloc(LORA_PCP_ENC_KEY_LEN);
                char *name_copy = strdup(saved_name);
                if (!name_copy) {
                    name_copy = strdup("Unknown"); // Fallback name
                    ESP_LOGW(TAG, "Out of memory saving new PolyPlug name, using fallback");
                }

                if (!slot || !name_copy) {
                    ESP_LOGE(TAG, "Out of memory saving new PolyPlug");
                    free(slot);
                    free(name_copy);
                } else {
                    memcpy(slot, enc_key_result.key, LORA_PCP_ENC_KEY_LEN);

                    // Save name and key under the same new index, then to NVS
                    lora_menu->size++;
                    lora_menu->options[lora_menu->size - 1] = name_copy;
                    lora_menu->keys[lora_menu->size - 1] = slot;

                    // Both saves must land or the entry won't survive a reboot;
                    // roll back on failure so memory and NVS stay consistent
                    if (lcd_lora_menu_nvs_save(lora_menu) == ESP_OK &&
                            lcd_lora_key_nvs_save(lora_menu) == ESP_OK) {
                        paired = true;

#ifdef POLYCAST5_DEBUG
                        ESP_LOGI(TAG, "Key saved at slot %d:", lora_menu->size - 1);
                        ESP_LOG_BUFFER_HEX("SAVED IN QUEUE", lora_menu->keys[lora_menu->size - 1], LORA_PCP_ENC_KEY_LEN);
#endif
                    } else {
                        ESP_LOGE(TAG, "Failed to save new PolyPlug to NVS");
                        lora_menu->options[lora_menu->size - 1] = NULL;
                        lora_menu->keys[lora_menu->size - 1] = NULL;
                        lora_menu->size--;
                        lcd_lora_menu_nvs_save(lora_menu); // Best-effort count restore
                        free(name_copy);
                        free(slot);
                    }
                }
            }

            lv_obj_delete(lbl_pairing);

            if (paired) {
                // Create new button for new option
                lora_menu->btns[lora_menu->size - 1] = lv_list_add_btn(lora_menu->main_list, NULL, lora_menu->options[lora_menu->size - 1]);
                lv_obj_set_size(lora_menu->btns[lora_menu->size - 1], 200, 30);
                lv_obj_add_style(lora_menu->btns[lora_menu->size - 1], &lora_menu->btn_style, 0);

                // Create and format text label
                lv_obj_t *lbl = lv_obj_get_child(lora_menu->btns[lora_menu->size - 1], 0);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
                lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
            } else { // Nothing was saved; tell the user and fall back to the LoRa menu
                ESP_LOGE(TAG, "PolyPlug pairing failed: no encryption key distributed");

                lv_obj_t *lbl_fail = lv_label_create(ACTIVE_SCR);
                lcd_format_label(lbl_fail, "Pairing failed!\nPlease try again.", user_secondary_color,
                        &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);

                // Show the notice, then clean it up
                for (int shown = 0; shown < LORA_PAIR_FAIL_SHOW_MS; shown += 10) {
                    lv_timer_handler();
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                lv_obj_delete(lbl_fail);
            }

            lcd_clear_pending_inputs = true; // Clear user inputs from wait
        }
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show LoRa list
        lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Switch to previous page
        ui_menu->page = LORA_PAGE;
        return;
    }
}

static void prompt_name_or_del(ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{    
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
        lv_timer_handler(); // Show
        
        // User hit cancel
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {            
            // Delete objects
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            // Show right arrow
            lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Show submenu
            lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
                
            // Switch pages
            ui_menu->page = LORA_SUBPAGE;
            
            // Go back
            return;
        }
        // Rename
        else if (xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
            // Delete objects
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
                        
            // Don't allow renaming the static entries ("Add PolyPlug", "Meshtastic")
            if (lora_menu->index < LORA_NUM_STATIC_OPTS) {
                // Show right arrow
                lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
                
                // Show submenu
                lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
                
                lcd_clear_pending_inputs = true; // Clear any false inputs
                    
                // Switch pages
                ui_menu->page = LORA_SUBPAGE;
            
                return;
            }

            lcd_clear_pending_inputs = true; // Clear any false inputs

            // Trigger overwrite
            lora_menu_overwrite = true;
        
            // Prompt rename
            ui_menu->page = LORA_NAME_PAGE;
            
            // Go back
            return;
        }
        // Delete
        else if (xSemaphoreTake(xDownButtonSemaphore, 0) == pdTRUE) {            
            // Delete objects
            lv_obj_delete(lbl_exit);
            lv_obj_delete(lbl_name);
            lv_obj_delete(lbl_del);
            lv_obj_delete(lbl_ins);
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Get user entry to remove
            int del_idx = lora_menu->index;     
            
            // Can't be a static entry ("Add PolyPlug", "Meshtastic")
            if (del_idx < LORA_NUM_STATIC_OPTS) {
                // Show right arrow
                lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
                
                // Show submenu
                lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
                
                lcd_clear_pending_inputs = true; // Clear any false inputs
                    
                // Switch pages
                ui_menu->page = LORA_SUBPAGE;
                
                return;
            }
            
            // Free any heap buffers allocated for that slot
            free(lora_menu->options[del_idx]); // Name string
            free(lora_menu->keys[del_idx]); // Key blob
            lv_obj_delete(lora_menu->btns[del_idx]); // LVGL list button
        
            // Shift everything above it down one
            for (int i = del_idx; i < lora_menu->size - 1; ++i) {
                // Change each to the one after
                lora_menu->options[i] = lora_menu->options[i + 1];
                lora_menu->keys[i] = lora_menu->keys[i + 1];
                lora_menu->btns[i] = lora_menu->btns[i + 1];
        
                // Update the label inside the button
                lv_obj_t *lbl = lv_obj_get_child(lora_menu->btns[i], 0);
                lv_label_set_text(lbl, lora_menu->options[i]);
            }
        
            // List is now one shorter
            lora_menu->size--;
            
            // Null out dangling index
            lora_menu->options[lora_menu->size] = NULL;
            lora_menu->keys[lora_menu->size] = NULL;
            lora_menu->btns[lora_menu->size] = NULL;
            
            // Adjust if was last
            if (lora_menu->index >= lora_menu->size) {
                lora_menu->index = lora_menu->size-1;
            }
                
            // Remove entry from NVS
            lcd_lora_menu_nvs_delete(del_idx);
            lcd_lora_key_nvs_delete(del_idx);
        
            // Refresh the list UI
            lcd_lora_update_menu(lora_menu);
            
            // Reset submenu index
            lora_menu->submenu.index = 0;
            // Refresh the submenu UI
            lcd_lora_update_submenu(lora_menu);
            lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN); // Hide submenu
            
            // Show LoRa page
            lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            
            // Hide right arrow
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            
            // Switch pages
            ui_menu->page = LORA_PAGE;
            
            // Go back
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lcd_lora_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu) 
{    
    // If received a valid receipt from the receiver
    if (xSemaphoreTake(xLoraReceiptValidSemaphore, 0) == pdTRUE) {
        // Show check in top left corner
        lv_obj_remove_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, LV_SYMBOL_OK);
    }
    
    // Scroll right
    if (ui_btns->right_btn == 1) {
        // Update selection
        lora_menu->submenu.index++;
        lcd_lora_update_submenu(lora_menu);
    } else if (ui_btns->left_btn == 1 && lora_menu->submenu.index == 0) { // Exit
        // Hide cont
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Hide and reset receipt label
        lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show LoRa list
        lv_obj_remove_flag(lora_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Go back
        ui_menu->page = LORA_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Reset selection
        lora_menu->submenu.index = 0;
        lcd_lora_update_submenu(lora_menu);
        
        // Hide cont
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Hide and reset receipt label
        lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    } else if (ui_btns->left_btn == 1) { // Scroll left
        // Update selection
        lora_menu->submenu.index--;
        lcd_lora_update_submenu(lora_menu);
    } else if (ui_btns->up_btn == 1) { // Scroll up
        // Update selection
        if (lora_menu->submenu.index > 2) {
            lora_menu->submenu.index -= 3;
        } else if (lora_menu->submenu.index < 3) {
            lora_menu->submenu.index += 3;
        }
        lcd_lora_update_submenu(lora_menu);
    } else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 0) { // Send selected
        // No key saved for this PolyPlug (interrupted pairing); don't send
        if (lora_menu->keys[lora_menu->index] == NULL) {
            ESP_LOGE(TAG, "Missing LoRa key for index %d; skipping send", lora_menu->index);
            return;
        }

        // Build the packet
        lora_pcp_cmd_t lora_cmd = {0}; // Zero out
        lora_cmd.index = lora_menu->submenu.index;
        memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
        
        // If recording command as hotkey
        if (!lv_obj_has_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN)) {
            // Zero out at start
            memset(&hotkey_cmd.lora_cmd[hotkey_cmd.active_idx], 0, sizeof(lora_pcp_cmd_t));
            
            // Save into hotkey struct under selected "Keyx"
            hotkey_cmd.lora_cmd[hotkey_cmd.active_idx].index = lora_menu->submenu.index;
            memcpy(hotkey_cmd.lora_cmd[hotkey_cmd.active_idx].key, lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
            
            // Flag that command exists
            hotkey_cmd.has_lora[hotkey_cmd.active_idx] = true;
            // Remove others
            hotkey_cmd.has_espnow[hotkey_cmd.active_idx] = false;
            hotkey_cmd.has_ir[hotkey_cmd.active_idx] = false;
            hotkey_cmd.is_page[hotkey_cmd.active_idx] = false;
            
            // Hide hotkey icon
            xEventGroupClearBits(xConnectionIconEventGroup, ICON_BIT_HOTKEY_ACTIVE);
            
            // Persist to NVS
            lcd_hotkey_nvs_save(&hotkey_cmd);
        }
        
        // Send the command
        xQueueOverwrite(xLoraSendEncQueue, &lora_cmd);
        xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drain stale receipt so only this command's ACK shows

        // RGB indicator
        uint8_t rgb_state = RGB_BLINK_TEAL;
        xQueueSend(xLEDQueue, &rgb_state, portMAX_DELAY);
        
#ifdef POLYCAST5_DEBUG
        //ESP_LOG_BUFFER_HEX("SENDING WITH KEY", lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
#endif
        
        // Reset receipt label
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
    } else if (ui_btns->down_btn == 1) { // Scroll down
        // Update selection
        if (lora_menu->submenu.index < 3) {
            lora_menu->submenu.index += 3;
        } else if (lora_menu->submenu.index > 2) {
            lora_menu->submenu.index -= 3;
        }
        lcd_lora_update_submenu(lora_menu);
    } else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 1) { // Loop selected
        // Hide cont
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Hide and reset receipt label
        lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
        
        // Go to subpage loop page
        ui_menu->page = LORA_LOOP_SUBPAGE;
    } else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 2) { // Plan selected
        // Hide and reset receipt label
        lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
        
        // Hide submenu
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Reveal the container and label
        lv_obj_remove_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
        
        // Default index 0
        lora_plan_menu->plan_index = 0;
        
        // Update plan menu
        lcd_lora_update_plan_menu(lora_plan_menu);
        
        // Go to subpage plan page
        ui_menu->page = LORA_PLAN_SUBPAGE;
    } else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 3) { // Away selected
        // Hide and reset receipt label
        lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
        
        // Hide submenu
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Go to subpage away page
        ui_menu->page = LORA_AWAY_SUBPAGE;
    } else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 4) { // GPIO selected
        // Hide cont
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Hide and reset receipt label
        lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
        
        // Hide up and down arrows
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Go to subpage gpio page
        ui_menu->page = LORA_GPIO_SUBPAGE;
    } else if (ui_btns->select_btn == 1 && lora_menu->submenu.index == 5) { // Edit selected
        // Hide cont
        lv_obj_add_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Hide and reset receipt label
        lv_obj_add_flag(lora_menu->submenu.lbl_receipt, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lora_menu->submenu.lbl_receipt, "");
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Rename or delete entry
        prompt_name_or_del(ui_menu, lora_menu);
    }
}

void lcd_lora_loop_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
    #define LOOP_TIME_OPT_COUNT (sizeof(time_opts)/sizeof(time_opts[0]))
    #define LOOP_Y_SEL_POS 43
    
    // Create statics
    static lv_obj_t *lbl_subpage_times = NULL;
    static lv_obj_t *lbl_subpage_ins = NULL;
    static lv_obj_t *lbl_selected_icon = NULL;
    static lv_obj_t *lbl_top_time = NULL;
    static lv_obj_t *lbl_bot_time = NULL;
    
    static uint8_t selected_index = 1;
    static int on_idx = 0;
    static int off_idx = 0;
    
    static const char *time_opts[] = {
        "1m", "3m", "5m", "15m",
        "30m", "45m", "1h", "2h",
        "3h", "4h", "6h", "8h",
        "12h", "16h", "18h", "24h"
    };
    
    // Create once
    if (!lbl_subpage_times) {
        // Create and format text labels
        lbl_subpage_times = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_subpage_times, "ON time:\nOFF time:", user_secondary_color,
                 &lv_font_montserrat_18, LV_ALIGN_TOP_MID, -15, 20); // +y = down, +x = right
                 
        lbl_subpage_ins = lv_label_create(ACTIVE_SCR);         
        lcd_format_label(lbl_subpage_ins, "- Right/left to adjust time.\n- Press select to confirm.\n- Down to exit.", user_secondary_color,
                 &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, -15);
        
        lbl_selected_icon = lv_label_create(ACTIVE_SCR);         
        lcd_format_label(lbl_selected_icon, LV_SYMBOL_PLAY, user_secondary_color,
             &lv_font_montserrat_12, LV_ALIGN_TOP_MID, -75, LOOP_Y_SEL_POS);
             
        lbl_top_time = lv_label_create(ACTIVE_SCR);         
        lcd_format_label(lbl_top_time, "", user_secondary_color,
             &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 55, 20);
             
        lbl_bot_time = lv_label_create(ACTIVE_SCR);         
        lcd_format_label(lbl_bot_time, "", user_secondary_color, 
             &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 55, LOOP_Y_SEL_POS - 2);
        
        char buf[4];
        snprintf(buf, sizeof(buf), "%s", time_opts[0]);
        lv_label_set_text(lbl_top_time, buf);
        lv_label_set_text(lbl_bot_time, buf);
    }
    
    // Move up
    if (ui_btns->up_btn == 1) {
        if (selected_index == 1) {
            // Move pointer up
            lv_obj_set_y(lbl_selected_icon, 22);
            selected_index = 0;
        } else if (selected_index == 0) {
            // Move pointer up
            lv_obj_set_y(lbl_selected_icon, LOOP_Y_SEL_POS);
            selected_index = 1;
        }
    } else if (ui_btns->down_btn == 1 && selected_index == 0) { // Move down
        // Move pointer down
        lv_obj_set_y(lbl_selected_icon, LOOP_Y_SEL_POS);
            
        selected_index = 1;
    } else if (ui_btns->right_btn == 1) { // Shift time of selected right
        // Changing top time
        if (selected_index == 0) {
            on_idx = (on_idx  + 1) % LOOP_TIME_OPT_COUNT;
            char buf[4];
            snprintf(buf, sizeof(buf), "%s", time_opts[on_idx]);
            lv_label_set_text(lbl_top_time, buf);
        } else { // Changing bot time
            off_idx = (off_idx + 1) % LOOP_TIME_OPT_COUNT;
            char buf[4];
            snprintf(buf, sizeof(buf), "%s", time_opts[off_idx]);
            lv_label_set_text(lbl_bot_time, buf);
        }
    } else if (ui_btns->left_btn == 1) { // Shift time of selected left
        // Changing top time
        if (selected_index == 0) {
            on_idx = (on_idx - 1 + LOOP_TIME_OPT_COUNT) % LOOP_TIME_OPT_COUNT;
            char buf[4];
            snprintf(buf, sizeof(buf), "%s", time_opts[on_idx]);
            lv_label_set_text(lbl_top_time, buf);
        } else { // Changing bot time
            off_idx = (off_idx - 1 + LOOP_TIME_OPT_COUNT) % LOOP_TIME_OPT_COUNT;
            char buf[4];
            snprintf(buf, sizeof(buf), "%s", time_opts[off_idx]);
            lv_label_set_text(lbl_bot_time, buf);
        }
    } else if (ui_btns->select_btn == 1) { // Confirm                 
        // Reset objects
        lv_obj_delete(lbl_subpage_times);
        lv_obj_delete(lbl_selected_icon);
        lv_obj_delete(lbl_top_time);
        lv_obj_delete(lbl_bot_time);
        lbl_subpage_times = NULL;
        lbl_selected_icon = NULL;
        lbl_top_time = NULL;
        lbl_bot_time = NULL;
        
        // Send the data to lora_task
        if (lora_menu->keys[lora_menu->index] == NULL) { // No key saved (interrupted pairing); don't send
            ESP_LOGE(TAG, "Missing LoRa key for index %d; skipping send", lora_menu->index);
        } else {
            lora_pcp_cmd_t lora_cmd = {0}; // Zero out
            lora_cmd.index = lora_menu->submenu.index;
            memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
            snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "on %s off %s", time_opts[on_idx], time_opts[off_idx]);

            // Confirmation text
            lcd_format_label(lbl_subpage_ins, "Sending to PolyPlug...", user_secondary_color,
                    &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
            lv_timer_handler();

            xQueueOverwrite(xLoraSendEncQueue, &lora_cmd); // Send the command
            xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drain stale receipt so only this command's ACK shows
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // Reset confirmation lbl
        lv_obj_delete(lbl_subpage_ins);
        lbl_subpage_ins = NULL;
        
        // Refresh statics 
        selected_index = 1;
        on_idx = 0;
        off_idx = 0;
        
        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
            
        // Go back
        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->down_btn == 1) { // Go back
        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Reset objects
        lv_obj_delete(lbl_subpage_times);
        lv_obj_delete(lbl_selected_icon);
        lv_obj_delete(lbl_subpage_ins);
        lv_obj_delete(lbl_top_time);
        lv_obj_delete(lbl_bot_time);
        lbl_subpage_times = NULL;
        lbl_selected_icon = NULL;
        lbl_subpage_ins = NULL;
        lbl_top_time = NULL;
        lbl_bot_time = NULL;
        
        // Refresh statics 
        selected_index = 1;
        on_idx = 0;
        off_idx = 0;
            
        // Go back
        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected            
        // Reset objects
        lv_obj_delete(lbl_subpage_times);
        lv_obj_delete(lbl_selected_icon);
        lv_obj_delete(lbl_subpage_ins);
        lv_obj_delete(lbl_top_time);
        lv_obj_delete(lbl_bot_time);
        lbl_subpage_times = NULL;
        lbl_selected_icon = NULL;
        lbl_subpage_ins = NULL;
        lbl_top_time = NULL;
        lbl_bot_time = NULL;
        
        // Refresh statics 
        selected_index = 1;
        on_idx = 0;
        off_idx = 0;
            
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_lora_gpio_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
    #define GPIO_X_POS -38
    #define GPIO_TX_TXT "Transmit: "
    #define GPIO_RX_TXT "Received: "
    #define GPIO_BUF_SIZE 4
    
    // Create statics
    static uint8_t cmd_to_send = 1; // Set default
    static bool tx_success = false;
    static bool init = false;
    
    static lv_obj_t *qr_canvas = NULL;
    static uint8_t *qr_buf = NULL; // Canvas backing buffer
    
    static lv_obj_t *lbl_send_tx = NULL;
    static lv_obj_t *lbl_send_rx = NULL;
    static lv_obj_t *lbl_send_cmd = NULL;
    static lv_obj_t *lbl_send_box = NULL;
    static lv_obj_t *lbl_info = NULL;
    static lv_obj_t *lbl_send = NULL;
    static lv_obj_t *arrow_top = NULL;
    static lv_obj_t *arrow_bot = NULL;
    static lv_style_t style_cmd;
    static lv_style_t style_info;
    
    // Do once
    if (!init) {
        tx_success = false;
        cmd_to_send = 1;
        
        // Create labels
        lbl_send_tx = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_send_tx, GPIO_TX_TXT, user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, GPIO_X_POS, 39);
                         
        lbl_send_rx = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_send_rx, GPIO_RX_TXT, user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_CENTER, GPIO_X_POS, 57);
    
        lbl_send_cmd = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_send_cmd, "1", user_secondary_color,
                &lv_font_montserrat_30, LV_ALIGN_CENTER, GPIO_X_POS, -20);
        lv_label_set_text_fmt(lbl_send_cmd, "%d", cmd_to_send);
    
        lbl_send_box = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_send_box, "", user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_CENTER, GPIO_X_POS, -20);
                         
        lbl_send = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_send, "SEND", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);
        
        lbl_info = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_info, LV_SYMBOL_HOME " INFO", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_BOTTOM_RIGHT, -5, -4);
        
        arrow_top = lv_label_create(ACTIVE_SCR);
        lcd_format_label(arrow_top, LV_SYMBOL_UP, user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_CENTER, GPIO_X_POS, -50);
                         
        arrow_bot = lv_label_create(ACTIVE_SCR);
        lcd_format_label(arrow_bot, LV_SYMBOL_DOWN, user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_CENTER, GPIO_X_POS, 10);
    
        // Create a style for the send cmd box
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
        
        // Create a style for the edit box
        lv_style_init(&style_info);
    
        lv_style_set_radius(&style_info, 8);
        lv_style_set_bg_color(&style_info, user_primary_color);
        lv_style_set_border_width(&style_info, 2);
        lv_style_set_border_color(&style_info, user_secondary_color);
        lv_style_set_border_side(&style_info, LV_BORDER_SIDE_FULL);
        lv_style_set_text_color(&style_info, user_secondary_color);
        
        lv_style_set_pad_left(&style_info, 10);
        lv_style_set_pad_right(&style_info, 10);
        lv_style_set_pad_top(&style_info, 6);
        lv_style_set_pad_bottom(&style_info, 6);
            
        lv_obj_add_style(lbl_send_box, &style_cmd, 0);
        lv_obj_add_style(lbl_info, &style_info, 0);
        
        // Show
        lv_timer_handler();
        
        // Done initializing
        init = true;
    }
    
    /* Status updates */
    // If transmission successful
    if (tx_success) {
        lv_label_set_text(lbl_send_tx, GPIO_TX_TXT LV_SYMBOL_OK);
        tx_success = false; // Update once
    }
    // If got a receipt
    if (xSemaphoreTake(xLoraReceiptValidSemaphore, 0) == pdTRUE) {
        lv_label_set_text(lbl_send_rx, GPIO_RX_TXT LV_SYMBOL_OK);
    }
    
    /* User input */
    // Increment command
    if (ui_btns->up_btn == 1) {
        // Reset receipts
        lv_label_set_text(lbl_send_tx, GPIO_TX_TXT);
        lv_label_set_text(lbl_send_rx, GPIO_RX_TXT);
        
        cmd_to_send++;
        
        char buf[GPIO_BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", cmd_to_send);
        lv_label_set_text(lbl_send_cmd, buf);
    } else if (ui_btns->down_btn == 1) { // Decrement command
        // Reset receipts
        lv_label_set_text(lbl_send_tx, GPIO_TX_TXT);
        lv_label_set_text(lbl_send_rx, GPIO_RX_TXT);
        
        cmd_to_send--;
        
        char buf[GPIO_BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", cmd_to_send);
        lv_label_set_text(lbl_send_cmd, buf);
    } else if (ui_btns->select_btn == 1) { // Increment command by 3
        // Reset receipts
        lv_label_set_text(lbl_send_tx, GPIO_TX_TXT);
        lv_label_set_text(lbl_send_rx, GPIO_RX_TXT);
        
        cmd_to_send += 3;
        
        char buf[GPIO_BUF_SIZE];
        snprintf(buf, sizeof(buf), "%u", cmd_to_send);
        lv_label_set_text(lbl_send_cmd, buf);
    } else if (ui_btns->right_btn == 1) { // Send to PolyPlug
        // Reset receipts
        lv_label_set_text(lbl_send_tx, GPIO_TX_TXT);
        lv_label_set_text(lbl_send_rx, GPIO_RX_TXT);
        
        // Send the data to lora_task
        if (lora_menu->keys[lora_menu->index] == NULL) { // No key saved (interrupted pairing); don't send
            ESP_LOGE(TAG, "Missing LoRa key for index %d; skipping send", lora_menu->index);
        } else {
            lora_pcp_cmd_t lora_cmd = {0}; // Zero out
            lora_cmd.index = lora_menu->submenu.index;
            memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
            snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "gpio %d", cmd_to_send);

            xQueueOverwrite(xLoraSendEncQueue, &lora_cmd); // Send the command
            xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drain stale receipt so only this command's ACK shows

            // TX confirmation
            tx_success = true;
        }
    } else if (ui_btns->left_btn == 1) { // Back
        // Reset objects
        lv_obj_delete(lbl_send_tx);
        lv_obj_delete(lbl_send_rx);
        lv_obj_delete(lbl_send_cmd);
        lv_obj_delete(lbl_send_box);
        lv_obj_delete(lbl_send);
        lv_obj_delete(lbl_info);
        lv_obj_delete(arrow_top);
        lv_obj_delete(arrow_bot);
        
        // Free the style
        lv_style_reset(&style_cmd);
        lv_style_reset(&style_info);
        
        // Reset statics
        lbl_send_tx = lbl_send_rx = lbl_send_cmd = lbl_send_box = lbl_send = arrow_top = arrow_bot = lbl_info = NULL;
        init = false;
        
        // Show up and down arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
            
        // Go back
        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->home_btn == 1) { // Home selected            
        // Reset objects
        lv_obj_delete(lbl_send_tx);
        lv_obj_delete(lbl_send_rx);
        lv_obj_delete(lbl_send_cmd);
        lv_obj_delete(lbl_send_box);
        lv_obj_delete(lbl_send);
        lv_obj_delete(lbl_info);
        lv_obj_delete(arrow_top);
        lv_obj_delete(arrow_bot);
        
        // Free the style
        lv_style_reset(&style_cmd);
        lv_style_reset(&style_info);
        
        // Reset statics
        lbl_send_tx = lbl_send_rx = lbl_send_cmd = lbl_send_box = lbl_send = arrow_top = arrow_bot = lbl_info = NULL;
        init = false;
        
        /* Show 'How to Use PolyCast5 LoRa' link */
        
        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
    
        // Create QR
        qr_canvas = lv_canvas_create(ACTIVE_SCR);
        lv_obj_set_size(qr_canvas, 110, 110);
        lv_obj_align(qr_canvas, LV_ALIGN_CENTER, 0, 0);
        const char *url = "https://polycast5.com/blogs/docs/use-polycast5-lora-in-your-own-projects/";
        
        // Draw the URL as a QR
        int n = lcd_draw_qr(qr_canvas, url, 110, &qr_buf);
        if (n != 0) {
            ESP_LOGE(TAG, "lcd_lora_gpio_subpage lcd_draw_qr failed: %d", n);
        }
        
        // Show until back selected
        while (xSemaphoreTake(xLeftButtonSemaphore, 0) != pdTRUE) {
            lv_timer_handler();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        // Free QR buffer
        if (qr_buf) {
            free(qr_buf);
            qr_buf = NULL;
        }
        lv_obj_delete(qr_canvas);
        qr_canvas = NULL;
        
        /* Exit */
        
        lcd_clear_pending_inputs = true;
        
        // Show arrows
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        
        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
            
        // Go back
        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->pwr_btn == 1) { // Power off selected            
        // Reset objects
        lv_obj_delete(lbl_send_tx);
        lv_obj_delete(lbl_send_rx);
        lv_obj_delete(lbl_send_cmd);
        lv_obj_delete(lbl_send_box);
        lv_obj_delete(lbl_send);
        lv_obj_delete(lbl_info);
        lv_obj_delete(arrow_top);
        lv_obj_delete(arrow_bot);
        
        // Free the style
        lv_style_reset(&style_cmd);
        lv_style_reset(&style_info);
        
        // Reset statics
        lbl_send_tx = lbl_send_rx = lbl_send_cmd = lbl_send_box = lbl_send = arrow_top = arrow_bot = lbl_info = NULL;
        init = false;
            
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_lora_setup_plan_page(ui_menu_t *ui_menu, lora_plan_menu_t *lora_plan_menu)
{
    static const char *options[LORA_PLAN_SUBMENU_COUNT] = {
        LV_SYMBOL_CLOSE "\nMON",
        LV_SYMBOL_CLOSE "\nTUE",
        LV_SYMBOL_CLOSE "\nWED",
        LV_SYMBOL_CLOSE "\nTHU",
        LV_SYMBOL_CLOSE "\nFRI",
        LV_SYMBOL_CLOSE "\nSAT",
        LV_SYMBOL_CLOSE "\nSUN",
        LV_SYMBOL_TRASH "\nREM",
    };

    // Assign options
    for (int i = 0; i < LORA_PLAN_SUBMENU_COUNT; ++i) {
        lora_plan_menu->plan_options[i] = options[i];
    }

    // Create instruction label
    lora_plan_menu->lbl_days_ins = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS, user_secondary_color,
                 &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 10);
    
    /* Initialize plan submenu */
    // Create container
    lora_plan_menu->plan_cont = lv_obj_create(ACTIVE_SCR);
    
    // Format
    lv_obj_set_size(lora_plan_menu->plan_cont, 230, 92);
    lv_obj_align(lora_plan_menu->plan_cont, LV_ALIGN_CENTER, 0, 7);
    lv_obj_set_style_bg_color(lora_plan_menu->plan_cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(lora_plan_menu->plan_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_scrollbar_mode(lora_plan_menu->plan_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(lora_plan_menu->plan_cont, LV_DIR_VER);
    
    // Set flow
    lv_obj_set_flex_flow(lora_plan_menu->plan_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(lora_plan_menu->plan_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Set padding
    lv_obj_set_style_pad_gap(lora_plan_menu->plan_cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Prepare styles 
    // Normal button style
    lv_style_init(&lora_plan_menu->plan_btn_style);
    lv_style_set_radius(&lora_plan_menu->plan_btn_style, 8);
    lv_style_set_bg_color(&lora_plan_menu->plan_btn_style, user_primary_color);
    lv_style_set_border_width(&lora_plan_menu->plan_btn_style, 2);
    lv_style_set_border_color(&lora_plan_menu->plan_btn_style, user_secondary_color);
    lv_style_set_border_side(&lora_plan_menu->plan_btn_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&lora_plan_menu->plan_btn_style, &lv_font_montserrat_14);
    lv_style_set_text_color(&lora_plan_menu->plan_btn_style, user_secondary_color);
    lv_style_set_text_align(&lora_plan_menu->plan_btn_style, LV_TEXT_ALIGN_CENTER);

    // Selected button style
    lv_style_init(&lora_plan_menu->plan_sel_style);
    lv_style_set_radius(&lora_plan_menu->plan_sel_style, 8);
    lv_style_set_bg_color(&lora_plan_menu->plan_sel_style, user_secondary_color);
    lv_style_set_border_width(&lora_plan_menu->plan_sel_style, 2);
    lv_style_set_border_color(&lora_plan_menu->plan_sel_style, user_secondary_color);
    lv_style_set_border_side(&lora_plan_menu->plan_sel_style, LV_BORDER_SIDE_FULL);
    lv_style_set_text_font(&lora_plan_menu->plan_sel_style, &lv_font_montserrat_14);
    lv_style_set_text_color(&lora_plan_menu->plan_sel_style, user_primary_color);
    lv_style_set_text_align(&lora_plan_menu->plan_sel_style, LV_TEXT_ALIGN_CENTER);

    // Create button per option
    for (int i = 0; i < LORA_PLAN_SUBMENU_COUNT; ++i) {
        lora_plan_menu->plan_btns[i] = lv_btn_create(lora_plan_menu->plan_cont);
        lv_obj_set_size(lora_plan_menu->plan_btns[i], 48, 43);
        
        // Add style
        if (i == lora_plan_menu->plan_index) {
            lv_obj_add_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_btn_style, 0);
        } else {
            lv_obj_add_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_sel_style, 0);
        }
    
        // Create child label
        lv_obj_t *lbl = lv_label_create(lora_plan_menu->plan_btns[i]);
        lv_label_set_text(lbl, lora_plan_menu->plan_options[i]);
        
        // Format
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -1);
    }
    
    // Put arrows after container
    lv_obj_move_foreground(ui_menu->arrow_top);
    lv_obj_move_foreground(ui_menu->arrow_bot);
    lv_obj_move_foreground(ui_menu->arrow_left);
    lv_obj_move_foreground(ui_menu->arrow_right);
    
    // Update plan menu
    lcd_lora_update_plan_menu(lora_plan_menu);
    
    // Hide the container and label
    lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
}

void lcd_lora_plan_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu)
{
    #define PLAN_COLUMNS 4 // 4x2 grid to fit 8 buttons
    
    // Statics for toggling
    static const char *days[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
    static bool days_selected[7] = {false};
    
    // Confirm
    if (ui_btns->right_btn == 1 && lora_plan_menu->plan_index == 7) {
        // Save selected days into global
        int pos = 0;
        for (int i = 0; i < 7; ++i) {
            // If day selected
            if (days_selected[i]) {
                // '1' + i gives '1' for Monday (i==0), '2' for Tuesday, ... '7' for Sunday
                plan_selected_days[pos++] = '1' + i;
            }
        }
        plan_selected_days[pos] = '\0'; // Terminate
        
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Days selected = '%s'", plan_selected_days);
#endif
        
        // Reset all labels and days
        for (int i = 0; i < 7; ++i) {
            days_selected[i] = false;
            lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
            char buf[20];
            snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
            lv_label_set_text(lbl, buf);
        }
        
        lora_plan_menu->plan_index = 0;
        
        // Reset text
        lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
        
        // Hide plan items
        lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
        
        // Hide arrows
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = LORA_PLAN_CONFIRM_SUBPAGE;
    } else if (ui_btns->right_btn == 1) { // Go right        
        // Update selection right
        lora_plan_menu->plan_index = (lora_plan_menu->plan_index + 1) % LORA_PLAN_SUBMENU_COUNT;
        
        // Update plan menu
        lcd_lora_update_plan_menu(lora_plan_menu);
    } else if (ui_btns->left_btn == 1 && lora_plan_menu->plan_index == 0) { // Back
        // Reset all labels and days
        for (int i = 0; i < 7; ++i) {
            days_selected[i] = false;
            lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
            char buf[20];
            snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
            lv_label_set_text(lbl, buf);
        }
        
        // Reset text
        lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
        
        // Hide plan items
        lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
        
        // Show LoRa submenu
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->left_btn == 1) { // Go left
        // Update selection left
        lora_plan_menu->plan_index = (lora_plan_menu->plan_index - 1 + LORA_PLAN_SUBMENU_COUNT) % LORA_PLAN_SUBMENU_COUNT;
        
        // Update plan menu
        lcd_lora_update_plan_menu(lora_plan_menu);
    } else if (ui_btns->up_btn == 1) { // Go up
        // Update selection up (for 4 columns: if > 3 subtract 4, else add 4)
        if (lora_plan_menu->plan_index > (PLAN_COLUMNS - 1)) {
            lora_plan_menu->plan_index -= PLAN_COLUMNS;
        } else {
            lora_plan_menu->plan_index += PLAN_COLUMNS;
        }
        
        // Wrap if needed
        if (lora_plan_menu->plan_index >= LORA_PLAN_SUBMENU_COUNT) {
            lora_plan_menu->plan_index -= LORA_PLAN_SUBMENU_COUNT;
        }
        
        // Update plan menu
        lcd_lora_update_plan_menu(lora_plan_menu);
    } else if (ui_btns->down_btn == 1) { // Go down
        // Update selection down (for 4 columns: if < 4 add 4, else subtract 4)
        if (lora_plan_menu->plan_index < PLAN_COLUMNS) {
            lora_plan_menu->plan_index += PLAN_COLUMNS;
        } else {
            lora_plan_menu->plan_index -= PLAN_COLUMNS;
        }
        
        // Wrap if needed
        if (lora_plan_menu->plan_index < 0) {
            lora_plan_menu->plan_index += LORA_PLAN_SUBMENU_COUNT;
        }
        
        // Update plan menu
        lcd_lora_update_plan_menu(lora_plan_menu);
    } else if (ui_btns->select_btn == 1) { // Select option
        // If a day, toggle it and update symbol
        if (lora_plan_menu->plan_index < 7) {
            days_selected[lora_plan_menu->plan_index] = !days_selected[lora_plan_menu->plan_index]; // Toggle day
            lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[lora_plan_menu->plan_index], 0);
            
            // Format new text into label
            char buf[20];
            snprintf(buf, sizeof(buf), "%s\n%s", days_selected[lora_plan_menu->plan_index] ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, days[lora_plan_menu->plan_index]);
            lv_label_set_text(lbl, buf);
            
            // Set confirmation text
            lv_label_set_text(lora_plan_menu->lbl_days_ins, "Hold right to confirm");
        } else { // Else "REM" selected (remove)
            // Reset all labels and days
            for (int i = 0; i < 7; ++i) {
                days_selected[i] = false;
                lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
                char buf[20];
                snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
                lv_label_set_text(lbl, buf);
            }
            
            // Reset text
            lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
            
            // Hide and go back
            lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
            
            // Show LoRa submenu
            lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

            // Switch pages
            ui_menu->page = LORA_SUBPAGE;
        }
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        // Reset all labels and days
        for (int i = 0; i < 7; ++i) {
            days_selected[i] = false;
            lv_obj_t *lbl = lv_obj_get_child(lora_plan_menu->plan_btns[i], 0);
            char buf[20];
            snprintf(buf, sizeof(buf), "%s\n%s", LV_SYMBOL_CLOSE, days[i]);
            lv_label_set_text(lbl, buf);
        }
        
        // Reset text
        lv_label_set_text(lora_plan_menu->lbl_days_ins, LORA_PLAN_SEL_INS);
            
        // Hide plan items
        lv_obj_add_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
        
        // Go home or power off
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu);
    }
}

void lcd_lora_plan_confirm_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_plan_menu_t *lora_plan_menu)
{
    static bool init = false;
    static lv_obj_t *lbl_ins_top = NULL;
    static lv_obj_t *lbl_ins_bot = NULL;
    static lv_obj_t *lbl_conf = NULL;
    
    if (!init) {
        lbl_ins_top = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins_top, "Please make sure the\n targeted PolyPlug is\n  connected to Wi-Fi\n  before proceeding!", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, -25);
                
        lbl_ins_bot = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins_bot, "If not, please do so in the\n   Wi-Fi menu via 'sync'.", user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 27);
                
        lbl_conf = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_conf, "Press select to confirm", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -5);
        
        init = true;
    }
    
    // Back
    if (ui_btns->left_btn == 1) {
        // Reset objects
        lv_obj_delete(lbl_ins_top);
        lv_obj_delete(lbl_ins_bot);
        lv_obj_delete(lbl_conf);
        
        // Reset statics
        lbl_ins_top = lbl_ins_bot = lbl_conf = NULL;
        init = false;
        
        // Show arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Reveal the plan container and label
        lv_obj_remove_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);
        
        lcd_lora_update_plan_menu(lora_plan_menu); // Refresh
        
        // Go back
        ui_menu->page = LORA_PLAN_SUBPAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        // Reset objects
        lv_obj_delete(lbl_ins_top);
        lv_obj_delete(lbl_ins_bot);
        lv_obj_delete(lbl_conf);
        
        // Reset statics
        lbl_ins_top = lbl_ins_bot = lbl_conf = NULL;
        init = false;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    } else if (ui_btns->select_btn == 1) { // Confirm
        // Reset objects
        lv_obj_delete(lbl_ins_top);
        lv_obj_delete(lbl_ins_bot);
        lv_obj_delete(lbl_conf);
        
        // Reset statics
        lbl_ins_top = lbl_ins_bot = lbl_conf = NULL;
        init = false;
        
        // Show arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Switch pages
        ui_menu->page = LORA_PLAN_TIMES_SUBPAGE;
    }
}

/* lcd_lora_plan_times_subpage HELPERS */
// Helper to update the full time range label using individual char labels
static void update_time_label(lv_obj_t *time_labels[], char *start_time, char *end_time) {
    const char *full_str = "00:00:00-00:00:00";  // Template for positions
    for (int i = 0; i < 17; ++i) {  // Full string length without null
        char ch[2] = { full_str[i], '\0' };  // Single char
        if (i < 8) {
            ch[0] = start_time[i];  // Override with actual start
        } else if (i == 8) {
            ch[0] = '-';
        } else {
            ch[0] = end_time[i - 9];  // Override with actual end (skip "-")
        }

        lv_label_set_text(time_labels[i], ch);
    }
}
// Helper to get digit value at position (0-5) in time string
static int get_digit(char *time_str, uint8_t pos) {
    uint8_t str_pos[] = {0, 1, 3, 4, 6, 7}; // Positions in "HH:MM:SS"
    return time_str[str_pos[pos]] - '0';
}
// Helper to set digit value at position
static void set_digit(char *time_str, uint8_t pos, int val) {
    uint8_t str_pos[] = {0, 1, 3, 4, 6, 7};
    time_str[str_pos[pos]] = '0' + val;
}

void lcd_lora_plan_times_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu, lora_plan_menu_t *lora_plan_menu)
{
    #define PLAN_TIME_DIGITS 12 // HHMMSS (ignoring :) for both times
    #define PLAN_TIME_STR_LEN 9 // "HH:MM:SS\0"
    #define PLAN_TIME_DIGIT_WIDTH 12 // Fixed width per digit/colon (adjust based on font metrics)
    #define PLAN_TIME_X_BASE -96

    // Create statics
    static lv_obj_t *lbl_subpage_times = NULL;
    static lv_obj_t *lbl_selected_icon = NULL;
    static lv_obj_t *lbl_ins = NULL;
    static lv_obj_t *time_labels[17]; // 8 for start ("HH:MM:SS") + 1 for "-" + 8 for end

    static int cursor_x_offsets[12];
    static uint8_t selected_digit = 0; // 0-11: positions across both HHMMSS
    static char start_time[PLAN_TIME_STR_LEN] = "00:00:00";
    static char end_time[PLAN_TIME_STR_LEN] = "00:00:00";
    
    static bool init = false;

    // Do once
    if (!init) {
        // Create and format text labels
        lbl_subpage_times = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_subpage_times, "ON time - OFF time", user_secondary_color,
                &lv_font_montserrat_20, LV_ALIGN_CENTER, 0, -25);
                
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "* Time is 24h format *", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -15);

        // Create individual char label for each digit
        for (int i = 0; i < 17; ++i) {
            time_labels[i] = lv_label_create(ACTIVE_SCR);
            lcd_format_label(time_labels[i], "0", user_secondary_color,
                     &lv_font_montserrat_18, LV_ALIGN_CENTER, PLAN_TIME_X_BASE + (i * PLAN_TIME_DIGIT_WIDTH), 0);
            
            // Set fixed width to prevent shifting
            lv_obj_set_width(time_labels[i], PLAN_TIME_DIGIT_WIDTH);
            lv_obj_set_style_text_align(time_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        }
        
        update_time_label(time_labels, start_time, end_time); // Initial update

        // Tracking arrow
        lbl_selected_icon = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_selected_icon, LV_SYMBOL_EJECT, user_secondary_color,
                &lv_font_montserrat_14, LV_ALIGN_CENTER, cursor_x_offsets[0], 20);
        
        // Calculate digit offsets
        cursor_x_offsets[0] = PLAN_TIME_X_BASE + (0 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[1] = PLAN_TIME_X_BASE + (1 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[2] = PLAN_TIME_X_BASE + (3 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[3] = PLAN_TIME_X_BASE + (4 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[4] = PLAN_TIME_X_BASE + (6 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[5] = PLAN_TIME_X_BASE + (7 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[6] = PLAN_TIME_X_BASE + (9 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[7] = PLAN_TIME_X_BASE + (10 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[8] = PLAN_TIME_X_BASE + (12 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[9] = PLAN_TIME_X_BASE + (13 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[10] = PLAN_TIME_X_BASE + (15 * PLAN_TIME_DIGIT_WIDTH);
        cursor_x_offsets[11] = PLAN_TIME_X_BASE + (16 * PLAN_TIME_DIGIT_WIDTH);
        
        // Set cursor X position
        lv_obj_set_x(lbl_selected_icon, cursor_x_offsets[selected_digit]);
        
        init = true;
    }
    
    // Confirm
    if (ui_btns->right_btn == 1 && selected_digit == 11) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Confirmed: start_time = '%s', end_time = '%s'", start_time, end_time);
#endif
        
        // Reset objects
        lv_obj_delete(lbl_subpage_times);
        lv_obj_delete(lbl_selected_icon);
        lv_obj_delete(lbl_ins);
        for (int i = 0; i < 17; ++i) {
            lv_obj_delete(time_labels[i]);
        }
        
        // Send the data to lora_task
        if (lora_menu->keys[lora_menu->index] == NULL) { // No key saved (interrupted pairing); don't send
            ESP_LOGE(TAG, "Missing LoRa key for index %d; skipping send", lora_menu->index);
        } else {
            lora_pcp_cmd_t lora_cmd = {0}; // Zero out
            lora_cmd.index = lora_menu->submenu.index;
            memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
            // Remove colons
            int h1, m1, s1, h2, m2, s2;
            sscanf(start_time, "%2d:%2d:%2d", &h1,&m1,&s1);
            sscanf(end_time, "%2d:%2d:%2d", &h2,&m2,&s2);
            snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "d %s o %02d%02d%02d f %02d%02d%02d",
                    plan_selected_days, h1, m1, s1, h2, m2, s2);

            // Confirmation text
            lv_obj_t *lbl_send_conf = lv_label_create(ACTIVE_SCR); // Create and format label
            lcd_format_label(lbl_send_conf, "Sending to PolyPlug...", user_secondary_color,
                    &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
            lv_timer_handler();
            xQueueOverwrite(xLoraSendEncQueue, &lora_cmd); // Send the command
            xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drain stale receipt so only this command's ACK shows

            vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1000ms
            lv_obj_delete(lbl_send_conf); // Delete label
        }
        lcd_clear_pending_inputs = true;
        
        // Reset statics
        lbl_subpage_times = lbl_selected_icon = lbl_ins = NULL;
        selected_digit = 0;
        strcpy(start_time, "00:00:00");
        strcpy(end_time, "00:00:00");
        init = false;

        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

        // Go back
        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->left_btn == 1 && selected_digit == 0) { // Go back
        // Reset objects
        lv_obj_delete(lbl_subpage_times);
        lv_obj_delete(lbl_selected_icon);
        lv_obj_delete(lbl_ins);
        for (int i = 0; i < 17; ++i) {
            lv_obj_delete(time_labels[i]);
        }

        // Reset statics
        lbl_subpage_times = lbl_selected_icon = lbl_ins = NULL;
        selected_digit = 0;
        strcpy(start_time, "00:00:00");
        strcpy(end_time, "00:00:00");
        init = false;

        // Reveal the plan container and label
        lv_obj_remove_flag(lora_plan_menu->plan_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lora_plan_menu->lbl_days_ins, LV_OBJ_FLAG_HIDDEN);

        lcd_lora_update_plan_menu(lora_plan_menu);

        // Go back
        ui_menu->page = LORA_PLAN_SUBPAGE;
    } else if (ui_btns->right_btn == 1) { // Digit right
        // Increment with wrap
        selected_digit = (selected_digit + 1) % PLAN_TIME_DIGITS;
        
        // Set cursor X position
        lv_obj_set_x(lbl_selected_icon, cursor_x_offsets[selected_digit]);
        
        // Update
        update_time_label(time_labels, start_time, end_time);
    } else if (ui_btns->left_btn == 1) { // Digit left
        // Decrement with wrap
        selected_digit = (selected_digit + PLAN_TIME_DIGITS - 1) % PLAN_TIME_DIGITS;
        
        // Set cursor X position
        lv_obj_set_x(lbl_selected_icon, cursor_x_offsets[selected_digit]);
        
        // Update
        update_time_label(time_labels, start_time, end_time);
    } else if (ui_btns->up_btn == 1) { // Digit up
        // Select time string and adjusted pos (0-5)
        char *current_time_str = (selected_digit < 6) ? start_time : end_time; // Which half
        uint8_t adj_pos = selected_digit % 6;
        int digit_val = get_digit(current_time_str, adj_pos);
        int max_val = 9;
        int min_val = 0;

        // Range limits based on position
        if (adj_pos == 0) { // Tens of hours (0-2)
            max_val = 2;
        } else if (adj_pos == 1) { // Hours
            max_val = (get_digit(current_time_str, 0) == 2) ? 3 : 9;
        } else if (adj_pos == 2 || adj_pos == 4) { // Tens of min/sec (0-5)
            max_val = 5;
        }

        // Increment with wrap
        digit_val = (digit_val + 1 > max_val) ? min_val : digit_val + 1;
        
        // Update
        set_digit(current_time_str, adj_pos, digit_val);
        // Keep hours <= 23: clamp ones-of-hours when tens-of-hours becomes 2
        if (adj_pos == 0 && digit_val == 2 && get_digit(current_time_str, 1) > 3) {
            set_digit(current_time_str, 1, 3);
        }
        update_time_label(time_labels, start_time, end_time);
    } else if (ui_btns->down_btn == 1) { // Digit down
        // Select time string and adjusted pos (0-5)
        char *current_time_str = (selected_digit < 6) ? start_time : end_time; // Which half
        uint8_t adj_pos = selected_digit % 6;
        int digit_val = get_digit(current_time_str, adj_pos);
        int max_val = 9;
        int min_val = 0;

        // Range limits based on position
        if (adj_pos == 0) { // Tens of hours (0-2)
            max_val = 2;
        } else if (adj_pos == 1) { // Hours
            max_val = (get_digit(current_time_str, 0) == 2) ? 3 : 9;
        } else if (adj_pos == 2 || adj_pos == 4) { // Tens of min/sec (0-5)
            max_val = 5;
        }

        // Decrement with wrap
        digit_val = (digit_val - 1 < min_val) ? max_val : digit_val - 1;
        
        // Update
        set_digit(current_time_str, adj_pos, digit_val);
        // Keep hours <= 23: clamp ones-of-hours when tens-of-hours becomes 2
        if (adj_pos == 0 && digit_val == 2 && get_digit(current_time_str, 1) > 3) {
            set_digit(current_time_str, 1, 3);
        }
        update_time_label(time_labels, start_time, end_time);
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Reset objects
        lv_obj_delete(lbl_subpage_times);
        lv_obj_delete(lbl_selected_icon);
        lv_obj_delete(lbl_ins);
        for (int i = 0; i < 17; ++i) {
            lv_obj_delete(time_labels[i]);
        }

        // Reset statics
        lbl_subpage_times = lbl_selected_icon = lbl_ins = NULL;
        selected_digit = 0;
        strcpy(start_time, "00:00:00");
        strcpy(end_time, "00:00:00");
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_lora_update_plan_menu(lora_plan_menu_t *lora_plan_menu)
{
    /* Update container */
    // Wrap index
    if (lora_plan_menu->plan_index >= LORA_PLAN_SUBMENU_COUNT) {
        lora_plan_menu->plan_index = 0;
    } else if (lora_plan_menu->plan_index < 0) {
        lora_plan_menu->plan_index = LORA_PLAN_SUBMENU_COUNT - 1;
    }

    // Reset every button to unselected
    for (int i = 0; i < LORA_PLAN_SUBMENU_COUNT; ++i) {
        lv_obj_remove_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_sel_style, 0);
        lv_obj_add_style(lora_plan_menu->plan_btns[i], &lora_plan_menu->plan_btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(lora_plan_menu->plan_btns[lora_plan_menu->plan_index], &lora_plan_menu->plan_btn_style, 0);
    lv_obj_add_style(lora_plan_menu->plan_btns[lora_plan_menu->plan_index], &lora_plan_menu->plan_sel_style, 0);
}

void lcd_lora_away_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{
    // Create statics
    static lora_menu_t *away_menu;
    static bool do_once = false;
    
    if (!do_once) {        
        // Allocate for away_menu
        away_menu = malloc(sizeof(lora_menu_t));
        if (!away_menu) {
            ESP_LOGE(TAG, "Failed to allocate away_menu");
            return;
        }
        
        // Zero out the struct
        memset(away_menu, 0, sizeof(*away_menu));

        // Fill entries
        away_menu->size = 6;
        away_menu->index = 0;
        away_menu->options[0] = "Add Custom";
        away_menu->options[1] = "10-60m ON/OFF";
        away_menu->options[2] = "5-30m ON/OFF";
        away_menu->options[3] = "1-15m ON/OFF";
        away_menu->options[4] = "1-5m ON/OFF";
        away_menu->options[5] = "0-1m ON/OFF";
        
        // Create everything
        lcd_lora_setup_page(ui_menu, away_menu);
        
        // Show and assign to first element
        away_menu->index = 0;
        lcd_lora_update_menu(away_menu);
        
        do_once = true;
    }
    
    // Back selected
    if (ui_btns->left_btn == 1) {
        // Delete away_menu lv_obj
        lv_obj_delete(away_menu->main_list);
        
        // Free the styles
        lv_style_reset(&away_menu->btn_style);
        lv_style_reset(&away_menu->sel_style);
        
        // Free what was allocated
        free(away_menu);
        
        // Reset statics
        do_once = false;
        away_menu = NULL;
        
        // Show right arrow
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);
        
        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Delete away_menu lv_obj
        lv_obj_delete(away_menu->main_list);
        
        // Free the styles
        lv_style_reset(&away_menu->btn_style);
        lv_style_reset(&away_menu->sel_style);
        
        // Free what was allocated
        free(away_menu);
        
        // Reset statics
        do_once = false;
        away_menu = NULL;
        
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    } else if (ui_btns->up_btn == 1) { // Scroll up pressed
        // Update selection
        away_menu->index--;
        lcd_lora_update_menu(away_menu);
    } else if (ui_btns->down_btn == 1) { // Scroll down pressed
        // Update selection
        away_menu->index++;
        lcd_lora_update_menu(away_menu);
    } else if (ui_btns->select_btn == 1 && away_menu->index == 0) { // Custom selected
        // Delete away_menu lv_obj
        lv_obj_delete(away_menu->main_list);
        
        // Free the styles
        lv_style_reset(&away_menu->btn_style);
        lv_style_reset(&away_menu->sel_style);
        
        // Free what was allocated
        free(away_menu);
        
        // Reset statics
        do_once = false;
        away_menu = NULL;
        
        // Show right arrow
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        
        ui_menu->page = LORA_AWAY_CUSTOM_SUBPAGE;
    } else if (ui_btns->select_btn == 1 && away_menu->index != 0) { // Specific option selected
        // Hide away_menu
        lv_obj_add_flag(away_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        
        // Send the data to lora_task
        if (lora_menu->keys[lora_menu->index] == NULL) { // No key saved (interrupted pairing); don't send
            ESP_LOGE(TAG, "Missing LoRa key for index %d; skipping send", lora_menu->index);
        } else {
            lora_pcp_cmd_t lora_cmd = {0}; // Zero out
            lora_cmd.index = lora_menu->submenu.index;
            memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
            snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "away %s", away_menu->options[away_menu->index]);

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Sending LoRa AWAY cmd instr '%s'", lora_cmd.instr);
#endif

            // Confirmation text
            lv_obj_t *lbl_send_conf = lv_label_create(ACTIVE_SCR); // Create and format label
            lcd_format_label(lbl_send_conf, "Sending to PolyPlug...", user_secondary_color,
                    &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
            lv_timer_handler();

            xQueueOverwrite(xLoraSendEncQueue, &lora_cmd); // Send
            xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drain stale receipt so only this command's ACK shows
            vTaskDelay(pdMS_TO_TICKS(500)); // Wait additional 500ms

            lv_obj_delete(lbl_send_conf); // Delete label
        }
        lcd_clear_pending_inputs = true;
        
        // Delete away_menu lv_obj
        lv_obj_delete(away_menu->main_list);

        // Free the styles
        lv_style_reset(&away_menu->btn_style);
        lv_style_reset(&away_menu->sel_style);

        // Free what was allocated
        free(away_menu);

        // Reset statics
        do_once = false;
        away_menu = NULL;
        
        // Show right arrow
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = LORA_SUBPAGE;
    }
}

void lcd_lora_away_custom_subpage(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu)
{    
    #define AWAY_CUSTOM_X_POS 54
    #define AWAY_CUSTOM_Y_POS 40
    #define AWAY_CUSTOM_BUF_SIZE 8
    #define AWAY_CUSTOM_X_OFFSET 110

    // Statics
    static bool do_once = false;
    static uint8_t user_idx = 0; // 0 = Min, 1 = Max
    static int16_t min_val = 1;
    static int16_t max_val = 10;

    static lv_obj_t *lbl_unit;
    static lv_obj_t *lbl_ins;
    static lv_obj_t *lbl_min;
    static lv_obj_t *lbl_max;
    static lv_obj_t *lbl_val_min;
    static lv_obj_t *lbl_val_max;
    static lv_obj_t *lbl_pointer;

    static lv_style_t style_box;

    // Only execute once
    if (!do_once) {
        // Default values
        user_idx = 0;
        min_val = 1;
        max_val = 10;
        
        // Instruction label
        lbl_ins = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_ins, "Press select to send!", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, 0, 15);

        // Headings
        lbl_min = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_min, "Min\n", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, AWAY_CUSTOM_X_POS - AWAY_CUSTOM_X_OFFSET, AWAY_CUSTOM_Y_POS);

        lbl_max = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_max, "Max\n", user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, AWAY_CUSTOM_X_POS, AWAY_CUSTOM_Y_POS);

        // Values
        char buf[AWAY_CUSTOM_BUF_SIZE];
        snprintf(buf, sizeof(buf), "%d", min_val);
        lbl_val_min = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_val_min, buf, user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_TOP_MID, AWAY_CUSTOM_X_POS - AWAY_CUSTOM_X_OFFSET, AWAY_CUSTOM_Y_POS + 25);

        snprintf(buf, sizeof(buf), "%d", max_val);
        lbl_val_max = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_val_max, buf, user_secondary_color,
                &lv_font_montserrat_24, LV_ALIGN_TOP_MID, AWAY_CUSTOM_X_POS, AWAY_CUSTOM_Y_POS + 25);

        // Pointer
        lbl_pointer = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_pointer, LV_SYMBOL_EJECT, user_secondary_color,
                &lv_font_montserrat_18, LV_ALIGN_TOP_MID, AWAY_CUSTOM_X_POS - AWAY_CUSTOM_X_OFFSET, AWAY_CUSTOM_Y_POS + 58);
        
        // Result
        lbl_unit = lv_label_create(ACTIVE_SCR);
        lcd_format_label(lbl_unit, "(minutes)", user_secondary_color,
                &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -17);
        
        // Box style for headings
        lv_style_reset(&style_box);
        lv_style_init(&style_box);
        lv_style_set_radius(&style_box, 8);
        lv_style_set_bg_color(&style_box, user_primary_color);
        lv_style_set_border_width(&style_box, 2);
        lv_style_set_border_color(&style_box, user_secondary_color);
        lv_style_set_border_side(&style_box, LV_BORDER_SIDE_FULL);
        lv_style_set_text_color(&style_box, user_secondary_color);
        lv_style_set_pad_left(&style_box, 25);
        lv_style_set_pad_right(&style_box, 25);
        lv_style_set_pad_top(&style_box, 4);
        lv_style_set_pad_bottom(&style_box, 4);

        lv_obj_add_style(lbl_min, &style_box, 0);
        lv_obj_add_style(lbl_max, &style_box, 0);

        do_once = true;
    }

    // Send
    if (ui_btns->select_btn == 1) {
        // Ensure min <= max (swap if needed)
        if (max_val < min_val) {
            int16_t tmp = min_val;
            min_val = max_val;
            max_val = tmp;

            char buf_a[AWAY_CUSTOM_BUF_SIZE], buf_b[AWAY_CUSTOM_BUF_SIZE];
            snprintf(buf_a, sizeof(buf_a), "%d", min_val);
            snprintf(buf_b, sizeof(buf_b), "%d", max_val);
            lv_label_set_text(lbl_val_min, buf_a);
            lv_label_set_text(lbl_val_max, buf_b);
        }
        
        /* Send the data */
        
        // Remove styles
        lv_obj_remove_style_all(lbl_min);
        lv_obj_remove_style_all(lbl_max);
        
        // Delete objects
        lv_obj_delete(lbl_unit);
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_min);
        lv_obj_delete(lbl_max);
        lv_obj_delete(lbl_val_min);
        lv_obj_delete(lbl_val_max);
        lv_obj_delete(lbl_pointer);
        
        // Reset statics
        do_once = false;
        lbl_unit = lbl_ins = lbl_min = lbl_max = lbl_val_min = lbl_val_max = lbl_pointer = NULL;
        
        // Send the data to lora_task
        if (lora_menu->keys[lora_menu->index] == NULL) { // No key saved (interrupted pairing); don't send
            ESP_LOGE(TAG, "Missing LoRa key for index %d; skipping send", lora_menu->index);
        } else {
            lora_pcp_cmd_t lora_cmd = {0}; // Zero out
            lora_cmd.index = lora_menu->submenu.index;
            memcpy(lora_cmd.key, lora_menu->keys[lora_menu->index], LORA_PCP_ENC_KEY_LEN);
            snprintf(lora_cmd.instr, sizeof(lora_cmd.instr), "away %d-%dm ON/OFF", min_val, max_val); // Keep formatting

#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Sending LoRa AWAY cmd instr '%s'", lora_cmd.instr);
#endif

            // Confirmation text
            lv_obj_t *lbl_send_conf = lv_label_create(ACTIVE_SCR); // Create and format label
            lcd_format_label(lbl_send_conf, "Sending to PolyPlug...", user_secondary_color,
                    &lv_font_montserrat_18, LV_ALIGN_CENTER, 0, 0);
            lv_timer_handler();

            xQueueOverwrite(xLoraSendEncQueue, &lora_cmd); // Send
            xSemaphoreTake(xLoraReceiptValidSemaphore, 0); // Drain stale receipt so only this command's ACK shows
            vTaskDelay(pdMS_TO_TICKS(500)); // Wait additional 500ms
            lcd_clear_pending_inputs = true;

            lv_obj_delete(lbl_send_conf); // Delete label
        }
        
        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->right_btn == 1) { // Move right (toggle min/max)
        // Point to max
        if (user_idx == 0) {
            lv_obj_set_x(lbl_pointer, AWAY_CUSTOM_X_POS);
            user_idx = 1;
        } else { // Back to min
            lv_obj_set_x(lbl_pointer, AWAY_CUSTOM_X_POS - AWAY_CUSTOM_X_OFFSET);
            user_idx = 0;
        }
    } else if (ui_btns->left_btn == 1 && user_idx != 0) { // Move left inside page (only if currently on max)
        lv_obj_set_x(lbl_pointer, AWAY_CUSTOM_X_POS - AWAY_CUSTOM_X_OFFSET);
        user_idx = 0;
    } else if (ui_btns->up_btn == 1) { // Increment value
        if (user_idx == 0) {
            min_val++;
            char buf[AWAY_CUSTOM_BUF_SIZE];
            snprintf(buf, sizeof(buf), "%d", min_val);
            lv_label_set_text(lbl_val_min, buf);
        } else {
            max_val++;
            char buf[AWAY_CUSTOM_BUF_SIZE];
            snprintf(buf, sizeof(buf), "%d", max_val);
            lv_label_set_text(lbl_val_max, buf);
        }
    } else if (ui_btns->down_btn == 1) { // Decrement value
        if (user_idx == 0) {
            min_val--;
            if (min_val < 0) {
                min_val = 0;
            }
            
            char buf[AWAY_CUSTOM_BUF_SIZE];
            snprintf(buf, sizeof(buf), "%d", min_val);
            lv_label_set_text(lbl_val_min, buf);
        } else {
            max_val--;
            if (max_val < 0) {
                max_val = 0;
            }
            
            char buf[AWAY_CUSTOM_BUF_SIZE];
            snprintf(buf, sizeof(buf), "%d", max_val);
            lv_label_set_text(lbl_val_max, buf);
        }
    } else if (ui_btns->left_btn == 1) { // Back selected and pointer is on min
        // Remove styles
        lv_obj_remove_style_all(lbl_min);
        lv_obj_remove_style_all(lbl_max);
        
        // Delete objects
        lv_obj_delete(lbl_unit);
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_min);
        lv_obj_delete(lbl_max);
        lv_obj_delete(lbl_val_min);
        lv_obj_delete(lbl_val_max);
        lv_obj_delete(lbl_pointer);

        // Reset statics
        do_once = false;
        lbl_unit = lbl_ins = lbl_min = lbl_max = lbl_val_min = lbl_val_max = lbl_pointer = NULL;

        // Show LoRa submenu cont
        lv_obj_remove_flag(lora_menu->submenu.cont, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = LORA_SUBPAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Remove styles
        lv_obj_remove_style_all(lbl_min);
        lv_obj_remove_style_all(lbl_max);
        
        // Delete objects
        lv_obj_delete(lbl_unit);
        lv_obj_delete(lbl_ins);
        lv_obj_delete(lbl_min);
        lv_obj_delete(lbl_max);
        lv_obj_delete(lbl_val_min);
        lv_obj_delete(lbl_val_max);
        lv_obj_delete(lbl_pointer);

        // Reset statics
        do_once = false;
        lbl_unit = lbl_ins = lbl_min = lbl_max = lbl_val_min = lbl_val_max = lbl_pointer = NULL;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

esp_err_t lcd_lora_menu_nvs_save(const lora_menu_t *menu)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(LORA_OPTIONS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    // menu->options[0..LORA_NUM_STATIC_OPTS-1] are static entries; only user plugs are persisted
    uint8_t user_cnt = (menu->size > LORA_NUM_STATIC_OPTS) ? menu->size - LORA_NUM_STATIC_OPTS : 0;
    err = nvs_set_u8(h, LORA_OPTIONS_KEY_COUNT, user_cnt);

    // If error, exit
    if (err != ESP_OK)
        goto out;

    // Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt; ++i) {
        char key[16];
        snprintf(key, sizeof(key), LORA_OPTIONS_KEY_FMT, i);

        // Store the menu option string; user plugs start after the static entries
        err = nvs_set_str(h, key, menu->options[i + LORA_NUM_STATIC_OPTS]);
        
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

esp_err_t lcd_lora_key_nvs_save(const lora_menu_t *menu)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    // menu->keys[0..LORA_NUM_STATIC_OPTS-1] belong to static entries; only user plugs are persisted
    uint8_t user_cnt = (menu->size > LORA_NUM_STATIC_OPTS) ? menu->size - LORA_NUM_STATIC_OPTS : 0;
    err = nvs_set_u8(h, LORA_ENC_KEY_COUNT, user_cnt);

    // If error, exit
    if (err != ESP_OK)
        goto out;

    // Loop through all and number them: n00, n01, etc.
    for (uint8_t i = 0; i < user_cnt; ++i) {
        char key[16];
        snprintf(key, sizeof(key), LORA_ENC_KEY_FMT, i);

        // Store the key blob; user plugs start after the static entries to match user options
        err = nvs_set_blob(h, key, menu->keys[i + LORA_NUM_STATIC_OPTS], LORA_PCP_ENC_KEY_LEN);
        
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

esp_err_t lcd_lora_menu_nvs_load(lora_menu_t *menu)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(LORA_OPTIONS_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;

    // Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_OPTIONS_KEY_COUNT, &user_cnt);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    menu->size = LORA_NUM_STATIC_OPTS; // Keep the static entries; append user plugs after them
    menu->index = 0;

    // Loop through all keys
    for (uint8_t i = 0; i < user_cnt; ++i) {

        char key[16];
        snprintf(key, sizeof(key), LORA_OPTIONS_KEY_FMT, i);
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
        if (menu->size >= MAX_LORA_OPTIONS) {
            free(buf);
            break;
        }
        menu->options[menu->size++] = buf;
    }
    
    // Close NVS
    nvs_close(h);
    
    return ESP_OK;
}

esp_err_t lcd_lora_key_nvs_load(lora_menu_t *menu)
{
    nvs_handle_t h;
        
    // Open NVS
    esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;

    // Get number of saved items
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_ENC_KEY_COUNT, &user_cnt);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    menu->size = LORA_NUM_STATIC_OPTS; // Keep the static entries; append user plug keys after them
    menu->index = 0;

    // Loop through all keys
    for (uint8_t i = 0; i < user_cnt; ++i) {

        char key[16];
        snprintf(key, sizeof(key), LORA_ENC_KEY_FMT, i);
        
        // Read exactly LORA_PCP_ENC_KEY_LEN bytes
        size_t blob_len = LORA_PCP_ENC_KEY_LEN;
        
        // First check existence & size
        err = nvs_get_blob(h, key, NULL, &blob_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK || blob_len != LORA_PCP_ENC_KEY_LEN) {
            break;
        }

        uint8_t *buf = malloc(LORA_PCP_ENC_KEY_LEN);
        if (!buf) {
            err = ESP_ERR_NO_MEM;
            break;
        }

        err = nvs_get_blob(h, key, buf, &blob_len);
        if (err != ESP_OK) {
            free(buf);
            break;
        }

        // Update menu struct
        if (menu->size >= MAX_LORA_OPTIONS) {
            free(buf);
            break;
        }
        menu->keys[menu->size++] = buf;
    }
    
    // Close NVS
    nvs_close(h);

    return err;
}

void lcd_lora_menu_load_reconcile(lora_menu_t *menu)
{
    // lcd_lora_menu_nvs_load and lcd_lora_key_nvs_load run as a pair and each set menu->size independently
    // A partial failure in either can leave menu->size counting a slot whose options[] or keys[] is NULL
    // Trim to the first slot missing either half and free anything dangling beyond it

    // Find the first user slot (after the static entries) missing a name or a key
    int good = menu->size;
    for (int i = LORA_NUM_STATIC_OPTS; i < menu->size; ++i) {
        if (menu->options[i] == NULL || menu->keys[i] == NULL) {
            good = i;
            break;
        }
    }

    // Free and NULL every entry from the first incomplete slot to the end of the arrays
    for (int i = good; i < MAX_LORA_OPTIONS; ++i) {
        free(menu->options[i]);
        menu->options[i] = NULL;
        free(menu->keys[i]);
        menu->keys[i] = NULL;
    }

    menu->size = good;
    if (menu->index >= menu->size) {
        menu->index = 0;
    }
}

esp_err_t lcd_lora_menu_nvs_delete(uint8_t del_idx)
{
    // Open NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(LORA_OPTIONS_NS, NVS_READWRITE, &h);
    
    // Error check
    if (err != ESP_OK)
        return err;

    // Get current number of items in menu
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_OPTIONS_KEY_COUNT, &user_cnt);

    // del_idx is the menu index; static entries are never stored in NVS
    // Convert to the 0-based user/NVS index and range-check
    if (err != ESP_OK || del_idx < LORA_NUM_STATIC_OPTS ||
            (del_idx - LORA_NUM_STATIC_OPTS) >= user_cnt) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t nvs_idx = del_idx - LORA_NUM_STATIC_OPTS;

    // Shift every key above nvs_idx down one slot
    for (uint8_t i = nvs_idx + 1; i < user_cnt; ++i) {
        char key_src[16], key_dst[16];

        // Format key
        snprintf(key_src, sizeof key_src, LORA_OPTIONS_KEY_FMT, i);
        snprintf(key_dst, sizeof key_dst, LORA_OPTIONS_KEY_FMT, i - 1);

        // Get key length
        size_t len = 0;
        if ((err = nvs_get_str(h, key_src, NULL, &len)) != ESP_OK)
            break;
        
        // Store
        char *buf = malloc(len);
        if (!buf) {
            err = ESP_ERR_NO_MEM; 
            break;
        }

        // Get the string
        err = nvs_get_str(h, key_src, buf, &len);
        
        // Set it to new destination
        if (err == ESP_OK)
            err = nvs_set_str(h, key_dst, buf);
        
        // Free buffer
        free(buf);
        
        if (err != ESP_OK)
            break;
    }

    // Erase the dangling last slot
    if (err == ESP_OK) {
        char key_last[16];
        snprintf(key_last, sizeof key_last, LORA_OPTIONS_KEY_FMT, user_cnt - 1);
        err = nvs_erase_key(h, key_last);
    }

    // Update count and commit changes to NVS
    if (err == ESP_OK) {
        err = nvs_set_u8(h, LORA_OPTIONS_KEY_COUNT, user_cnt - 1);
        if (err == ESP_OK)
            err = nvs_commit(h);
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t lcd_lora_key_nvs_delete(uint8_t del_idx)
{
    // Open NVS
    nvs_handle_t h;
    esp_err_t err = nvs_open(LORA_ENC_NS, NVS_READWRITE, &h);
    
    // Error check
    if (err != ESP_OK) {
        return err;
    }

    // Get number of keys
    uint8_t user_cnt = 0;
    err = nvs_get_u8(h, LORA_ENC_KEY_COUNT, &user_cnt);

    // del_idx is the menu index; static entries are never stored in NVS
    // Convert to the 0-based user/NVS index and range-check
    if (err != ESP_OK || del_idx < LORA_NUM_STATIC_OPTS ||
            (del_idx - LORA_NUM_STATIC_OPTS) >= user_cnt) {
        nvs_close(h);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t nvs_idx = del_idx - LORA_NUM_STATIC_OPTS;

    // Buffer
    uint8_t tmp[LORA_PCP_ENC_KEY_LEN];

    // Shift all keys above nvs_idx down one
    for (uint8_t i = nvs_idx + 1; i < user_cnt; ++i) {
        char src[16], dst[16];

        // Format key
        snprintf(src, sizeof src, LORA_ENC_KEY_FMT, i);
        snprintf(dst, sizeof dst, LORA_ENC_KEY_FMT, i - 1);

        size_t len = LORA_PCP_ENC_KEY_LEN;
        // Get key from src
        err = nvs_get_blob(h, src, tmp, &len);
        if (err != ESP_OK || len != LORA_PCP_ENC_KEY_LEN) {
            break;
        }

        // Set key to new dst
        err = nvs_set_blob(h, dst, tmp, LORA_PCP_ENC_KEY_LEN);
        if (err != ESP_OK) {
            break;
        }
    }

    // Erase dangling key
    if (err == ESP_OK) {
        char last[16];
        snprintf(last, sizeof last, LORA_ENC_KEY_FMT, user_cnt - 1);
        err = nvs_erase_key(h, last);
    }
    
    // Set new count
    if (err == ESP_OK) {
        err = nvs_set_u8(h, LORA_ENC_KEY_COUNT, user_cnt - 1);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }

    // Close NVS
    nvs_close(h);
    return err;
}