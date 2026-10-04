#ifndef LCD_BOOT_SPLASH_H
#define LCD_BOOT_SPLASH_H

#include "st7789.h"

/**
 * @brief Paint the boot splash into GRAM and start animating it.
 *
 * Drawn over raw SPI, so it needs neither LVGL nor LittleFS. Call from lcd_init_driver() once
 * MADCTL and the offsets are set, before the backlight comes on. The animation task runs until
 * lcd_boot_splash_stop(). Only built with POLYCAST5_EN_BOOT_SCREEN, so guard every call site.
 *
 * @param [in] dev Initialized panel. Kept for the animation, so it must outlive it
 */
void lcd_boot_splash_show(TFT_t *dev);

/**
 * @brief Stop the animation: no splash frame reaches the panel after the caller's next write
 *        under xSPIBusMutex.
 *
 * Never blocks and is idempotent, so st7789_flush_cb calls it on every flush. The task deletes
 * itself.
 */
void lcd_boot_splash_stop(void);

#endif // LCD_BOOT_SPLASH_H
