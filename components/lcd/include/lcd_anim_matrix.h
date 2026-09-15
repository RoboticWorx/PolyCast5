#ifndef LCD_ANIM_MATRIX_H
#define LCD_ANIM_MATRIX_H

#include <stdbool.h>

#include "core/lv_obj.h"

#include "polycast5_macros.h"

#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM

/**
 * @brief Procedural "matrix rain" homescreen animation.
 *
 * Replaces the 42-frame LittleFS flipbook that used to back this animation, at
 * twice its frame rate and a few KB of code instead of 2.72 MB of pixels. The
 * screen is a 40 x 15 grid of 6 x 9 px glyph tiles; each column runs two falling
 * streams, and a cell's brightness is derived every frame from its distance
 * behind the nearest head rather than stored and decayed, which is what gives
 * sub-cell head motion on a coarse grid. Geometry, fall speed and colour are
 * fitted to the frames that were deleted, with the ramp lifted for the panel the
 * browser capture was much dimmer than. Glyph art lives in lcd_anim_matrix_glyphs.h.
 *
 * The module owns its LVGL canvas, PSRAM framebuffer, glyph/falloff/palette
 * tables and the sim/render lv_timer. lcd_anim.c drives it through the calls
 * below, mirroring lcd_anim_fluid.c's lifecycle. All per-frame math is Q8/Q16
 * fixed point: the ESP32-C5 has no FPU and the build is -Og.
 */

/**
 * @brief Create the canvas + framebuffer, build the LUTs, seed the rain and
 *        create the running sim/render timer. Call once, at homescreen init.
 *
 * @param [in] parent LVGL parent (the active screen) to create the canvas on
 * @return true on success, false if a required allocation failed
 */
bool lcd_anim_matrix_init(lv_obj_t *parent);

/** @brief Show the matrix canvas and resume the sim/render timer. */
void lcd_anim_matrix_start(void);

/** @brief Pause the sim/render timer (visibility unchanged). No-op if uninitialized. */
void lcd_anim_matrix_pause(void);

/** @brief Hide the matrix canvas and pause the sim/render timer. No-op if uninitialized. */
void lcd_anim_matrix_stop(void);

#endif // POLYCAST5_EN_MATRIX_RAIN_ANIM
#endif // LCD_ANIM_MATRIX_H
