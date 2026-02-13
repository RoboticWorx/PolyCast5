#ifndef LCD_ANIM_H
#define LCD_ANIM_H

#include "polycast5_macros.h"

#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"

#include "lcd_utils.h"

#define LCD_ANIM_DEFAULT_X_EDGE_LEFT (-lv_obj_get_width(ACTIVE_SCR) - 8)
#define LCD_ANIM_DEFAULT_X_EDGE_RIGHT (lv_obj_get_width(ACTIVE_SCR) + 8)
#define LCD_ANIM_DEFAULT_X_TIME 200

#define LCD_ANIM_DEFAULT_Y_EDGE_TOP (-lv_obj_get_height(ACTIVE_SCR) - 8)
#define LCD_ANIM_DEFAULT_Y_EDGE_BOTTOM (lv_obj_get_height(ACTIVE_SCR) + 8)
#define LCD_ANIM_DEFAULT_Y_TIME 100

/**
 * @brief Perform label slide animation on x-axis
 *
 * @param [in] old_lbl Label to slide out and be deleted at end of animation
 * @param [in] new_text Text for new label to be created and slid in
 * @param [in] cur_font Font for new label to be created and slid in
 * @param [in] start_x_left Starting x-coordinate for new label (relative to alignment)
 * @param [in] end_x_right Ending x-coordinate for old label (relative to alignment)
 *
 * @returns New label object that has been slid in
 */
lv_obj_t *lcd_anim_animate_label_x(lv_obj_t *old_lbl, const char *new_text, const lv_font_t *cur_font, lv_coord_t start_x_left, lv_coord_t end_x_right);

/**
 * @brief Check if lcd_animate_label_x is currently running
 *
 * @returns True if animation is running, false if not
 */
bool lcd_anim_label_x_animate_is_busy(void);

/**
 * @brief Reset the lcd_animate_label_x animation state
 */
void lcd_anim_label_x_animate_reset(void);

/**
 * @brief Perform label slide animation on y-axis
 *
 * @param [in] old_lbl Label to slide out and be deleted at end of animation
 * @param [in] new_text Text for new label to be created and slid in
 * @param [in] cur_font Font for new label to be created and slid in
 * @param [in] start_y_top Starting y-coordinate for new label (relative to alignment)
 * @param [in] end_y_bottom Ending y-coordinate for old label (relative to alignment)
 *
 * @returns New label object that has been slid in
 */
lv_obj_t *lcd_anim_animate_label_y(lv_obj_t *old_lbl, const char *new_text, const lv_font_t *cur_font, lv_coord_t start_y_top, lv_coord_t end_y_bottom);

/**
 * @brief Check if lcd_animate_label_y is currently running
 *
 * @returns True if animation is running, false if not
 */
bool lcd_anim_label_y_animate_is_busy(void);

/**
 * @brief Reset the lcd_animate_label_y animation state
 */
void lcd_anim_label_y_animate_reset(void);

#endif // LCD_ANIM_H