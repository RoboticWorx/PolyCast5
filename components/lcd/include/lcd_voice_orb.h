#ifndef LCD_VOICE_ORB_H
#define LCD_VOICE_ORB_H

#include <stdbool.h>

#include "core/lv_obj.h"

/**
 * @brief Create the canvas + framebuffer, build the LUTs from the current theme colours and
 *        create the render timer (paused). Call once, when the AI keyboard page opens.
 *
 * Safe to call repeatedly; later calls are no-ops while the orb is already initialized.
 *
 * @param [in] parent LVGL parent (the active screen) to create the canvas on
 * @return true on success, false if a required allocation failed - the caller should then
 *         fall back to a plain widget (see lcd_voice_orb_is_available())
 */
bool lcd_voice_orb_init(lv_obj_t *parent);

/** @brief Show the orb and resume the render timer. No-op if uninitialized. */
void lcd_voice_orb_start(void);

/** @brief Hide the orb and pause the render timer. No-op if uninitialized. */
void lcd_voice_orb_stop(void);

/**
 * @brief Delete the timer and canvas and free the framebuffer. Call from every page-exit path.
 *
 * Unlike the homescreen animations this module is page-scoped, so its PSRAM must be returned
 * when the page closes. Safe to call when uninitialized.
 */
void lcd_voice_orb_deinit(void);

/** @brief True if init() succeeded and the orb is drawable. */
bool lcd_voice_orb_is_available(void);

#endif // LCD_VOICE_ORB_H
