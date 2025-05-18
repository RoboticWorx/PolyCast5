#ifndef LCD_FUNCS_H
#define LCD_FUNCS_H

#include "lvgl.h"
#include "lcd_infrared_funcs.h"

#define SPI_MISO_PIN 2 // MISO for SX126x
#define SPI_MOSI_PIN 7 // SPI2 MOSI
#define SPI_SCLK_PIN 6 // SPI2 SCLK
#define ST7789_CS_PIN 11 // CS
#define ST7789_DC_PIN 25 // D/C
#define ST7789_RST_PIN 26 // RESET
#define ST7789_LEDK_PIN 10 // Backlight

#define HOR_RES 240
#define VER_RES 135

#define HOME_PAGE 0
#define SELECTION_PAGE 1
#define LORA_PAGE 2
#define ESPNOW_PAGE 3
#define INFRARED_PAGE 4
#define SETTINGS_PAGE 5
#define TOOLS_PAGE 6
#define WIFI_PAGE 7
#define BLUETOOTH_PAGE 8
#define INFRARED_SIGNAL_PAGE 12

#define INFRARED_SIGNAL_NAME_PAGE 9
#define INFRARED_SIGNAL_EDIT_PAGE 13
#define INFRARED_REMOTE_NAME_PAGE 10
#define INFRARED_REMOTE_EDIT_PAGE 11


typedef struct ui_menu_t {
    const char **options; // your array of strings
    int size; // how many entries
    int index; // the one that’s currently in the middle
    int page;
    lv_obj_t *lbl_top; // the three labels on screen
    lv_obj_t *lbl_mid;
    lv_obj_t *lbl_bot;
    lv_obj_t *arrow_bot;
    lv_obj_t *arrow_top;
    lv_obj_t *arrow_right;
    lv_obj_t *arrow_left;
} ui_menu_t;

typedef struct ui_btns_t {
    bool up_btn;
    bool down_btn;
    bool right_btn;
    bool left_btn;
    bool back_btn;
    bool home_btn;
} ui_btns_t;

extern ui_btns_t ui_btns;

/** 
 * @brief Initialise SPI bus + ST7789 panel (blocking).
 */
void lcd_init_driver(void);

/**
 * @brief Initialise LVGL draw buffers, tick timer and register flush cb.
 */
void lcd_lvgl_init(void);

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
 * @brief Determine which page was selected by the user in the wireless selection page
 *
 * @param [in] menu UI menu structure
 */
void lcd_selection_btn_pressed(ui_menu_t *menu);

/**
 * @brief Everything to be done on wireless selection page
 *
 * @param [in] menu UI menu structure
 * @param [in] menu UI input structure
 */
void lcd_selection_page_selected(ui_menu_t *menu, ui_btns_t *ui_btns);

/**
 * @brief Everything to be done on infrared page
 *
 * @param [in] ui_menu UI menu structure
 * @param [in] ir_menu Infrared menu structure
 * @param [in] menu UI input structure
 */
void lcd_infrared_page_selected(ui_menu_t *ui_menu, ir_menu_t *ir_menu, ui_btns_t *ui_btns); 



#endif /* LCD_FUNCS_H */