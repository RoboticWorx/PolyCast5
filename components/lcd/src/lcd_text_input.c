#include <string.h>

#include "lvgl.h" // Button matrix, label, LV_KEY_*, events, fonts

#include "lcd_utils.h" // Colors, ACTIVE_SCR, HOR_RES, lcd_format_label, tail-window helper
#include "lcd_text_input.h"

// Special-key labels
#define K_SPACE        " "
#define K_DEL          "DEL"
#define K_EXIT         "EXIT"
#define K_OK           "OK"
#define K_MODE_LOWER   "abc" // Shown in ABC mode  -> switches to abc
#define K_MODE_SYMBOL  "12#" // Shown in abc mode  -> switches to 12#
#define K_MODE_UPPER   "ABC" // Shown in 12# mode  -> switches to ABC

// Character sets
static const char * const kb_map_upper[] = {
    "A", "B", "C", "D", "E", "F", "G", "\n",
    "H", "I", "J", "K", "L", "M", "N", "\n",
    "O", "P", "Q", "R", "S", "T", "U", "\n",
    "V", "W", "X", "Y", "Z", "_", "\n",
    K_MODE_LOWER, K_SPACE, K_DEL, K_EXIT, K_OK, ""
};

static const char * const kb_map_lower[] = {
    "a", "b", "c", "d", "e", "f", "g", "\n",
    "h", "i", "j", "k", "l", "m", "n", "\n",
    "o", "p", "q", "r", "s", "t", "u", "\n",
    "v", "w", "x", "y", "z", "_", "\n",
    K_MODE_SYMBOL, K_SPACE, K_DEL, K_EXIT, K_OK, ""
};

static const char * const kb_map_sym[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "\n",
    "_", "=", "+", "[", "]", "{", "}", ";", ":", "'", "\"", "\n",
    ",", ".", "<", ">", "/", "?", "\\", "|", "`", "~", "!", "\n",
    "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    K_MODE_UPPER, K_SPACE, K_DEL, K_EXIT, K_OK, ""
};

// Locked variants (adding-new flows): identical to the maps above but with no
// EXIT key, so the action row is [mode][space][DEL][OK] and there is no reserved
// gap where a hidden EXIT would otherwise sit.
static const char * const kb_map_upper_locked[] = {
    "A", "B", "C", "D", "E", "F", "G", "\n",
    "H", "I", "J", "K", "L", "M", "N", "\n",
    "O", "P", "Q", "R", "S", "T", "U", "\n",
    "V", "W", "X", "Y", "Z", "_", "\n",
    K_MODE_LOWER, K_SPACE, K_DEL, K_OK, ""
};

static const char * const kb_map_lower_locked[] = {
    "a", "b", "c", "d", "e", "f", "g", "\n",
    "h", "i", "j", "k", "l", "m", "n", "\n",
    "o", "p", "q", "r", "s", "t", "u", "\n",
    "v", "w", "x", "y", "z", "_", "\n",
    K_MODE_SYMBOL, K_SPACE, K_DEL, K_OK, ""
};

static const char * const kb_map_sym_locked[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "\n",
    "_", "=", "+", "[", "]", "{", "}", ";", ":", "'", "\"", "\n",
    ",", ".", "<", ">", "/", "?", "\\", "|", "`", "~", "!", "\n",
    "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    K_MODE_UPPER, K_SPACE, K_DEL, K_OK, ""
};

// True if the activated key label is one of the character-set toggles
static bool key_is_mode(const char *t)
{
    return strcmp(t, K_MODE_LOWER) == 0 ||
           strcmp(t, K_MODE_SYMBOL) == 0 ||
           strcmp(t, K_MODE_UPPER) == 0;
}

// The character-set map for the current mode. Locked entries (adding-new) use
// the EXIT-less variants so there is no reserved hole in the action row.
static const char * const *kb_map_for_mode(const lcd_text_input_t *ti)
{
    if (ti->lock_until_submit) {
        switch (ti->mode) {
            case 1:  return kb_map_lower_locked;
            case 2:  return kb_map_sym_locked;
            default: return kb_map_upper_locked;
        }
    }
    switch (ti->mode) {
        case 1:  return kb_map_lower;
        case 2:  return kb_map_sym;
        default: return kb_map_upper;
    }
}

// Locate the (row, col) of button `idx` in a map ("\n" ends a row, "" ends the map)
// Falls back to the last position if idx is past the end
static void kb_index_to_rc(const char * const *map, uint32_t idx, int *row, int *col)
{
    int r = 0, c = 0;
    uint32_t b = 0;
    for (uint32_t i = 0; map[i][0] != '\0'; i++) {
        if (map[i][0] == '\n') {
            r++;
            c = 0;
            continue;
        }
        if (b == idx) {
            *row = r;
            *col = c;
            return;
        }
        b++;
        c++;
    }
    *row = r;
    *col = c;
}

// Button index at (row, col) in a map, clamping row into the map and col into
// that row. This lets the cursor keep its visual spot across a layout swap - the
// action row stays the action row even though the letter/symbol rows differ in
// width, so the mode key doesn't jump onto a random character.
static uint32_t kb_rc_to_index(const char * const *map, int row, int col)
{
    uint32_t row_start[8];
    int      row_len[8];
    int      nrows = 0;
    uint32_t b = 0;      // Running button index
    int      c = 0;      // Buttons seen in the current row
    uint32_t start = 0;  // First button index of the current row

    for (uint32_t i = 0;; i++) {
        char first = map[i][0];
        if (first == '\n' || first == '\0') {
            if (c > 0 && nrows < 8) {
                row_start[nrows] = start;
                row_len[nrows] = c;
                nrows++;
            }
            start = b;
            c = 0;
            if (first == '\0') {
                break;
            }
        } else {
            b++;
            c++;
        }
    }

    if (nrows == 0) {
        return 0;
    }
    if (row < 0) row = 0;
    if (row >= nrows) row = nrows - 1;
    if (col < 0) col = 0;
    if (col >= row_len[row]) col = row_len[row] - 1;
    return row_start[row] + (uint32_t)col;
}

// Rebuild the matrix for the current mode/lock family and highlight button `sel`
static void kb_build(lcd_text_input_t *ti, uint32_t sel)
{
    lv_buttonmatrix_set_map(ti->kb, kb_map_for_mode(ti));
    lv_buttonmatrix_set_selected_button(ti->kb, sel);
}

// Advance to the next character set, keeping the cursor on the same visual
// (row, col) so switching into 12# doesn't jump the highlight to a random key.
static void kb_cycle_mode(lcd_text_input_t *ti)
{
    uint32_t sel = lv_buttonmatrix_get_selected_button(ti->kb);
    int row = 0, col = 0;
    if (sel != LV_BUTTONMATRIX_BUTTON_NONE) {
        kb_index_to_rc(kb_map_for_mode(ti), sel, &row, &col);
    }

    ti->mode = (ti->mode + 1) % 3;
    kb_build(ti, kb_rc_to_index(kb_map_for_mode(ti), row, col));
}

// True if the string contains at least one non-space character
// Space is the only whitespace the keyboard can produce, so this rejects all-space names
static bool has_visible_char(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s != ' ') {
            return true;
        }
    }
    return false;
}

// Refresh the typed-text preview, keeping the tail on screen when it overflows
static void update_preview(lcd_text_input_t *ti)
{
    lcd_set_input_label_text(ti->lbl_text, ti->buf);
    lv_obj_align(ti->lbl_text, LV_ALIGN_TOP_MID, 0, 40);
}

static void insert_char(lcd_text_input_t *ti, char c)
{
    if (ti->len < (int)ti->buf_size - 1) {
        ti->buf[ti->len++] = c;
        ti->buf[ti->len] = '\0';
        update_preview(ti);
    }
}

static void backspace(lcd_text_input_t *ti)
{
    if (ti->len > 0) {
        ti->buf[--ti->len] = '\0';
        update_preview(ti);
    }
}

// Move the keyboard cursor by reusing the button matrix's own key navigation,
// which already handles ragged rows and hidden keys.
static void kb_nav(lcd_text_input_t *ti, uint32_t key)
{
    lv_obj_send_event(ti->kb, LV_EVENT_KEY, &key);
}

/* ---- Public API ---- */

void lcd_text_input_start(lcd_text_input_t *ti)
{
    // Seed the buffer from the prefill (rename flows) or start blank.
    ti->len = 0;
    ti->buf[0] = '\0';
    if (ti->prefill && ti->prefill[0]) {
        size_t max = ti->buf_size - 1;
        strncpy(ti->buf, ti->prefill, max);
        ti->buf[max] = '\0';
        ti->len = (int)strlen(ti->buf);
    }
    ti->mode = 0; // Always open on the uppercase set

    // Title
    ti->lbl_title = lv_label_create(ACTIVE_SCR);
    lcd_format_label(ti->lbl_title, ti->title ? ti->title : "", user_secondary_color,
                     &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 2);

    // Optional hint
    ti->lbl_hint = NULL;
    if (ti->hint && ti->hint[0]) {
        ti->lbl_hint = lv_label_create(ACTIVE_SCR);
        lcd_format_label(ti->lbl_hint, ti->hint, user_secondary_color,
                         &lv_font_montserrat_12, LV_ALIGN_TOP_MID, 0, 22);
    }

    // Typed-text preview
    ti->lbl_text = lv_label_create(ACTIVE_SCR);
    lcd_format_label(ti->lbl_text, "", user_secondary_color,
                     &lv_font_montserrat_24, LV_ALIGN_TOP_MID, 0, 40);

    // Grid keyboard (button matrix)
    ti->kb = lv_buttonmatrix_create(ACTIVE_SCR);
    lv_obj_set_size(ti->kb, HOR_RES - 4, 66);
    lv_obj_align(ti->kb, LV_ALIGN_BOTTOM_MID, 0, -1);

    // Container: no chrome, tight key gaps
    lv_obj_set_style_bg_opa(ti->kb, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ti->kb, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ti->kb, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(ti->kb, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ti->kb, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(ti->kb, 2, LV_PART_MAIN);

    // Keys: normal
    lv_obj_set_style_text_font(ti->kb, &lv_font_montserrat_12, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(ti->kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(ti->kb, lv_color_darken(user_primary_color, 20), LV_PART_ITEMS);
    lv_obj_set_style_text_color(ti->kb, user_secondary_color, LV_PART_ITEMS);
    lv_obj_set_style_radius(ti->kb, 3, LV_PART_ITEMS);
    lv_obj_set_style_border_width(ti->kb, 0, LV_PART_ITEMS);

    // Keys: the highlighted (selected) one. draw_main only applies FOCUSED to the
    // selected button when the widget itself holds FOCUSED, so pin that state on.
    lv_obj_set_style_bg_color(ti->kb, user_secondary_color, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(ti->kb, user_primary_color, LV_PART_ITEMS | LV_STATE_FOCUSED);
    lv_obj_add_state(ti->kb, LV_STATE_FOCUSED);

    kb_build(ti, 0);
    update_preview(ti);

    // The keyboard covers the global nav arrows, so hide them while it's up and
    // remember their prior state to restore on close.
    lv_obj_t *arrows[4] = { ti->arrow_top, ti->arrow_bot, ti->arrow_left, ti->arrow_right };
    for (int i = 0; i < 4; i++) {
        if (arrows[i]) {
            ti->arrow_was_hidden[i] = lv_obj_has_flag(arrows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(arrows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    ti->active = true;
}

lcd_ti_status_t lcd_text_input_tick(lcd_text_input_t *ti, ui_btns_t *btns)
{
    if (!ti->active) {
        return LCD_TI_PENDING;
    }

    if (btns->up_btn) {
        kb_nav(ti, LV_KEY_UP);
    } else if (btns->down_btn) {
        kb_nav(ti, LV_KEY_DOWN);
    } else if (btns->left_btn) {
        kb_nav(ti, LV_KEY_LEFT);
    } else if (btns->right_btn) {
        kb_nav(ti, LV_KEY_RIGHT);
    } else if (btns->home_btn) {
        kb_cycle_mode(ti); // Shortcut for the on-grid mode key
    } else if (btns->select_btn) {
        uint32_t id = lv_buttonmatrix_get_selected_button(ti->kb);
        const char *t = (id == LV_BUTTONMATRIX_BUTTON_NONE)
                            ? NULL
                            : lv_buttonmatrix_get_button_text(ti->kb, id);
        if (t) {
            if (strcmp(t, K_OK) == 0) {
                // Refuse to submit a value with no visible character
                if (ti->len > 0 && (ti->allow_space_only || has_visible_char(ti->buf))) {
                    lcd_text_input_close(ti);
                    return LCD_TI_SUBMITTED;
                }
            } else if (strcmp(t, K_EXIT) == 0) {
                // EXIT is absent from the locked maps, but guard anyway
                if (!ti->lock_until_submit) {
                    lcd_text_input_close(ti);
                    return LCD_TI_CANCELLED;
                }
            } else if (strcmp(t, K_DEL) == 0) {
                backspace(ti);
            } else if (key_is_mode(t)) {
                kb_cycle_mode(ti);
            } else {
                // Space (" ") or a normal single character
                insert_char(ti, t[0]);
            }
        }
    } else if (btns->pwr_btn) {
        if (!ti->lock_until_submit) {
            lcd_text_input_close(ti);
            return LCD_TI_POWER_OFF;
        }
    }

    return LCD_TI_PENDING;
}

void lcd_text_input_close(lcd_text_input_t *ti)
{
    if (ti->lbl_title) {
        lv_obj_delete(ti->lbl_title);
    }
    if (ti->lbl_hint) {
        lv_obj_delete(ti->lbl_hint);
    }
    if (ti->lbl_text) {
        lv_obj_delete(ti->lbl_text);
    }
    if (ti->kb) {
        lv_obj_delete(ti->kb);
    }

    // Restore the nav arrows to whatever they were before the keyboard opened
    // Callers' own exit-path arrow tweaks run after this and take precedence
    lv_obj_t *arrows[4] = { ti->arrow_top, ti->arrow_bot, ti->arrow_left, ti->arrow_right };
    for (int i = 0; i < 4; i++) {
        if (arrows[i] && !ti->arrow_was_hidden[i]) {
            lv_obj_remove_flag(arrows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    ti->lbl_title = ti->lbl_hint = ti->lbl_text = ti->kb = NULL;
    ti->len = 0;
    ti->mode = 0;
    ti->active = false;
}
