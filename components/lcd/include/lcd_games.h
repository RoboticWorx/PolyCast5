#ifndef LCD_GAMES_H
#define LCD_GAMES_H

#include "lcd_tools.h" // tools_menu_t + ui_btns_t/ui_menu_t forward declarations

/**
 * @brief Tetris game implemetation
 *
 * @param [in] ui_btns User input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_games_tetris_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

#endif // LCD_GAMES_H
