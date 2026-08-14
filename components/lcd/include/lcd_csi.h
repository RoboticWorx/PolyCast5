#ifndef LCD_CSI_H
#define LCD_CSI_H

#include "lcd_utils.h"

/**
 * @brief Explains what Wi-Fi motion sensing does and warns about its power cost
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_csi_intro_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

/**
 * @brief Live on-device Wi-Fi motion sensing
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] tools_menu Tools menu structure
 */
void lcd_csi_local_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu);

#endif // LCD_CSI_H
