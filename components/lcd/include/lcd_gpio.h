#ifndef LCD_GPIO_H
#define LCD_GPIO_H

#include "misc/lv_style.h"
#include "misc/lv_types.h"

#define MAX_GPIO_OPTIONS 4

// Forward-declare structs (from lcd_utils.h)
typedef struct ui_btns_t ui_btns_t;
typedef struct ui_menu_t ui_menu_t;

typedef struct {
    char *options[MAX_GPIO_OPTIONS];
    lv_obj_t *btns[MAX_GPIO_OPTIONS];
    int size;
    int index;
    lv_obj_t *main_list;
    lv_style_t btn_style;
    lv_style_t sel_style;
    lv_obj_t *cont;
} gpio_menu_t;

extern gpio_menu_t gpio_menu;

/**
 * @brief Pre-load GPIO page for quick access
 *
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_setup_page(gpio_menu_t *gpio_menu);

/**
 * @brief Update GPIO page based on user input
 *
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_update_menu(gpio_menu_t *gpio_menu);

/**
 * @brief Executes page to explain how connectible hardware works
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_how_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu);

/**
 * @brief Live magcelerometer (LIS2DH12 + MMC5603) tilt readout page
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_magcel_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu);

/**
 * @brief Streams live magcelerometer readings to the selected ESP-NOW peer
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_magcel_stream_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu);

/**
 * @brief Compass calibration page (entered with DOWN from the magcel page). SELECT starts/stops a
 *        manual hard-/soft-iron calibration turn; the result is saved to NVS.
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_magcal_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu);

/**
 * @brief I2C terminal page to communicate with external hardware
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_terminal_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu);

/**
 * @brief I2C scanner page to show connected I2C device addresses
 *
 * @param [in] ui_btns UI input structure
 * @param [in] ui_menu UI menu structure
 * @param [in] gpio_menu GPIO menu structure
 */
void lcd_gpio_scanner_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu);


#endif // LCD_GPIO_H