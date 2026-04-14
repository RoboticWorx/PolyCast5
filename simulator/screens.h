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

/**
 * Render the Tools page.
 * Shows the vertical menu with Coin Flipper, Dice Roller, Tetris, etc.
 */
void screen_tools(void);

/**
 * Render the Wi-Fi beacon scan results page.
 * Shows a bar chart of 20 RSSI samples with color-graded bars, plus SSID/
 * channel/security info below.
 */
void screen_wifi_beacon(void);

/**
 * Render the Wi-Fi data frame scan page.
 * Shows a bar chart of per-client packet counts (sorted desc, color-graded
 * red→green by count) and a scrollable list of identified client MACs.
 */
void screen_wifi_data(void);

/* ─── Menu navigation (Up/Down arrows) ───────────────────────── */

#define MENU_MAX_BTNS 16

/** Reset active menu state — call before each screen render. */
void screen_menu_reset(void);

/** Navigate the active list up (dir=-1) or down (dir=+1).
 *  Matches the firmware's lcd_*_update_menu: restyle all buttons,
 *  highlight selected, lv_obj_scroll_to_view with LV_ANIM_ON.
 *  If no menu is active but a scroll container is registered (see
 *  screen_set_scroll), scrolls that container instead. */
void screen_menu_navigate(int direction);

/** Register a scrollable container for chart pages that have no menu.
 *  Up/Down keys will scroll it by step_px pixels (DOWN reveals content
 *  below, matching firmware button semantics). Cleared on menu_reset. */
void screen_set_scroll(lv_obj_t *cont, int step_px);

#endif /* SCREENS_H */
