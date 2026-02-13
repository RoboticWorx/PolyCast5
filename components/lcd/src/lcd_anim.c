#include "polycast5_macros.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"

#include "esp_log.h"
#include "esp_err.h"

#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"
#include "font/lv_symbol_def.h"
#include "widgets/label/lv_label.h"
#include "misc/lv_timer.h"

#include "lcd_utils.h"

static bool label_x_anim_busy = false;

static void animate_label_x_delete_cb(lv_anim_t *a)
{
    lv_obj_t *old_lbl = (lv_obj_t *)lv_anim_get_user_data(a);
    if (old_lbl && lv_obj_is_valid(old_lbl)) {
        lv_obj_delete(old_lbl);
    }
}

static void animate_label_x_done_cb(lv_anim_t *a)
{
    (void)a;
    label_x_anim_busy = false;
}

bool lcd_anim_label_x_animate_is_busy(void)
{
    return label_x_anim_busy;
}

void lcd_anim_label_x_animate_reset(void)
{
    label_x_anim_busy = false;
}

lv_obj_t *lcd_anim_animate_label_x(lv_obj_t *old_lbl, const char *new_text, const lv_font_t *cur_font, lv_coord_t start_x_left, lv_coord_t end_x_right)
{
    if (!old_lbl) {
        return NULL;
    }

    // Prevent overlapping animations if button repeats quickly
    if (label_x_anim_busy) {
        return old_lbl;
    }
    label_x_anim_busy = true;

    lv_obj_t *parent = lv_obj_get_parent(old_lbl);
    lv_coord_t old_x = lv_obj_get_x(old_lbl);
    lv_coord_t old_y = lv_obj_get_y(old_lbl);
    lv_coord_t old_w = lv_obj_get_width(old_lbl);

    // Create incoming label with same style
    lv_obj_t *new_lbl = lv_label_create(parent);
    lv_label_set_text(new_lbl, new_text);
    lv_obj_set_style_text_font(new_lbl, cur_font, 0);
    lv_obj_set_style_text_color(new_lbl, user_secondary_color, 0);
    lv_obj_update_layout(new_lbl);

    lv_coord_t new_w = lv_obj_get_width(new_lbl);

    // Keep same visual center as old label
    lv_coord_t target_x = old_x + (old_w - new_w) / 2;

    lv_obj_set_pos(new_lbl, start_x_left, old_y);

    // Old label: center -> right
    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, old_lbl);
    lv_anim_set_values(&a_out, old_x, end_x_right);
    lv_anim_set_time(&a_out, 220);
    lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a_out, animate_label_x_delete_cb);
    lv_anim_set_user_data(&a_out, old_lbl);
    lv_anim_start(&a_out);

    // New label: left -> center
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, new_lbl);
    lv_anim_set_values(&a_in, start_x_left, target_x);
    lv_anim_set_time(&a_in, 220);
    lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a_in, animate_label_x_done_cb);
    lv_anim_start(&a_in);

    return new_lbl;
}