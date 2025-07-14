#ifndef LCD_FUNCS_H
#define LCD_FUNCS_H

#include "lvgl.h"

#include "lcd_ir_funcs.h"
#include "lcd_lora_funcs.h"
#include "lcd_espnow_funcs.h"
#include "lcd_wifi_funcs.h"
#include "lcd_tools_funcs.h"
#include "lcd_settings_funcs.h"

#include "gpio_funcs.h"

#define HOR_RES 240
#define VER_RES 135

// Define each sequentially (0, 1, 2, ...)
enum {
    HOME_PAGE,
    SELECTION_PAGE,
    
    LORA_PAGE,
    ESPNOW_PAGE,
    INFRARED_PAGE,
    SETTINGS_PAGE,
    TOOLS_PAGE,
    WIFI_PAGE,
    BLUETOOTH_PAGE,
    
    INFRARED_REMOTE_NAME_PAGE,
    INFRARED_REMOTE_EDIT_PAGE,
    
    LORA_NAME_PAGE,
    LORA_KEY_PAGE,
    LORA_SUBPAGE,
    LORA_LOOP_SUBPAGE,
    LORA_AWAY_SUBPAGE,
    LORA_PLAN_SUBPAGE,
    LORA_PLAN_CONFIRM_SUBPAGE,
    
    ESPNOW_RX_MAC_PAGE,
    ESPNOW_NAME_PAGE,
    ESPNOW_OPTION_PAGE,
    
    WIFI_SCAN_PAGE,
    WIFI_PASSWORD_PAGE,
    WIFI_BEACON_PAGE,
    WIFI_DATA_PAGE,
    WIFI_SYNC_PAGE,
    WIFI_SEND_PAGE,
    WIFI_NAME_PAGE,
    
    TOOLS_COIN_PAGE,
    TOOLS_DOCS_PAGE,
    TOOLS_DICE_PAGE,
    
    SETTINGS_COLORS_PAGE,
    SETTINGS_COLORS_SEL_PAGE,
    SETTINGS_FACTORY_RST_PAGE
};

#define ACTIVE_SCR (lv_scr_act())

extern lv_color_t user_primary_color;
extern lv_color_t user_secondary_color;

extern volatile bool lcd_clear_pending_inputs;
extern volatile bool go_to_sleep;

typedef struct ui_menu_t {
    const char **options; // your array of strings
    int size; // how many entries
    int index; // the one that’s currently in the middle
    int page;
    lv_obj_t *btn_mid;
    lv_obj_t *lbl_top; // the three labels on screen
    lv_obj_t *lbl_mid;
    lv_obj_t *lbl_bot;
    lv_obj_t *arrow_bot;
    lv_obj_t *arrow_top;
    lv_obj_t *arrow_right;
    lv_obj_t *arrow_left;
    lv_obj_t *lbl_battery_txt;
    lv_obj_t *lbl_battery_icon;
} ui_menu_t;

typedef struct ui_btns_t {
    bool up_btn;
    bool down_btn;
    bool right_btn;
    bool left_btn;
    bool select_btn;
    bool home_btn;
    bool pwr_btn;
} ui_btns_t;

extern ui_btns_t ui_btns;

/** 
 * @brief Put device and LCD into sleep mode
 */
void lcd_device_sleep(void);

/** 
 * @brief Initialise SPI bus + ST7789 panel (blocking)
 */
void lcd_init_driver(void);

/**
 * @brief Initialise LVGL draw buffers, tick timer and register flush cb
 */
void lcd_lvgl_init(void);

/**
 * @brief Initialize and create selection labels
 */
void lcd_init_selection_labels(ui_menu_t *ui_menu);

/**
 * @brief Clears all NVS for namespace
 */
void lcd_ns_nvs_clear(const char* ns);

/**
 * @brief Switch page to home, redraw, then set sleep flag
 *
 * @param [in] home Go home or sleep
 * @param [in] ui_menu UI menu structure
 */
void lcd_funcs_transition_back(bool home, ui_menu_t *ui_menu);

/**
 * @brief Format labels
 *
 * @param [in] label Label to format
 * @param [in] text Label text
 * @param [in] color Label color
 * @param [in] font Label font
 * @param [in] alignment Alignment via LVGL function
 * @param [in] x_offset X position offset
 * @param [in] y_offset Y position offset
 */
void lcd_format_label(lv_obj_t *label, const char *text, lv_color_t  color, const lv_font_t *font, lv_align_t  alignment, lv_coord_t  x_offset, lv_coord_t  y_offset);

/**
 * @brief Swap labels for scroll animation
 *
 * @param [in] lbl_top Top label
 * @param [in] lbl_mid Middle label
 * @param [in] lbl_bot Bottom label
 * @param [in] new_bot_text Text to replace bottom label
 */
void lcd_scroll_up(lv_obj_t *lbl_top, lv_obj_t *lbl_mid, lv_obj_t *lbl_bot, const char *new_bot_text);

/**
 * @brief Swap labels for scroll animation
 *
 * @param [in] lbl_top Top label
 * @param [in] lbl_mid Middle label
 * @param [in] lbl_bot Bottom label
 * @param [in] new_top_text Text to replace top label
 */
void lcd_scroll_down(lv_obj_t *lbl_top, lv_obj_t *lbl_mid, lv_obj_t *lbl_bot, const char *new_top_text);

/**
 * @brief Perform scroll animation for wireless selection page (up or down)
 *
 * @param [in] menu UI menu structure
 * @param [in] txt Pass new text at top or bottom into animation callback for scroll functions
 * @param [in] scrolling_up Direction being scrolled (up/!up)
 * @param [in] speed_px_s Speed to move animation
 */
void lcd_scroll_anim(ui_menu_t *menu, const char *txt, bool scrolling_up, uint32_t speed_px_s);

/**
 * @brief Perform swipe animation for wireless selection page (left or right)
 *
 * @param [in] menu UI menu structure
 * @param [in] swipe_left Direction being swiped (left/!left)
 * @param [in] speed_px_s Speed to move animation
 */
void lcd_swipe_anim(ui_menu_t *menu, bool swipe_left, uint32_t speed_px_s);
							  
/**
 * @brief Format main center button for wireless selection page
 *
 * @param [in] btn_mid Button to format
 * @param [in] user_primary_color Color for button background
 * @param [in] user_secondary_color Color for button outline
 */
void lcd_format_center_button(lv_obj_t *btn_mid, lv_color_t user_primary_color, lv_color_t user_secondary_color);

/**
 * @brief Initialize images to be used in animation
 */
void lcd_init_images();

/**
 * @brief Clear all user inputs
 */
void lcd_clear_user_in();

/**
 * @brief Updates battery label and icon
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] battery_percentage Battery percentage read and calculated from adc_task
*/
void lcd_update_battery(ui_menu_t *ui_menu, uint8_t battery_percentage);

/**
 * @brief Display/scroll through home page animations
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_home_page(ui_menu_t *ui_menu, ui_btns_t *ui_btns);

/**
 * @brief Executes on selection page
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ui_btns UI input structure
 * @param [in] ir_menu IR menu structure
 * @param [in] lora_menu LoRa menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 * @param [in] tools_menu Tools menu structure
  @param [in] settings_menu Settings menu structure
 */
void lcd_selection_page(ui_menu_t *ui_menu, ui_btns_t *ui_btns, ir_menu_t* ir_menu, lora_menu_t* lora_menu, espnow_menu_t* espnow_menu, wifi_menu_t* wifi_menu, tools_menu_t* tools_menu, settings_menu_t* settings_menu);

/**
 * @brief Executes on infrared page
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu Infrared menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_infrared_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t *ir_menu ); 

/**
 * @brief Executes on LoRa page
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] lora_menu LoRa menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_lora_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, lora_menu_t *lora_menu);

/**
 * @brief Executes on ESP-NOW page
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] espnow_menu LoRa menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_espnow_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu);

/**
 * @brief Executes on Wi-Fi page
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 * @param [in] ui_btns UI input structure
 */
void lcd_wifi_page(ui_btns_t  *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Executes on tools page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Executes on settings page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_settings_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);


#endif /* LCD_FUNCS_H */