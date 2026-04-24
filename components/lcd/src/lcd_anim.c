#include "polycast5_macros.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/idf_additions.h"
#include "portmacro.h"

#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"

#include "draw/lv_image_decoder.h"
#include "draw/lv_image_decoder_private.h"
#include "core/lv_obj.h"
#include "core/lv_obj_pos.h"
#include "font/lv_symbol_def.h"
#include "widgets/label/lv_label.h"
#include "misc/lv_timer.h"

#include "lcd_asset_macros.h"
#include "lcd_anim.h"
#include "lcd_utils.h"

#define LCD_ANIM_NS "anim_data"
#define LCD_ANIM_KEY "selected"

/* Animation macros */
#define NUM_ANIMS 3

// Frame periods (ms)
#ifdef POLYCAST5_EN_CITY_ANIM
    #define CITY_FRAME_PERIOD 120 // 160
#endif
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    #define BLACK_HOLE_FRAME_PERIOD 120
#endif
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    #define MATRIX_RAIN_FRAME_PERIOD 100
#endif
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    #define PYRAMID_FRAME_PERIOD 120
#endif

// Number each sequentially
enum
{
#ifdef POLYCAST5_EN_CITY_ANIM
    CITY,
#endif
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    BLACK_HOLE,
#endif
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    MATRIX_RAIN,
#endif
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    PYRAMID
#endif
};

static const char *TAG = "LCD_ANIM";

static bool label_x_anim_busy = false;
static bool label_y_anim_busy = false;

static lv_obj_t *loading_anim_cont = NULL; // Loading animation container

static uint8_t anim_active = 0; // Default determined in lcd_anim_nvs_load

/* Animation */
typedef struct {
    lv_obj_t *img; // Single lv_img
    const char **frames; // Pointer to file‐path strings
    uint8_t frame_cnt; // Num frames
    bool pingpong; // False = wrap
    bool forward; // Current direction in pingpong
    uint8_t cur; // Current frame index
    lv_timer_t *timer; // LVGL timer
} anim_t;

// Define animation frame paths
#ifdef POLYCAST5_EN_CITY_ANIM
const char *city_paths[CITY_FRAME_CNT] = { // 64.84KB each
    ANIM_CITY_1, ANIM_CITY_2, ANIM_CITY_3,    ANIM_CITY_4, ANIM_CITY_5,
    ANIM_CITY_6, ANIM_CITY_7, ANIM_CITY_8,    ANIM_CITY_9, ANIM_CITY_10,
    ANIM_CITY_11, ANIM_CITY_12, ANIM_CITY_13, ANIM_CITY_14, ANIM_CITY_15,
    ANIM_CITY_16, ANIM_CITY_17, ANIM_CITY_18, ANIM_CITY_19, ANIM_CITY_20,
    ANIM_CITY_21, ANIM_CITY_22, ANIM_CITY_23, ANIM_CITY_24, ANIM_CITY_25,
    ANIM_CITY_26, ANIM_CITY_27, ANIM_CITY_28, ANIM_CITY_29, ANIM_CITY_30,
    ANIM_CITY_31, ANIM_CITY_32, ANIM_CITY_33, ANIM_CITY_34, ANIM_CITY_35,
    ANIM_CITY_36, ANIM_CITY_37, ANIM_CITY_38, ANIM_CITY_39, ANIM_CITY_40,
    ANIM_CITY_41, ANIM_CITY_42, ANIM_CITY_43, ANIM_CITY_44, ANIM_CITY_45,
    ANIM_CITY_46, ANIM_CITY_47, ANIM_CITY_48, ANIM_CITY_49, ANIM_CITY_50,
    ANIM_CITY_51, ANIM_CITY_52, ANIM_CITY_53, ANIM_CITY_54, ANIM_CITY_55,
    ANIM_CITY_56, ANIM_CITY_57, ANIM_CITY_58, ANIM_CITY_59, ANIM_CITY_60,
};
#endif

#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
const char *black_hole_paths[BLACK_HOLE_FRAME_CNT] = {
    ANIM_BLACK_HOLE_1, ANIM_BLACK_HOLE_2, ANIM_BLACK_HOLE_3, ANIM_BLACK_HOLE_4, ANIM_BLACK_HOLE_5,
    ANIM_BLACK_HOLE_6, ANIM_BLACK_HOLE_7, ANIM_BLACK_HOLE_8, ANIM_BLACK_HOLE_9, ANIM_BLACK_HOLE_10,
    ANIM_BLACK_HOLE_11, ANIM_BLACK_HOLE_12, ANIM_BLACK_HOLE_13, ANIM_BLACK_HOLE_14, ANIM_BLACK_HOLE_15,
    ANIM_BLACK_HOLE_16, ANIM_BLACK_HOLE_17, ANIM_BLACK_HOLE_18
};
#endif

#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
const char *matrix_rain_paths[MATRIX_RAIN_FRAME_CNT] = {
    ANIM_MATRIX_RAIN_1, ANIM_MATRIX_RAIN_2, ANIM_MATRIX_RAIN_3, ANIM_MATRIX_RAIN_4,    ANIM_MATRIX_RAIN_5,
    ANIM_MATRIX_RAIN_6, ANIM_MATRIX_RAIN_7, ANIM_MATRIX_RAIN_8, ANIM_MATRIX_RAIN_9,    ANIM_MATRIX_RAIN_10,
    ANIM_MATRIX_RAIN_11, ANIM_MATRIX_RAIN_12, ANIM_MATRIX_RAIN_13, ANIM_MATRIX_RAIN_14, ANIM_MATRIX_RAIN_15,
    ANIM_MATRIX_RAIN_16, ANIM_MATRIX_RAIN_17, ANIM_MATRIX_RAIN_18, ANIM_MATRIX_RAIN_19, ANIM_MATRIX_RAIN_20,
    ANIM_MATRIX_RAIN_21, ANIM_MATRIX_RAIN_22, ANIM_MATRIX_RAIN_23, ANIM_MATRIX_RAIN_24, ANIM_MATRIX_RAIN_25,
    ANIM_MATRIX_RAIN_26, ANIM_MATRIX_RAIN_27, ANIM_MATRIX_RAIN_28, ANIM_MATRIX_RAIN_29, ANIM_MATRIX_RAIN_30,
    ANIM_MATRIX_RAIN_31, ANIM_MATRIX_RAIN_32, ANIM_MATRIX_RAIN_33, ANIM_MATRIX_RAIN_34, ANIM_MATRIX_RAIN_35,
    ANIM_MATRIX_RAIN_36, ANIM_MATRIX_RAIN_37, ANIM_MATRIX_RAIN_38, ANIM_MATRIX_RAIN_39, ANIM_MATRIX_RAIN_40,
    ANIM_MATRIX_RAIN_41, ANIM_MATRIX_RAIN_42
};
#endif

#ifdef POLYCAST5_EN_PYRAMID_ANIM
const char *pyramid_paths[PYRAMID_FRAME_CNT] = {
    ANIM_PYRAMID_1, ANIM_PYRAMID_2, ANIM_PYRAMID_3, ANIM_PYRAMID_4, ANIM_PYRAMID_5,
    ANIM_PYRAMID_6, ANIM_PYRAMID_7,  ANIM_PYRAMID_8, ANIM_PYRAMID_9, ANIM_PYRAMID_10,
    ANIM_PYRAMID_11, ANIM_PYRAMID_12, ANIM_PYRAMID_13, ANIM_PYRAMID_14, ANIM_PYRAMID_15,
    ANIM_PYRAMID_16, ANIM_PYRAMID_17, ANIM_PYRAMID_18, ANIM_PYRAMID_19, ANIM_PYRAMID_20,
    ANIM_PYRAMID_21, ANIM_PYRAMID_22, ANIM_PYRAMID_23, ANIM_PYRAMID_24, ANIM_PYRAMID_25,
    ANIM_PYRAMID_26, ANIM_PYRAMID_27, ANIM_PYRAMID_28, ANIM_PYRAMID_29, ANIM_PYRAMID_30,
    ANIM_PYRAMID_31, ANIM_PYRAMID_32, ANIM_PYRAMID_33, ANIM_PYRAMID_34, ANIM_PYRAMID_35,
    ANIM_PYRAMID_36, ANIM_PYRAMID_37, ANIM_PYRAMID_38, ANIM_PYRAMID_39, ANIM_PYRAMID_40,
    ANIM_PYRAMID_41, ANIM_PYRAMID_42, ANIM_PYRAMID_43, ANIM_PYRAMID_44, ANIM_PYRAMID_45,
    ANIM_PYRAMID_46, ANIM_PYRAMID_47, ANIM_PYRAMID_48, ANIM_PYRAMID_49, ANIM_PYRAMID_50,
    ANIM_PYRAMID_51, ANIM_PYRAMID_52, ANIM_PYRAMID_53, ANIM_PYRAMID_54, ANIM_PYRAMID_55,
    ANIM_PYRAMID_56
};
#endif

// Animation structs
#ifdef POLYCAST5_EN_CITY_ANIM
static anim_t city_anim = {
    .frames = city_paths,
    .frame_cnt = CITY_FRAME_CNT,
    .pingpong = false,
    .forward = true,
    .cur = 0,
    .img = NULL,
    .timer = NULL
};
#endif

#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
static anim_t black_hole_anim = {
    .frames = black_hole_paths,
    .frame_cnt = BLACK_HOLE_FRAME_CNT,
    .pingpong = false,
    .forward = true,
    .cur = 0,
    .img = NULL,
    .timer = NULL
};
#endif

#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
static anim_t matrix_rain_anim = {
    .frames = matrix_rain_paths,
    .frame_cnt = MATRIX_RAIN_FRAME_CNT,
    .pingpong = false,
    .forward = true,
    .cur = 0,
    .img = NULL,
    .timer = NULL
};
#endif

#ifdef POLYCAST5_EN_PYRAMID_ANIM
static anim_t pyramid_anim = {
    .frames = pyramid_paths,
    .frame_cnt = PYRAMID_FRAME_CNT,
    .pingpong = false,
    .forward = true,
    .cur = 0,
    .img = NULL,
    .timer = NULL
};
#endif

static void warm_anim(const char **paths, int cnt)
{
    lv_image_decoder_dsc_t dsc;
    for (int i = 0; i < cnt; ++i) {
        if (lv_image_decoder_open(&dsc, paths[i], NULL) == LV_RESULT_OK) {
            lv_image_decoder_close(&dsc);
        } else {
            ESP_LOGE(TAG, "Failed to warm anim frame: %s", paths[i]);
        }
    }
}

void lcd_anim_warm_all(void)
{
#ifdef POLYCAST5_EN_CITY_ANIM
    warm_anim(city_paths, CITY_FRAME_CNT);
#endif
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    warm_anim(black_hole_paths, BLACK_HOLE_FRAME_CNT);
#endif
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    warm_anim(matrix_rain_paths, MATRIX_RAIN_FRAME_CNT);
#endif
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    warm_anim(pyramid_paths, PYRAMID_FRAME_CNT);
#endif
}

esp_err_t lcd_anim_nvs_save(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(LCD_ANIM_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    // Store anim_active as a single byte
    err = nvs_set_u8(h, LCD_ANIM_KEY, anim_active);
    if (err == ESP_OK) {
        // Commit to flash
        err = nvs_commit(h);
        
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Saved NVS animation: %u", anim_active);
#endif
    } else {
        ESP_LOGE(TAG, "Failed to save NVS animation");
    }
    
    // Close NVS
    nvs_close(h);
    return err;
}

esp_err_t lcd_anim_nvs_load(void)
{
    nvs_handle_t h;
    
    // Open NVS
    esp_err_t err = nvs_open(LCD_ANIM_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {        
        return err;
    }
    
    // Get the uint8
    uint8_t stored = 0;
    err = nvs_get_u8(h, LCD_ANIM_KEY, &stored);
    switch (err) {
        case ESP_OK:
            anim_active = stored;
            break;
        case ESP_ERR_NVS_NOT_FOUND:
            // First‐boot or key erased -> default
#ifdef POLYCAST5_EN_CITY_ANIM
            anim_active = CITY;
#elif defined(POLYCAST5_EN_BLACK_HOLE_ANIM)
            anim_active = BLACK_HOLE;
#elif defined(POLYCAST5_EN_MATRIX_RAIN_ANIM)
            anim_active = MATRIX_RAIN;
#elif defined(POLYCAST5_EN_PYRAMID_ANIM)
            anim_active = PYRAMID;
#else
            anim_active = this is an error in lcd_anim_nvs_load;
#endif
            err = ESP_OK;
            break;
        default:
            break;
    }
    
#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "Loaded NVS animation: %u", anim_active);
#endif
    
    // Close NVS
    nvs_close(h);
    return err;
}

static void anim_timer_cb(lv_timer_t *t)
{
    anim_t *anim = (anim_t *)lv_timer_get_user_data(t);
    uint8_t current = anim->cur;

    if (anim->pingpong) { // If ping ponging
        if (anim->forward) { // Going forward
            if (current + 1 < anim->frame_cnt) {
                current++; // Iterate frame
            } else { // When reached end
                anim->forward = false; // Switch dir
                current--; // Decrement frame
            }
        } else { // Going back
            if (current > 0) {
                current--; // Decrement frame
            } else { // When reached start
                anim->forward = true; // Switch dir
                current++;
            }
        }
    } else { // Wrapping
        current = (current + 1) % anim->frame_cnt; // Iterate with wrap
    }
    
    // Set frame
    anim->cur = current;
    lv_image_set_src(anim->img, anim->frames[current]);
}

void lcd_anim_init_images(void)
{
    // Load selected from NVS
    lcd_anim_nvs_load();
    
    /* City */
#ifdef POLYCAST5_EN_CITY_ANIM
    // Create image
    city_anim.img = lv_img_create(ACTIVE_SCR);
    lv_image_set_src(city_anim.img, city_anim.frames[0]);
    lv_obj_center(city_anim.img);
    
    // Create timer
    city_anim.timer = lv_timer_create(anim_timer_cb, CITY_FRAME_PERIOD, &city_anim);
    
    // Check if set
    if (anim_active != CITY) {
        lv_obj_add_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(city_anim.timer);
    }
#endif

    /* Black hole */
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    // Create image
    black_hole_anim.img = lv_img_create(ACTIVE_SCR);
    lv_image_set_src(black_hole_anim.img, black_hole_anim.frames[0]);
    lv_obj_center(black_hole_anim.img);
    
    // Create timer
    black_hole_anim.timer = lv_timer_create(anim_timer_cb, BLACK_HOLE_FRAME_PERIOD, &black_hole_anim);
    
    // Check if set
    if (anim_active != BLACK_HOLE) {
        lv_obj_add_flag(black_hole_anim.img, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(black_hole_anim.timer);
    }
#endif
    
    /* Matrix rain */
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    // Create image
    matrix_rain_anim.img = lv_img_create(ACTIVE_SCR);
    lv_image_set_src(matrix_rain_anim.img, matrix_rain_anim.frames[0]);
    lv_obj_center(matrix_rain_anim.img);
    
    // Create timer
    matrix_rain_anim.timer = lv_timer_create(anim_timer_cb, MATRIX_RAIN_FRAME_PERIOD, &matrix_rain_anim);
    
    // Check if set
    if (anim_active != MATRIX_RAIN) {
        lv_obj_add_flag(matrix_rain_anim.img, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(matrix_rain_anim.timer);
    }
#endif
    
    /* Pyramid */
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    // Create image
    pyramid_anim.img = lv_img_create(ACTIVE_SCR);
    lv_image_set_src(pyramid_anim.img, pyramid_anim.frames[0]);
    lv_obj_center(pyramid_anim.img);
    
    // Create timer
    pyramid_anim.timer = lv_timer_create(anim_timer_cb, PYRAMID_FRAME_PERIOD, &pyramid_anim);
    
    // Check if set
    if (anim_active != PYRAMID) {
        lv_obj_add_flag(pyramid_anim.img, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(pyramid_anim.timer);
    }
#endif
}

void lcd_anim_start_animation(void)
{
    // Start the active
#ifdef POLYCAST5_EN_CITY_ANIM
    if (anim_active == CITY) {
        lv_obj_remove_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(city_anim.timer);
    }
#endif
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    if (anim_active == BLACK_HOLE) {
        lv_obj_remove_flag(black_hole_anim.img,  LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(black_hole_anim.timer);
    }
#endif
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    if (anim_active == MATRIX_RAIN) {
        lv_obj_remove_flag(matrix_rain_anim.img,  LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(matrix_rain_anim.timer);
    }
#endif
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    if (anim_active == PYRAMID) {
        lv_obj_remove_flag(pyramid_anim.img,  LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(pyramid_anim.timer);
    }
#endif
}

static void pause_animations(void)
{
    // Halt all animations
#ifdef POLYCAST5_EN_CITY_ANIM
    lv_timer_pause(city_anim.timer);
#endif
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    lv_timer_pause(black_hole_anim.timer);
#endif
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    lv_timer_pause(matrix_rain_anim.timer);
#endif
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    lv_timer_pause(pyramid_anim.timer);
#endif
}

void lcd_anim_stop_animations(void)
{
    pause_animations();

    // Hide paused animations
#ifdef POLYCAST5_EN_CITY_ANIM
    lv_obj_add_flag(city_anim.img, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef POLYCAST5_EN_BLACK_HOLE_ANIM
    lv_obj_add_flag(black_hole_anim.img, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM
    lv_obj_add_flag(matrix_rain_anim.img, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef POLYCAST5_EN_PYRAMID_ANIM
    lv_obj_add_flag(pyramid_anim.img, LV_OBJ_FLAG_HIDDEN);
#endif
}

void lcd_anim_transition_animation(bool dir)
{    
    lcd_anim_stop_animations();
    
    if (dir) {
        anim_active = (anim_active + 1) % NUM_ANIMS; // + 1 with wrap
    } else {
        anim_active = (anim_active + NUM_ANIMS - 1) % NUM_ANIMS; // - 1 with wrap
    }
    
    lcd_anim_start_animation();
    
    // Save choice to NVS
    lcd_anim_nvs_save();
}

static void loading_anim_x_cb(void * var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_t *parent = lv_obj_get_parent(obj);

    if (parent) {
        lv_coord_t max_x = lv_obj_get_width(parent) - lv_obj_get_width(obj);
        if (max_x < 0) {
            max_x = 0;
        }

        if (v < 0) {
            v = 0;
        } else if (v > max_x) {
            v = max_x;
        }
    }

    lv_obj_set_x(obj, (lv_coord_t)v);
}

static void loading_anim_size_cb(void * var, int32_t v)
{
    lv_obj_t *obj = (lv_obj_t *)var;
    lv_obj_t *parent = lv_obj_get_parent(obj);

    lv_obj_set_size(obj, (lv_coord_t)v, (lv_coord_t)v);

    // Keep vertically centered within parent container as size changes
    if (parent) {
        lv_coord_t h = lv_obj_get_height(parent);
        lv_obj_set_y(obj, (h - (lv_coord_t)v) / 2);

        // Also keep X in-bounds if width changed
        lv_coord_t max_x = lv_obj_get_width(parent) - lv_obj_get_width(obj);
        if (max_x < 0) {
            max_x = 0;
        }
        lv_coord_t x = lv_obj_get_x(obj);
        if (x > max_x) {
            lv_obj_set_x(obj, max_x);
        } else if (x < 0) {
            lv_obj_set_x(obj, 0);
        }
    }
}

void lcd_anim_loading_start(lv_align_t align, lv_coord_t x_off, lv_coord_t y_off, lv_color_t color)
{
    // If already running, stop first
    if (loading_anim_cont) {
        // Stop and recreate
        lcd_anim_loading_stop();
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "lcd_anim_loading_start: Loading animation already running, restarting"); 
#endif
    }

    const lv_coord_t min_sz = 6;
    const lv_coord_t max_sz = 24;

    // Container (invisible) so we can position once and animate inside it
    loading_anim_cont = lv_obj_create(ACTIVE_SCR);
    lv_obj_set_size(loading_anim_cont, max_sz + 1, max_sz + 1);
    lv_obj_set_style_bg_opa(loading_anim_cont, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(loading_anim_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(loading_anim_cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(loading_anim_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(loading_anim_cont, align, x_off, y_off);

    // Dot
    lv_obj_t *dot = lv_obj_create(loading_anim_cont);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_size(dot, min_sz, min_sz);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

    // Pulse animation (size)
    lv_anim_t a_size;
    lv_anim_init(&a_size);
    lv_anim_set_var(&a_size, dot);
    lv_anim_set_exec_cb(&a_size, loading_anim_size_cb);
    lv_anim_set_values(&a_size, min_sz, max_sz);
    lv_anim_set_time(&a_size, 900);
    lv_anim_set_playback_delay(&a_size, 80);
    lv_anim_set_playback_time(&a_size, 280);
    lv_anim_set_repeat_delay(&a_size, 250);
    lv_anim_set_repeat_count(&a_size, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_size, lv_anim_path_ease_in_out);
    lv_anim_start(&a_size);

    // Travel animation (x) - values are clamped in loading_anim_x_cb anyway
    lv_anim_t a_x;
    lv_anim_init(&a_x);
    lv_anim_set_var(&a_x, dot);
    lv_anim_set_exec_cb(&a_x, loading_anim_x_cb);
    lv_anim_set_values(&a_x, 0, lv_obj_get_width(loading_anim_cont));
    lv_anim_set_time(&a_x, 1100);
    lv_anim_set_repeat_count(&a_x, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_x, lv_anim_path_ease_in_out);
    lv_anim_start(&a_x);
}

void lcd_anim_loading_stop(void)
{
    if (!loading_anim_cont) {
        return;
    }

    lv_obj_t *dot = lv_obj_get_child(loading_anim_cont, 0);
    if (dot) {
        lv_anim_delete(dot, loading_anim_size_cb);
        lv_anim_delete(dot, loading_anim_x_cb);
        lv_obj_delete(dot);
        dot = NULL;
    }

    lv_obj_delete(loading_anim_cont);
    loading_anim_cont = NULL;
}

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
    if (!old_lbl || !new_text || !cur_font) {
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
    lv_anim_set_time(&a_out, LCD_ANIM_DEFAULT_X_TIME);
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
    lv_anim_set_time(&a_in, LCD_ANIM_DEFAULT_X_TIME);
    lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a_in, animate_label_x_done_cb);
    lv_anim_start(&a_in);

    return new_lbl;
}

static void animate_label_y_delete_cb(lv_anim_t *a)
{
    lv_obj_t *old_lbl = (lv_obj_t *)lv_anim_get_user_data(a);
    if (old_lbl && lv_obj_is_valid(old_lbl)) {
        lv_obj_delete(old_lbl);
    }
}

static void animate_label_y_done_cb(lv_anim_t *a)
{
    (void)a;
    label_y_anim_busy = false;
}

bool lcd_anim_label_y_animate_is_busy(void)
{
    return label_y_anim_busy;
}

void lcd_anim_label_y_animate_reset(void)
{
    label_y_anim_busy = false;
}

lv_obj_t *lcd_anim_animate_label_y(lv_obj_t *old_lbl, const char *new_text, const lv_font_t *cur_font, lv_coord_t start_y_top, lv_coord_t end_y_bottom)
{
    if (!old_lbl || !new_text || !cur_font) {
        return old_lbl;
    }

    // Prevent overlapping animations if button repeats quickly
    if (label_y_anim_busy) {
        return old_lbl;
    }
    label_y_anim_busy = true;

    lv_obj_t *parent = lv_obj_get_parent(old_lbl);
    lv_coord_t old_x = lv_obj_get_x(old_lbl);
    lv_coord_t old_y = lv_obj_get_y(old_lbl);
    lv_coord_t old_w = lv_obj_get_width(old_lbl);
    lv_coord_t old_h = lv_obj_get_height(old_lbl);

    // Create incoming label with same style
    lv_obj_t *new_lbl = lv_label_create(parent);
    lv_label_set_text(new_lbl, new_text);
    lv_obj_set_style_text_font(new_lbl, cur_font, 0);
    lv_obj_set_style_text_color(new_lbl, user_secondary_color, 0);
    lv_obj_update_layout(new_lbl);

    lv_coord_t new_w = lv_obj_get_width(new_lbl);
    lv_coord_t new_h = lv_obj_get_height(new_lbl);

    // Keep same visual center as old label
    lv_coord_t target_x = old_x + (old_w - new_w) / 2;
    lv_coord_t target_y = old_y + (old_h - new_h) / 2;

    lv_obj_set_pos(new_lbl, target_x, start_y_top);

    // Old label: center -> bottom
    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, old_lbl);
    lv_anim_set_values(&a_out, old_y, end_y_bottom);
    lv_anim_set_time(&a_out, LCD_ANIM_DEFAULT_Y_TIME);
    lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a_out, animate_label_y_delete_cb);
    lv_anim_set_user_data(&a_out, old_lbl);
    lv_anim_start(&a_out);

    // New label: top -> center
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, new_lbl);
    lv_anim_set_values(&a_in, start_y_top, target_y);
    lv_anim_set_time(&a_in, LCD_ANIM_DEFAULT_Y_TIME);
    lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a_in, animate_label_y_done_cb);
    lv_anim_start(&a_in);

    return new_lbl;
}
