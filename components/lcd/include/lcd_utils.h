#ifndef LCD_FUNCS_H
#define LCD_FUNCS_H

#include <stdint.h>

#include "lvgl.h"

#include "lcd_anim.h"
#include "lcd_infrared.h"
#include "lcd_lora.h"
#include "lcd_espnow.h"
#include "lcd_wifi.h"
#include "lcd_tools.h"
#include "lcd_games.h"
#include "lcd_settings.h"
#include "lcd_hotkey.h"
#include "lcd_bluetooth.h"
#include "lcd_gpio.h"

#include "gpio_utils.h"

#include "wifi_task.h" // icon_state_t

#define ACTIVE_SCR (lv_scr_act())

#define HOR_RES 240
#define VER_RES 135

#define LCD_LOADING_ANIM_START_DEFAULT() do { lcd_anim_loading_start(LV_ALIGN_BOTTOM_RIGHT, -10, -10, user_secondary_color); } while (0)

#define OPTION_GPIO "GPIO"
#define OPTION_WIFI "Wi-Fi"
#define OPTION_BLUETOOTH "Bluetooth"
#define OPTION_LORA "LoRa" // "PolyPlug"
#define OPTION_ESPNOW "ESP-NOW" // "ESP32"
#define OPTION_INFRARED "Infrared"
#define OPTION_TOOLS "Tools"
#define OPTION_GAMES "Games"
#define OPTION_SETTINGS "Settings"

#define LCD_WAIT_FOR_BIT_BETTER_SUCCESS 0
#define LCD_WAIT_FOR_BIT_BETTER_TIMEOUT 1
#define LCD_WAIT_FOR_BIT_BETTER_EXIT    2

// Define each sequentially (0, 1, 2, ...)
enum {
    BOOT_PAGE,
    HOME_PAGE,
    UNLOCK_PAGE,
    HOTKEY_PAGE,
    HOTKEY_OPTION_PAGE,
    SELECTION_PAGE,
    
    LORA_PAGE,
    ESPNOW_PAGE,
    INFRARED_PAGE,
    SETTINGS_PAGE,
    TOOLS_PAGE,
    GAMES_PAGE,
    WIFI_PAGE,
    BLUETOOTH_PAGE,
    
    INFRARED_REMOTE_NAME_PAGE,
    INFRARED_REMOTE_EDIT_PAGE,
    
    LORA_NAME_PAGE,
    LORA_ADD_PAGE,
    LORA_KEY_PAGE,
    LORA_SUBPAGE,
    LORA_LOOP_SUBPAGE,
    LORA_AWAY_SUBPAGE,
    LORA_AWAY_CUSTOM_SUBPAGE,
    LORA_PLAN_SUBPAGE,
    LORA_PLAN_CONFIRM_SUBPAGE,
    LORA_PLAN_TIMES_SUBPAGE,
    LORA_GPIO_SUBPAGE,
    
    ESPNOW_ECOMPASS_PAGE,
    ESPNOW_ECOMPASS_STREAM_PAGE,
    ESPNOW_ECOMPASS_CAL_PAGE,
    ESPNOW_RX_MAC_PAGE,
    ESPNOW_NAME_PAGE,
    ESPNOW_OPTION_PAGE,
    
    WIFI_SCAN_PAGE,
    WIFI_SCAN_DEAUTH_PAGE,
    WIFI_AI_CONFIG_PAGE,
    WIFI_AI_PACKET_PAGE,
    WIFI_AI_PACKET_RESULTS_PAGE,
    WIFI_PASSWORD_PAGE,
    WIFI_BEACON_PAGE,
    WIFI_DATA_PAGE,
    WIFI_SYNC_PAGE,
    WIFI_SEND_PAGE,
    WIFI_NAME_PAGE,
    WIFI_DEAUTH_PAGE,
    
    TOOLS_COIN_PAGE,
    TOOLS_DOCS_PAGE,
    TOOLS_DICE_PAGE,
    TOOLS_NUM_GEN_PAGE,
    TOOLS_BTC_ADDR_PAGE,
    TOOLS_BTC_ADDR_SETUP_PAGE,
    TOOLS_POMODORO_PAGE,
    TOOLS_HOW_SRS_PAGE,
    TOOLS_SRS_PAGE,
    TOOLS_CLAUDE_USAGE_PAGE,
    TOOLS_CLAUDE_SETUP_PAGE,

    SETTINGS_OTA_CONFIRM_PAGE,
    SETTINGS_OTA_UPDATING_PAGE,
    SETTINGS_COLORS_PAGE,
    SETTINGS_COLORS_SEL_PAGE,
    SETTINGS_FACTORY_RST_PAGE,
    SETTINGS_PIN_PAGE,
    SETTINGS_PIN_LOCKOUT_PAGE,
    SETTINGS_HAPTIC_PAGE,
    SETTINGS_RGB_LED_PAGE,
    SETTINGS_LCD_PAGE,
    SETTINGS_SYSTEM_PAGE,
    SETTINGS_HELP_PAGE,
    SETTINGS_SLEEP_TIMER_PAGE,
    
    BLUETOOTH_PAIRING_PAGE,
    BLUETOOTH_FORGET_ALL_PAGE,
    BLUETOOTH_MEDIA_CLASSIC_PAGE,
    BLUETOOTH_MEDIA_SCROLL_PAGE,
    BLUETOOTH_MEDIA_PRESENTATION_PAGE,
    BLUETOOTH_MEDIA_CAMERA_PAGE,
    BLUETOOTH_MEDIA_SOCIALS_PAGE,
    BLUETOOTH_AI_KEYBOARD_PAGE,
    BLUETOOTH_AI_CONFIG_PAGE,
    BLUETOOTH_KEYBOARD_PAGE,
    BLUETOOTH_KEYBOARD_SUB_PAGE,
    BLUETOOTH_SCRIPT_ADD_PAGE,
    BLUETOOTH_KNOWN_DEVICES_PAGE,
    BLUETOOTH_PAIR_NEW_PAGE,
    BLUETOOTH_RENAME_PEER_PAGE,
    
    GPIO_PAGE,
    GPIO_HOW_PAGE,
    GPIO_SCANNER_PAGE,
    GPIO_TERMINAL_PAGE,

    GAMES_TETRIS_PAGE,
    GAMES_TREX_PAGE,
    GAMES_FLAPPY_PAGE,
    GAMES_DOOM_PAGE,

    LORA_MESHTASTIC_PAGE,

    SETTINGS_LORA_SF_PAGE,

    SETTINGS_LORA_REGION_PAGE,
};

extern uint32_t pin_attempts;
extern uint32_t pin_lockout_seconds; // Remaining lockout duration in seconds after failed PIN attempts

extern lv_color_t user_primary_color;
extern lv_color_t user_secondary_color;

extern volatile bool lcd_clear_pending_inputs;
extern volatile bool go_to_sleep;

typedef struct ui_menu_t {
    const char **options; // Array of strings
    int size; // How many entries
    int index; // The one that's currently in the middle
    int page;
    lv_obj_t *scroll_bar;
    lv_obj_t *scroll_track;
    lv_obj_t *btn_mid;
    lv_obj_t *lbl_top;
    lv_obj_t *lbl_mid;
    lv_obj_t *lbl_bot;
    lv_obj_t *arrow_bot;
    lv_obj_t *arrow_top;
    lv_obj_t *arrow_right;
    lv_obj_t *arrow_left;
    lv_obj_t *lbl_battery_txt;
    lv_obj_t *lbl_battery_icon;
    lv_obj_t *lbl_hotkey_icon;
    lv_obj_t *lbl_wifi_icon;
    lv_obj_t *lbl_bluetooth_icon;
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

#ifdef POLYCAST5_PERSIST_SELECTION_INDEX
/**
 * @brief Load the previous selection index from NVS
 *
 * @param [in] ui_menu UI menu structure
 */
void lcd_selection_index_nvs_load(ui_menu_t *ui_menu);
#endif

/**
 * @brief Initialize and create selection labels
 *
 * @param [in] ui_menu UI menu structure
 */
void lcd_init_selection_labels(ui_menu_t *ui_menu);

/**
 * @brief Unhide and show selection labels
 *
 * @param [in] ui_menu UI menu structure
 */
void lcd_unhide_selection_widgets(ui_menu_t *ui_menu);

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
void lcd_transition_back(bool home, ui_menu_t *ui_menu);

/**
 * @brief Format labels
 *
 * @param [in] label Label to format
 * @param [in] text Label text
 * @param [in] color Label color
 * @param [in] font Label font
 * @param [in] alignment Alignment via LVGL function
 * @param [in] x_offset X offset from alignment (positive = right)
 * @param [in] y_offset Y offset from alignment (positive = down)
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
 * @param [in] charging If the battery is charging or not
*/
void lcd_update_battery(ui_menu_t *ui_menu, uint8_t battery_percentage, bool charging);

/**
 * @brief Mark that first boot has happened via NVS
 *
 * @returns ESP error status 
 */
esp_err_t lcd_save_first_boot(void);

/**
 * @brief Check if first boot has happened via NVS
 *
 * @returns True if first boot
 */
bool lcd_is_first_boot(void);

/**
 * @brief Format a clean scrollbar indicator
 *
 * @param [in] obj Object to apply the scrollbar to
 */
void lcd_apply_scrollbar_style(lv_obj_t *obj);

/**
 * @brief Create an uptime timer to recalculate uptime every 60 seconds
 */
void lcd_create_uptime_timer(void);

/**
 * @brief Get live total uptime in seconds (NVS prior + current boot time)
 */
uint64_t lcd_get_uptime_seconds(void);

/**
 * @brief Draw text as a QR into an LVGL canvas (RGB565)
 *
 * @param [out] canvas Canvas to draw the QR in to
 * @param [in] text Text to encode
 * @param [in] size_px Size of QR canvas
 * @param [in] pbuf Canvas backing buffer
 *
 * @returns 0 on success
 */
int lcd_draw_qr(lv_obj_t *canvas, const char *text, int size_px, uint8_t **pbuf);

/**
 * @brief Update connectivity icons for the LCD
 *
 * @param [in] icon_state Icon state structure
 * @param [in] ui_menu UI menu structure
 */
void lcd_update_icons(icon_state_t *icon_state, ui_menu_t *ui_menu);

/**
 * @brief Wait for a bit to be set in an event group with timeout and exit option
 *
 * @param [in] event_group Event group handle
 * @param [in] bit Bit to wait for
 * @param [in] timeout_ms Timeout in milliseconds
 *
 * @returns LCD_WAIT_FOR_BIT_BETTER_SUCCESS on success, LCD_WAIT_FOR_BIT_BETTER_TIMEOUT on timeout,
 *          LCD_WAIT_FOR_BIT_BETTER_EXIT on left button exit
 */
uint8_t lcd_wait_for_bit_better(EventGroupHandle_t event_group, EventBits_t bit, uint32_t timeout_ms);

/**
 * @brief Show one time boot up page with some starter info
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 */
void lcd_boot_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu);

/**
 * @brief Display/scroll through home page animations
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_home_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes unlock page where the user must enter a pin to gain access
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] settings_menu Settings menu structure
 */
void lcd_unlock_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, settings_menu_t *settings_menu);

/**
 * @brief Executes hotkey page where the user can configure hotkeys
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] hotkey_menu Hotkey menu structure
 */
void lcd_hotkey_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, hotkey_menu_t *hotkey_menu);

/**
 * @brief Executes on selection page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu IR menu structure
 * @param [in] lora_menu LoRa menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 * @param [in] wifi_menu Wi-Fi menu structure
 * @param [in] tools_menu Tools menu structure
 * @param [in] games_menu Games menu structure
 * @param [in] settings_menu Settings menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_selection_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, ir_menu_t* ir_menu, lora_menu_t* lora_menu, espnow_menu_t* espnow_menu,
        wifi_menu_t* wifi_menu, tools_menu_t* tools_menu, games_menu_t* games_menu, settings_menu_t* settings_menu, bluetooth_menu_t *bluetooth_menu, gpio_menu_t *gpio_menu);

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

/**
 * @brief Executes on bluetooth page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] bluetooth_menu Bluetooth menu structure
 */
void lcd_bluetooth_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, bluetooth_menu_t *bluetooth_menu);

/**
 * @brief Executes on GPIO page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu);


#endif /* LCD_FUNCS_H */