#ifndef LCD_FUNCS_H
#define LCD_FUNCS_H

#include "lvgl.h"
#include "st7789.h"

#define SPI_MISO_PIN 2 // MISO for SX126x
#define SPI_MOSI_PIN 7 // SPI2 MOSI
#define SPI_SCLK_PIN 6 // SPI2 SCLK
#define ST7789_CS_PIN 11 // CS
#define ST7789_DC_PIN 25 // D/C
#define ST7789_RST_PIN 26 // RESET
#define ST7789_LEDK_PIN 10 // Backlight

#define HOR_RES 240
#define VER_RES 135

/** Initialise SPI bus + ST7789 panel (blocking). */
void lcd_init_driver(void);

/** Initialise LVGL draw buffers, tick timer and register flush cb. */
void lcd_lvgl_init(void);

/** Expose the internal lv_display_t* for UI creation. */
lv_display_t *lcd_get_display(void);

/** Format label for display. */
void lcd_format_label(lv_obj_t *label, const char *text, lv_color_t  color, const lv_font_t *font, lv_align_t  alignment, lv_coord_t  x_offset, lv_coord_t  y_offset);

/**
 * Rotate the three labels up:
 *  • center → top
 *  • bottom → center
 *  • new_text → bottom
 */
void lcd_scroll_up(lv_obj_t *lbl_top, lv_obj_t *lbl_mid, lv_obj_t *lbl_bot, const char *new_bot_text);

void lcd_scroll_down(lv_obj_t *lbl_top, lv_obj_t *lbl_mid, lv_obj_t *lbl_bot, const char *new_top_text);

void lcd_scroll_anim(lv_obj_t *top, lv_obj_t *mid, lv_obj_t *bot,
							  const char *txt, bool up_direction,
							  uint32_t speed_px_s);


#ifdef __cplusplus
}
#endif

#endif /* LCD_FUNCS_H */