#ifndef LCD_DICE_H
#define LCD_DICE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/lv_obj.h"

/**
 * Procedural dice for the Tools > Dice Roller page: a grid of 3D dice that tumble and land on
 * the values they are given. The grid fills the arena, so one die is drawn as large as the
 * arena allows and many dice shrink to share it. Sides 1..6 show pips, anything larger numerals
 * (on dice big enough to read them; smaller ones stay plain and the page's total carries the result).
 */

/**
 * @brief Create the arena canvas and its framebuffer and build the palette from the current
 *        theme colours. Call once, when the dice page opens.
 *
 * Safe to call repeatedly; later calls are no-ops while the arena is already initialized.
 *
 * @param [in] parent LVGL parent (the active screen) to create the canvas on
 * @param [in] x, y Top-left corner of the arena on the parent
 * @param [in] w, h Arena size in pixels
 * @return true on success, false if an allocation failed - the page then runs without dice
 */
bool lcd_dice_init(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * @brief Lay out count dice of the given sides and paint them at rest, each showing its highest
 *        face (the side count). Call whenever either value changes. No-op if uninitialized.
 */
void lcd_dice_reset(uint8_t count, uint8_t sides);

/**
 * @brief Start a roll. Each die tumbles from the face it shows now and lands on its value.
 *
 * @param [in] values count entries (as last passed to lcd_dice_reset), each 1..sides
 * @param [in] seed Varies the tumble only; the outcome is fixed by values
 * @param [in] now_ms Current time; lcd_dice_roll_step must be called on the same clock
 */
void lcd_dice_roll_start(const uint8_t *values, uint32_t seed, uint32_t now_ms);

/**
 * @brief Draw the roll as it stands at now_ms and invalidate the cells that changed.
 * @return true while any die is still moving; false once every die has landed
 */
bool lcd_dice_roll_step(uint32_t now_ms);

/**
 * @brief Delete the canvas and free its memory. Call from every page-exit path.
 *        Safe to call when uninitialized.
 */
void lcd_dice_deinit(void);

/** @brief True if init() succeeded and the arena is drawable. */
bool lcd_dice_is_available(void);

#endif // LCD_DICE_H
