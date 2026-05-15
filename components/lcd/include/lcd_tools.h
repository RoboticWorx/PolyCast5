#ifndef LCD_TOOLS_H
#define LCD_TOOLS_H

#include "misc/lv_style.h"
#include "misc/lv_types.h"

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

#define MAX_TOOLS_OPTIONS 20

typedef struct {
    char *options[MAX_TOOLS_OPTIONS];
    lv_obj_t *btns[MAX_TOOLS_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
    lv_style_t btn_style;
    lv_style_t sel_style;
    lv_obj_t *cont;
} tools_menu_t;

extern tools_menu_t tools_menu;

/**
 * @brief Pre-load tools page for quick access
 *
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_setup_page(tools_menu_t *tools_menu);

/**
 * @brief Update tools page based on user input
 *
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_update_menu(tools_menu_t *tools_menu);

/**
 * @brief Runs tools coin flipper page
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_coin_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Runs tools 'read the docs' page
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_docs_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Runs tools dice roller page
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_dice_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Runs random number generator
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_num_gen_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Tetris game implemetation
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_tetris_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Creates QR from public bitcoin address
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_btc_addr_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Explains how the public bitcoin address page works and setup
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_btc_addr_setup_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Show pomodoro work rest timer
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_pomodoro_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Explains how the SRS memory planner works
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_how_srs_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Runs LTP SRS planner memory assist page
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_srs_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Shows live Claude AI subscription usage fetched from a LAN companion host
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_claude_usage_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Web-portal setup page for the Claude companion host (IP + port entry)
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_tools_claude_setup_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);


#endif // LCD_TOOLS_H