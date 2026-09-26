#ifndef LCD_TEXT_INPUT_H
#define LCD_TEXT_INPUT_H

#include <stddef.h>
#include <stdbool.h>

#include "lcd_utils.h" // ui_btns_t, colors, ACTIVE_SCR, screen size helpers

typedef enum {
    LCD_TI_PENDING = 0, /**< Still editing */
    LCD_TI_SUBMITTED,   /**< OK pressed; `buf` holds the entered text */
    LCD_TI_CANCELLED,   /**< BACK pressed; caller should return to its menu */
    LCD_TI_POWER_OFF,   /**< POWER pressed; caller should sleep/transition out */
} lcd_ti_status_t;

typedef struct {
    /* ---- Caller configuration (set before lcd_text_input_start) ---- */
    char       *buf;              /**< Result buffer, caller-owned */
    size_t      buf_size;         /**< sizeof(buf); max chars = buf_size - 1 */
    const char *title;            /**< Instruction line shown at the top */
    const char *hint;             /**< Small hint under the title, NULL to hide */
    const char *prefill;          /**< Initial text (rename flows), NULL/"" = blank */
    bool        lock_until_submit;/**< If true, BACK and POWER are disabled (only OK exits) */
    bool        allow_space_only; /**< false (names): value must have a non-space char; true
                                       (passwords): only emptiness is rejected. Default false. */
    bool        sensitive;        /**< Wi-Fi passwords and API keys: the Screen Mirror blanks
                                       the preview and refuses remote typing. Default false. */
    lv_obj_t   *arrow_top;        /**< Optional global nav arrows, hidden while the keyboard is up */
    lv_obj_t   *arrow_bot;
    lv_obj_t   *arrow_left;
    lv_obj_t   *arrow_right;

    /* ---- Internal state (do not touch) ---- */
    bool        arrow_was_hidden[4]; /**< Prior visibility of {top,bot,left,right}, restored on close */
    lv_obj_t   *lbl_title;
    lv_obj_t   *lbl_hint;
    lv_obj_t   *lbl_text;         /**< Typed-text preview (uses the tail-window helper) */
    lv_obj_t   *kb;               /**< Grid keyboard (button matrix) */
    int         len;              /**< Current text length in `buf` */
    int         mode;            /**< Active character set: 0 = ABC, 1 = abc, 2 = 12# */
    bool        active;           /**< True between start and a terminal tick */
} lcd_text_input_t;

/**
 * @brief Build the entry UI from the configured fields and seed it from `prefill`.
 *        Sets `active` = true. Safe to call only when `active` is false.
 */
void lcd_text_input_start(lcd_text_input_t *ti);

/**
 * @brief Process one LCD tick of button input.
 * @return the current status; on a terminal status the UI is already torn down.
 */
lcd_ti_status_t lcd_text_input_tick(lcd_text_input_t *ti, ui_btns_t *btns);

/**
 * @brief Tear down the entry UI and reset state. Called automatically on a
 *        terminal tick; exposed so a caller can force-close if it must abandon
 *        the entry for its own reasons.
 */
void lcd_text_input_close(lcd_text_input_t *ti);

typedef enum {
    LCD_TI_REMOTE_BACKSPACE = 0,
    LCD_TI_REMOTE_SUBMIT,
    LCD_TI_REMOTE_CANCEL,
} lcd_ti_remote_key_t;

/**
 * @brief Queue printable ASCII typed on a remote (Screen Mirror) keyboard
 *
 *        Dropped when no entry screen is open or the open one is `sensitive`. Applied on
 *        the LCD task by the next lcd_text_input_tick(), so buffer bounds and the
 *        lock_until_submit guard still hold and no LVGL call leaves that task.
 *
 * @return Characters accepted, which may be fewer than `len` if the queue filled
 */
size_t lcd_text_input_remote_type(const char *ascii, size_t len);

/**
 * @brief Queue a backspace, submit or cancel from a remote keyboard
 */
void lcd_text_input_remote_key(lcd_ti_remote_key_t key);

/**
 * @brief Whether an entry screen is open, so the mirror can raise the web keyboard
 *
 * @param [out] out_sensitive Set when the open screen refuses remote input; may be NULL
 * @param [out] out_buf       Copy of the text so far, blanked when sensitive; may be NULL
 * @param [in]  out_size      sizeof(out_buf)
 */
bool lcd_text_input_remote_state(bool *out_sensitive, char *out_buf, size_t out_size);

#endif /* LCD_TEXT_INPUT_H */
