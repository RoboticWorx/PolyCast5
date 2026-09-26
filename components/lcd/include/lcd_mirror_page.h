#ifndef LCD_MIRROR_PAGE_H
#define LCD_MIRROR_PAGE_H

#include "lcd_utils.h"
#include "lcd_wifi.h"

#ifdef POLYCAST5_EN_SCREEN_MIRROR

#include "mirror.h" // mirror_state, mirror_has_viewer, mirror_code, mirror_check
#include "mirror_proto.h" // MIRROR_BYE_*

/**
 * @brief Hand the mirror its text sink, so a remote keyboard can type on the device
 *
 *        Called once from the LCD init, before any session can start.
 */
void lcd_mirror_page_init(void);

/**
 * @brief Tell the mirror whether the page now showing is a credential screen
 *
 *        Called from the LCD task before each page dispatch, and the only place redaction
 *        is lowered. Streaming a PIN or a saved login through the relay would be a real
 *        security regression, so those pages capture a flat placeholder instead of their
 *        pixels. Lowering invalidates the whole screen so the real pixels are recaptured.
 */
void lcd_mirror_page_sync(int page);

/**
 * @brief Raise redaction if the page now showing is a credential screen; never lowers it
 *
 *        Called right before the LCD task's own lv_timer_handler(), so a credential page
 *        entered during this pass is blanked before its first flush. Lowering here would
 *        leak the page being left, which LVGL has not repainted yet.
 */
void lcd_mirror_page_raise(int page);

/**
 * @brief Pairing page: shows the code and link state, approves or denies an attached
 *        viewer, and starts or stops the session
 *
 *        Mirroring is global state rather than a page, so leaving this screen keeps the
 *        session running and the stream follows the user wherever they navigate.
 */
void lcd_wifi_screen_mirror_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, wifi_menu_t *wifi_menu);

/**
 * @brief Delete the pairing page's labels
 */
void lcd_mirror_page_teardown(void);

#endif // POLYCAST5_EN_SCREEN_MIRROR

#endif // LCD_MIRROR_PAGE_H
