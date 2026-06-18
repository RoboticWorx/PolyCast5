#ifndef LCD_GAMES_H
#define LCD_GAMES_H

#include "misc/lv_style.h"
#include "misc/lv_types.h"

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

#define MAX_GAMES_OPTIONS 8

typedef struct {
    char *options[MAX_GAMES_OPTIONS];
    lv_obj_t *btns[MAX_GAMES_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
    lv_style_t btn_style;
    lv_style_t sel_style;
    lv_obj_t *cont;
} games_menu_t;

extern games_menu_t games_menu;

/**
 * @brief Pre-load the Games menu list for quick access
 *
 * @param [in] games_menu Games menu structure
 */
void lcd_games_setup_page(games_menu_t *games_menu);

/**
 * @brief Update the Games menu list based on user input
 *
 * @param [in] games_menu Games menu structure
 */
void lcd_games_update_menu(games_menu_t *games_menu);

/**
 * @brief Games menu page; routes selection to the individual games
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] games_menu Games menu structure
 */
void lcd_games_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu);

/**
 * @brief Tetris game implemetation
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] games_menu Games menu structure
 */
void lcd_games_tetris_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu);

/**
 * @brief T-Rex Runner game implemetation
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] games_menu Games menu structure
 */
void lcd_games_trex_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu);

/**
 * @brief Flappy Bird game implemetation
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] games_menu Games menu structure
 */
void lcd_games_flappy_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu);

/**
 * @brief DOOM raycaster mini-FPS implementation
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] games_menu Games menu structure
 */
void lcd_games_doom_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu);

#endif // LCD_GAMES_H
