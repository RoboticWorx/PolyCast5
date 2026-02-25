#ifndef LCD_ANIM_H
#define LCD_ANIM_H

#include "esp_err.h"

#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"

#include "polycast5_macros.h"

#include "lcd_utils.h"

#define LCD_ANIM_DEFAULT_X_EDGE_LEFT (-lv_obj_get_width(ACTIVE_SCR) - 8)
#define LCD_ANIM_DEFAULT_X_EDGE_RIGHT (lv_obj_get_width(ACTIVE_SCR) + 8)
#define LCD_ANIM_DEFAULT_X_TIME 200

#define LCD_ANIM_DEFAULT_Y_EDGE_TOP (-lv_obj_get_height(ACTIVE_SCR) - 8)
#define LCD_ANIM_DEFAULT_Y_EDGE_BOTTOM (lv_obj_get_height(ACTIVE_SCR) + 8)
#define LCD_ANIM_DEFAULT_Y_TIME 100

// Animation frame counts
#ifdef POLYCAST5_EN_CITY_ANIM
    #define CITY_FRAME_CNT 60
#else
    #define CITY_FRAME_CNT 0
#endif
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    #define BLACK_HOLE_FRAME_CNT 18
#else
    #define BLACK_HOLE_FRAME_CNT 0
#endif
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    #define MATRIX_RAIN_FRAME_CNT 42
#else
    #define MATRIX_RAIN_FRAME_CNT 0
#endif
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    #define PYRAMID_FRAME_CNT 56
#else
    #define PYRAMID_FRAME_CNT 0
#endif

/**
 * @brief Warm up all animation frames by preloading them into memory
 */
void lcd_anim_warm_all(void);

/**
 * @brief Save the currently selected animation as the default animation in NVS
 *
 * @returns ESP error status
 */
esp_err_t lcd_anim_nvs_save(void);

/**
 * @brief Loads the default animation from NVS and sets it as the active animation
 *
 * @returns ESP error status
 */
esp_err_t lcd_anim_nvs_load(void);

/**
 * @brief Initialize animation images and timers, and set visibility based on selected animation
 */
void lcd_anim_init_images(void);

/**
 * @brief Start the currently selected animation by making it visible and resuming its timer
 */
void lcd_anim_start_animation(void);

/**
 * @brief Stop all animations by hiding them and pausing their timers
 */
void lcd_anim_stop_animations(void);

/**
 * @brief Transition to the next or previous animation based on the direction parameter, and save the selection to NVS
 *
 * @param [in] dir If true, transition to the next animation; if false, transition to the previous animation
 */
void lcd_anim_transition_animation(bool dir);

/**
 * @brief Generic loading animation (single dot that moves + pulses)
 *
 * @param [in] align Alignment of the loading animation
 * @param [in] x_off X offset from alignment (positive = right)
 * @param [in] y_off Y offset from alignment (positive = down)
 * @param [in] color Color of the loading dot
 */
void lcd_anim_loading_start(lv_align_t align, lv_coord_t x_off, lv_coord_t y_off, lv_color_t color);

/**
 * @brief Stop and delete loading animation
 */
void lcd_anim_loading_stop(void);

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