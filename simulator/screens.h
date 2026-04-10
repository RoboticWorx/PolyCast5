/**
 * PolyCast5 Simulator - Screen rendering functions
 *
 * Each function renders a specific screen from the firmware UI.
 * Copy/adapt the LVGL widget creation code from the firmware's lcd_*.c files
 * into functions here. Strip out ESP-IDF calls (NVS, FreeRTOS, etc.) and keep
 * only the pure LVGL rendering.
 */

#ifndef SCREENS_H
#define SCREENS_H

#include "lvgl.h"

/* Display dimensions (matches ST7789 on device) */
#define HOR_RES 240
#define VER_RES 135

/* Device default colors */
#define USER_PRIMARY_COLOR   lv_color_make(0x00, 0x00, 0x8B)  /* Dark blue */
#define USER_SECONDARY_COLOR lv_color_make(0xFF, 0xFF, 0xFF)  /* White */

/**
 * Render the main selection/home screen.
 * Shows the scrolling menu with LoRa in center, arrows, battery icon, etc.
 */
void screen_selection(void);

/**
 * Render the LoRa subpage (example of a category page).
 */
void screen_lora(void);

/**
 * Render the settings page.
 */
void screen_settings(void);

/**
 * Render the infrared remote page.
 * Shows the rotated horizontal list with remote name, signals, Edit, Add New.
 */
void screen_infrared(void);

/**
 * Render the infrared "add signal" page.
 * Shows instruction text and the IR lens image while waiting for a signal.
 */
void screen_infrared_add_signal(void);

/**
 * Render the Bluetooth page.
 * Shows the vertical menu with Pair Device, Auto Keyboard, AI Keyboard options.
 */
void screen_bluetooth(void);

#endif /* SCREENS_H */
