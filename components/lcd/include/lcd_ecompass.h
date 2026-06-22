#ifndef LCD_ECOMPASS_H
#define LCD_ECOMPASS_H

#include "lcd_utils.h"

/**
 * @brief Live ecompass (LIS2DH12 + MMC5603) tilt readout page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 */
void lcd_ecompass_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu);

/**
 * @brief Streams live ecompass readings to the selected ESP-NOW peer
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 */
void lcd_ecompass_stream_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu);

/**
 * @brief Compass calibration page (entered with DOWN from the ecompass page). SELECT starts/stops a
 *        manual hard-/soft-iron calibration turn; the result is saved to NVS.
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] espnow_menu ESP-NOW menu structure
 */
void lcd_ecompass_calibration_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu);

#endif // LCD_ECOMPASS_H