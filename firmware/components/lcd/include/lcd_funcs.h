#ifndef LCD_FUNCS_H
#define LCD_FUNCS_H

#include "lvgl.h"
#include "st7789.h"

#define SPI_MISO_PIN 2 // MISO for SX126x
#define SPI_MOSI_PIN      7   // SPI2 MOSI
#define SPI_SCLK_PIN      6   // SPI2 SCLK
#define ST7789_CS_PIN     11  // CS
#define ST7789_DC_PIN     25  // D/C
#define ST7789_RST_PIN    26  // RESET
#define ST7789_LEDK_PIN     10  // Backlight

/** Initialise SPI bus + ST7789 panel (blocking). */
void lcd_init_driver(void);

/** Initialise LVGL draw buffers, tick timer and register flush cb. */
void lcd_lvgl_init(void);

/** Expose the internal lv_display_t* for UI creation. */
lv_display_t *lcd_get_display(void);

#ifdef __cplusplus
}
#endif

#endif /* LCD_FUNCS_H */