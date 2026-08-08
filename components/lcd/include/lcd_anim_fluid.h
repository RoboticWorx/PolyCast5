#ifndef LCD_ANIM_FLUID_H
#define LCD_ANIM_FLUID_H

#include <stdbool.h>

#include "core/lv_obj.h"

#include "polycast5_macros.h"

#ifdef POLYCAST5_EN_WATER_ANIM

/**
 * @brief Procedural 3D particle-fluid homescreen animation.
 *
 * A port of the esp32-fluidbox look (V4C38/esp32-fluidbox):
 * a Clavet double-density-relaxation SPH fluid rendered with a pinhole-camera
 * perspective and a speed/depth colour ramp (deep blue -> bright -> pale ->
 * white spray). Gravity + shake come from the LIS2DH12 accelerometer. Adapted
 * to the ESP32-C5 single core, the 240x135 LVGL canvas, and an accelerometer-only
 * board (the reference's gyro pseudo-forces are dropped).
 *
 * The module owns its LVGL canvas, PSRAM framebuffer, particle pools and the
 * sim/render lv_timer. lcd_anim.c drives it through the calls below, mirroring
 * the flipbook animations' lifecycle.
 */

/**
 * @brief Create the canvas + framebuffer, seed the fluid and create the running
 *        sim/render timer. Call once, at homescreen init.
 *
 * @param [in] parent LVGL parent (the active screen) to create the canvas on
 * @return true on success, false if a required allocation failed
 */
bool lcd_anim_fluid_init(lv_obj_t *parent);

/** @brief Show the fluid canvas and resume the sim/render timer. */
void lcd_anim_fluid_start(void);

/** @brief Pause the sim/render timer (visibility unchanged). No-op if uninitialized. */
void lcd_anim_fluid_pause(void);

/** @brief Hide the fluid canvas and pause the sim/render timer. No-op if uninitialized. */
void lcd_anim_fluid_stop(void);

#endif // POLYCAST5_EN_WATER_ANIM
#endif // LCD_ANIM_FLUID_H
