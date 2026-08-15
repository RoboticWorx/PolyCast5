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
 * Alternate selection/home screen — same layout but with a black
 * background and 0x008B00 green accent/text color (hacker palette).
 */
void screen_selection_green(void);

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

/**
 * Render the Bluetooth AI Keyboard page (ready / "Hold & talk!" state).
 * Shows the centered "Hold & talk!" prompt, "Use: non-reasoning" footer,
 * settings-gear affordance, and persistent Wi-Fi + BT connected icons
 * stacked in the top-left.
 */
void screen_ai_keyboard(void);

/**
 * Render the GPIO submenu (How It Works / Terminal / PolyCast5-Claw / I2C Scanner).
 */
void screen_gpio(void);

/**
 * Render the I2C Terminal page with a single successful send/receive
 * round-trip already shown in the scrollable log.
 */
void screen_gpio_terminal(void);

/**
 * Render the OTA update page.  Shows the "Updating..." title, progress bar,
 * percent label, and recovery instructions.  On entry the bar animates
 * from 0% to 100%, then switches to the success state.
 */
void screen_ota_update(void);

/**
 * Render the Tetris page with a self-playing example game.  A timer drives
 * a simple best-landing-x AI so pieces move, rotate, fall, clear lines,
 * score points, and eventually trigger game-over / restart.
 */
void screen_tetris(void);

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

/** Register per-page Up/Down action callbacks. When installed, these
 *  override menu navigation and scrolling for that direction.
 *  Cleared on menu_reset. Either pointer may be NULL. */
void screen_set_nav_handlers(void (*on_up)(void), void (*on_down)(void));

/** Register a cleanup hook run just before the next screen renders
 *  (from screen_menu_reset). Used to delete timers / animations that
 *  outlive their screen widgets. Cleared after invocation. */
void screen_set_cleanup(void (*on_cleanup)(void));

#endif /* SCREENS_H */
