#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"

#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include "polycast5_macros.h"
#include "gpio_task.h"
#include "lcd_utils.h"
#include "lcd_games.h"

#define TAG "LCD_GAMES"

#define HIGH_SCORE_NS "tetris"
#define HIGH_SCORE_KEY "score"

games_menu_t games_menu = {
    .options = {"DOOM", "Tetris", "T-Rex Runner", "Flappy Bird"},
    .size = 4,
    .index = 0,
    .cont = NULL,
};

void lcd_games_setup_page(games_menu_t *menu)
{
    // Create list
    menu->main_list = lv_list_create(ACTIVE_SCR);
    lv_obj_set_size(menu->main_list, 210, 106);

    // Format
    lv_obj_set_style_bg_color(menu->main_list, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(menu->main_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_width(menu->main_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lcd_apply_scrollbar_style(menu->main_list);
    lv_obj_set_scroll_dir(menu->main_list, LV_DIR_VER);

    // Create button style
    lv_style_init(&menu->btn_style);

    lv_style_set_radius(&menu->btn_style, 8);
    lv_style_set_bg_color(&menu->btn_style, user_primary_color);

    lv_style_set_border_width(&menu->btn_style, 2);
    lv_style_set_border_color(&menu->btn_style, user_secondary_color);
    lv_style_set_border_side(&menu->btn_style, LV_BORDER_SIDE_FULL);

    lv_style_set_pad_top(&menu->btn_style, 3);
    lv_style_set_pad_bottom(&menu->btn_style, 3);

    lv_style_set_text_font(&menu->btn_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->btn_style, user_secondary_color);
    lv_style_set_text_align(&menu->btn_style, LV_TEXT_ALIGN_CENTER);

    // Create selected button style
    lv_style_init(&menu->sel_style);

    lv_style_set_radius(&menu->sel_style, 8);
    lv_style_set_bg_color(&menu->sel_style, user_secondary_color);

    lv_style_set_border_width(&menu->sel_style, 2);
    lv_style_set_border_color(&menu->sel_style, user_secondary_color);
    lv_style_set_border_side(&menu->sel_style, LV_BORDER_SIDE_FULL);

    lv_style_set_pad_top(&menu->sel_style, 3);
    lv_style_set_pad_bottom(&menu->sel_style, 3);

    lv_style_set_text_font(&menu->sel_style, &lv_font_montserrat_16);
    lv_style_set_text_color(&menu->sel_style, user_primary_color);
    lv_style_set_text_align(&menu->sel_style, LV_TEXT_ALIGN_CENTER);

    // Create buttons
    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }

    // Create button for each option
    for (int i = 0; i < menu->size; ++i) {

        menu->btns[i] = lv_list_add_btn(menu->main_list, NULL, menu->options[i]);
        lv_obj_set_size(menu->btns[i], 200, 30);

        // Style selected
        if (i == menu->index) {
            lv_obj_add_style(menu->btns[i], &menu->sel_style, 0);
        } else {
            lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
        }

        // Create and format text label
        lv_obj_t *lbl = lv_obj_get_child(menu->btns[i], 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
    }

    // Format buttons as container
    menu->cont = lv_obj_get_parent(menu->btns[0]);
    lv_obj_set_flex_flow (menu->cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu->cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(menu->cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT); // Set button spacing

    // Hide for now
    lv_obj_add_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);
}

void lcd_games_update_menu(games_menu_t *menu)
{
    // Reveal
    lv_obj_remove_flag(menu->main_list, LV_OBJ_FLAG_HIDDEN);

    // Wrap index
    if (menu->index >= menu->size) {
        menu->index = 0;
    } else if (menu->index < 0) {
        menu->index = menu->size - 1;
    }

    // Reset every button to unselected
    for (int i = 0; i < menu->size; ++i) {
        lv_obj_remove_style(menu->btns[i], &menu->sel_style, 0);
        lv_obj_add_style(menu->btns[i], &menu->btn_style, 0);
    }

    // Highlight only the current index
    lv_obj_remove_style(menu->btns[menu->index], &menu->btn_style, 0);
    lv_obj_add_style(menu->btns[menu->index], &menu->sel_style, 0);

    // Enable scrolling if list gets too long
    lv_obj_scroll_to_view(menu->btns[menu->index], LV_ANIM_ON); // LV_ANIM_OFF
}

void lcd_games_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu)
{
    // Statics
    static bool do_once = false;

    // Only execute once
    if (!do_once) {
        // Show games list
        lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        do_once = true;
    }

    // Up button pressed
    if (ui_btns->up_btn == 1) {
        // Update selection
        games_menu->index--;
        lcd_games_update_menu(games_menu);
    } else if (ui_btns->down_btn == 1) { // Down button pressed
        // Update selection
        games_menu->index++;
        lcd_games_update_menu(games_menu);
    } else if (ui_btns->select_btn == 1 && games_menu->index == 0) { // DOOM selected
        // Hide games menu
        lv_obj_add_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Reset static
        do_once = false;

        // Hide all arrows
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

        // Switch pages
        ui_menu->page = GAMES_DOOM_PAGE;
    } else if (ui_btns->select_btn == 1 && games_menu->index == 1) { // Tetris selected
        // Hide games menu
        lv_obj_add_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Reset static
        do_once = false;

        // Show right arrow
        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        // Switch pages
        ui_menu->page = GAMES_TETRIS_PAGE;
    } else if (ui_btns->select_btn == 1 && games_menu->index == 2) { // T-Rex Runner selected
        // Hide games menu
        lv_obj_add_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Reset static
        do_once = false;

        // Hide all arrows
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

        // Switch pages
        ui_menu->page = GAMES_TREX_PAGE;
    } else if (ui_btns->select_btn == 1 && games_menu->index == 3) { // Flappy bird selected
        // Hide games menu
        lv_obj_add_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Reset static
        do_once = false;

        // Hide all arrows
        lv_obj_add_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);

        // Switch pages
        ui_menu->page = GAMES_FLAPPY_PAGE;
    } else if (ui_btns->left_btn == 1) { // Back selected
        // Hide games menu
        lv_obj_add_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Show selection labels
        lcd_unhide_selection_widgets(ui_menu);

        // Reset static
        do_once = false;

        // Switch pages
        ui_menu->page = SELECTION_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off selected
        // Hide games menu
        lv_obj_add_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Reset static
        do_once = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

/* =============== TETRIS IMPLEMENTATION =============== */

// Macros for game config
#define ORTHO_SIZE 10 // Number of rows (vertical when held sideways)
#define FALL_SIZE 25 // Total fall length (horizontal on screen)
#define VISIBLE_FALL 21    // Visible fall length
#define HIDDEN_FALL 4 // Hidden start columns (left on screen)
#define CELL_SIZE 10 // Pixel size per cell (210/21 ≈10, 106/10=10.6 ≈10)
#define FALL_DELAY_MS 500 // Base fall speed (decreases with level)
#define SOFT_DROP_MULTIPLIER 5 // Faster fall when down pressed
#define LEVEL_SPEED_INCREASE 50 // MS decrease per level (every 1000 points)

// Game colors
#define COLOR_PIECE_FALLING (lv_color_make(0, 139, 0)) // 8B Green #008B00
#define COLOR_PIECE_RESTING (lv_color_make(0, 71, 171)) // Cobalt blue #0047AB
#define COLOR_PIECE_OUTLINE (lv_color_white())
#define COLOR_BOARD_FILL (lv_color_black())

// Tetromino shapes: 4 rotations each per shape, 4x4 grid
static const int tetrominoes[7][4][16] = {
    // I
 {{0,0,0,0, 1,1,1,1, 0,0,0,0, 0,0,0,0},
     {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0},
     {0,0,0,0, 1,1,1,1, 0,0,0,0, 0,0,0,0},
     {0,0,1,0, 0,0,1,0, 0,0,1,0, 0,0,1,0}},
    // O
    {{0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0},
     {0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0},
     {0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0},
     {0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0}},
    // T
    {{0,0,0,0, 0,1,0,0, 1,1,1,0, 0,0,0,0},
     {0,0,1,0, 0,1,1,0, 0,0,1,0, 0,0,0,0},
     {0,0,0,0, 1,1,1,0, 0,1,0,0, 0,0,0,0},
     {0,1,0,0, 0,1,1,0, 0,1,0,0, 0,0,0,0}},
    // S
    {{0,0,0,0, 0,1,1,0, 1,1,0,0, 0,0,0,0},
     {0,1,0,0, 0,1,1,0, 0,0,1,0, 0,0,0,0},
     {0,0,0,0, 0,1,1,0, 1,1,0,0, 0,0,0,0},
     {0,1,0,0, 0,1,1,0, 0,0,1,0, 0,0,0,0}},
    // Z
    {{0,0,0,0, 1,1,0,0, 0,1,1,0, 0,0,0,0},
     {0,0,1,0, 0,1,1,0, 0,1,0,0, 0,0,0,0},
     {0,0,0,0, 1,1,0,0, 0,1,1,0, 0,0,0,0},
     {0,0,1,0, 0,1,1,0, 0,1,0,0, 0,0,0,0}},
    // J
    {{0,0,0,0, 1,0,0,0, 1,1,1,0, 0,0,0,0},
     {0,1,1,0, 0,1,0,0, 0,1,0,0, 0,0,0,0},
     {0,0,0,0, 1,1,1,0, 0,0,1,0, 0,0,0,0},
     {0,0,1,0, 0,0,1,0, 0,1,1,0, 0,0,0,0}},
    // L
    {{0,0,0,0, 0,0,1,0, 1,1,1,0, 0,0,0,0},
     {0,1,0,0, 0,1,0,0, 0,1,1,0, 0,0,0,0},
     {0,0,0,0, 1,1,1,0, 1,0,0,0, 0,0,0,0},
     {0,1,1,0, 0,0,1,0, 0,0,1,0, 0,0,0,0}}
};

typedef struct {
    int x, y; // x: fall position (horizontal), y: orthogonal position (vertical)
    int type; // 0-6
    int rotation; // 0-3
} tetris_piece_t;

// Game state (PSRAM: only touched at the page tick, never from ISRs)
POLYCAST5_USE_PSRAM_BSS static int tetris_board[ORTHO_SIZE][FALL_SIZE]; // board[row][col], col increases right (fall dir)
POLYCAST5_USE_PSRAM_BSS static tetris_piece_t tetris_current_piece;
POLYCAST5_USE_PSRAM_BSS static tetris_piece_t tetris_next_piece;
POLYCAST5_USE_PSRAM_BSS static uint32_t tetris_score;
POLYCAST5_USE_PSRAM_BSS static bool tetris_game_over;
POLYCAST5_USE_PSRAM_BSS static TickType_t tetris_last_fall_time;

// LVGL elements
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *tetris_canvas;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *tetris_score_label;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *tetris_game_over_label;

POLYCAST5_USE_PSRAM_BSS static void *tetris_canvas_pixels; // Raw pixel buffer in PSRAM

// Save score as high score
static void tetris_high_score_nvs_save(uint32_t score)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(HIGH_SCORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tetris_high_score_nvs_save nvs_open failed");
        return; // Handle not open - nothing to close
    }

    // Store high score as a uint32
    err = nvs_set_u32(h, HIGH_SCORE_KEY, score);
    if (err == ESP_OK) {
        // Commit to flash
        err = nvs_commit(h);

#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Saved Tetris high score: %" PRIu32, score);
#endif
    } else {
        ESP_LOGE(TAG, "Failed to tetris_high_score_nvs_save nvs_set_u32: %" PRIu32, score);
    }

    // Close NVS
    nvs_close(h);
}

// Load Tetris high score
static uint32_t tetris_high_score_nvs_load(void)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(HIGH_SCORE_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "tetris_high_score_nvs_load NS DNE");
#endif
        return 0; // Handle not open - no high score saved yet
    }

    // Get the uint32
    uint32_t score = 0;
    err = nvs_get_u32(h, HIGH_SCORE_KEY, &score);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "Failed tetris_high_score_nvs_load nvs_get_u32");
#endif
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Loaded Tetris high score: %" PRIu32, score);
#endif
    }

    // Close NVS
    nvs_close(h);

    return score;
}

// Helper: Check if piece collides at given pos/rot
static bool check_collision(int x, int y, int rotation)
{
    // Get shape and given rotation
    const int *shape = tetrominoes[tetris_current_piece.type][rotation];

    // Loops over 4x4: If cell occupied, calculates board position
    for (int i = 0; i < 4; i++) { // i: ortho (row)
        for (int j = 0; j < 4; j++) { // j: fall (col)
            // Checks out-of-bounds or overlap with existing board cell
            if (shape[i * 4 + j]) { // i * 4 + j converts 2D coordinates (row i, col j) to a 1D index in the flattened array
                int board_col = x + j;
                int board_row = y + i;

                // Returns true if collision
                if (board_col < 0 || board_col >= FALL_SIZE || board_row < 0 || board_row >= ORTHO_SIZE || tetris_board[board_row][board_col]) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Helper: Place piece on board
static void place_piece()
{
    // Get shape and given rotation
    const int *shape = tetrominoes[tetris_current_piece.type][tetris_current_piece.rotation];

    // Loops over 4x4: If occupied, sets board cell at piece position to 1 (locked)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (shape[i * 4 + j]) { // i * 4 + j converts 2D coordinates (row i, col j) to a 1D index in the flattened array
                tetris_board[tetris_current_piece.y + i][tetris_current_piece.x + j] = 1;
            }
        }
    }
}

// Helper: Clear full lines (now full columns in fall dir), return lines cleared
static int clear_lines()
{
    int lines = 0;

    // Scans columns from right (bottom in rotated view) to left
    for (int col = FALL_SIZE - 1; col >= 0; col--) {
        // For each column: Check if all rows occupied (full=true)

        bool full = true;

        for (int row = 0; row < ORTHO_SIZE; row++) {
            if (!tetris_board[row][col]) {
                full = false;
                break;
            }
        }

        // If full: Increment lines, shift all columns left (copy from c-1 to c), clear leftmost (new empty)
        if (full) {
            lines++;

            // Shift left (decrease col)
            for (int c = col; c > 0; c--) {
                for (int r = 0; r < ORTHO_SIZE; r++) {
                    tetris_board[r][c] = tetris_board[r][c - 1];
                }
            }

            // Clear leftmost col
            for (int r = 0; r < ORTHO_SIZE; r++) {
                tetris_board[r][0] = 0;
            }

            // Re-scan the now-shifted column (might be full again after multi-clear)
            col++;
        }
    }

    return lines;
}

// Helper: Spawn new piece
static void spawn_piece()
{
    // Copies next to current
    tetris_current_piece = tetris_next_piece;

    // Sets spawn: x=0 (hidden left), y=3 (middle of 10 rows, centered for 4-cell height)
    tetris_current_piece.x = 0;
    tetris_current_piece.y = ORTHO_SIZE / 2 - 2;

    // Random next type (0-6), rotation=0
    tetris_next_piece.type = esp_random() % 7;
    tetris_next_piece.rotation = 0;

    // If collides at spawn (stack too high), set game_over
    if (check_collision(tetris_current_piece.x, tetris_current_piece.y, tetris_current_piece.rotation)) {
        tetris_game_over = true;
    }
}

// Helper: Draw board on canvas (only visible cols)
static void draw_board()
{
    lv_canvas_fill_bg(tetris_canvas, COLOR_BOARD_FILL, LV_OPA_COVER); // Clear background

    // Inits a draw layer for batched rendering
    lv_layer_t layer;
    lv_canvas_init_layer(tetris_canvas, &layer);

    // Draw board cells: For each occupied cell, draw filled rect (primary color) + border (secondary color)
    for (int col = HIDDEN_FALL; col < HIDDEN_FALL + VISIBLE_FALL; col++) { // Visible only
        for (int row = 0; row < ORTHO_SIZE; row++) {
            if (tetris_board[row][col]) {
                int px = (col - HIDDEN_FALL) * CELL_SIZE; // x increases right (fall)
                int py = row * CELL_SIZE; // y increases down

                // Fill
                lv_draw_rect_dsc_t fill_dsc;
                lv_draw_rect_dsc_init(&fill_dsc);
                fill_dsc.bg_color = COLOR_PIECE_RESTING;
                fill_dsc.bg_opa = LV_OPA_COVER;
                fill_dsc.radius = 0;
                lv_area_t fill_area;
                lv_area_set(&fill_area, px, py, px + CELL_SIZE - 1, py + CELL_SIZE - 1);
                lv_draw_rect(&layer, &fill_dsc, &fill_area);

                // Border
                lv_draw_rect_dsc_t border_dsc;
                lv_draw_rect_dsc_init(&border_dsc);
                border_dsc.border_color = COLOR_PIECE_OUTLINE;
                border_dsc.border_width = 1;
                border_dsc.border_opa = LV_OPA_COVER;
                border_dsc.bg_opa = LV_OPA_TRANSP; // Transparent fill for border only
                border_dsc.radius = 0;
                lv_draw_rect(&layer, &border_dsc, &fill_area);
            }
        }
    }

    // Draws current piece similarly
    const int *shape = tetrominoes[tetris_current_piece.type][tetris_current_piece.rotation];
    for (int i = 0; i < 4; i++) { // i: ortho offset
        for (int j = 0; j < 4; j++) { // j: fall offset
            if (shape[i * 4 + j]) { // i * 4 + j converts 2D coordinates (row i, col j) to a 1D index in the flattened array
                int board_col = tetris_current_piece.x + j;
                int board_row = tetris_current_piece.y + i;

                if (board_col >= HIDDEN_FALL && board_col < HIDDEN_FALL + VISIBLE_FALL) { // Visible
                    int px = (board_col - HIDDEN_FALL) * CELL_SIZE;
                    int py = board_row * CELL_SIZE;

                    // Fill
                    lv_draw_rect_dsc_t fill_dsc;
                    lv_draw_rect_dsc_init(&fill_dsc);
                    fill_dsc.bg_color = COLOR_PIECE_FALLING;
                    fill_dsc.bg_opa = LV_OPA_COVER;
                    fill_dsc.radius = 0;
                    lv_area_t fill_area;
                    lv_area_set(&fill_area, px, py, px + CELL_SIZE - 1, py + CELL_SIZE - 1);
                    lv_draw_rect(&layer, &fill_dsc, &fill_area);

                    // Border
                    lv_draw_rect_dsc_t border_dsc;
                    lv_draw_rect_dsc_init(&border_dsc);
                    border_dsc.border_color = COLOR_PIECE_OUTLINE;
                    border_dsc.border_width = 1;
                    border_dsc.border_opa = LV_OPA_COVER;
                    border_dsc.bg_opa = LV_OPA_TRANSP;
                    border_dsc.radius = 0;
                    lv_draw_rect(&layer, &border_dsc, &fill_area);
                }
            }
        }
    }

    // Draw whole board outline
    lv_draw_rect_dsc_t board_border_dsc;
    lv_draw_rect_dsc_init(&board_border_dsc);
    board_border_dsc.border_color = COLOR_PIECE_OUTLINE;
    board_border_dsc.border_width = 1;
    board_border_dsc.border_opa = LV_OPA_COVER;
    board_border_dsc.bg_opa = LV_OPA_TRANSP;
    board_border_dsc.radius = 0;
    lv_area_t board_area;
    lv_area_set(&board_area, 0, 0, VISIBLE_FALL * CELL_SIZE - 1, ORTHO_SIZE * CELL_SIZE - 1);
    lv_draw_rect(&layer, &board_border_dsc, &board_area);

    // Finishes layer, invalidates canvas to trigger screen update
    lv_canvas_finish_layer(tetris_canvas, &layer);
    lv_obj_invalidate(tetris_canvas); // Refresh
}

void lcd_games_tetris_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu)
{
    static bool init = false;
    static lv_draw_buf_t canvas_buf; // Metadata struct (small, internal SRAM)

    if (!init) {
        // Reset game state
        memset(tetris_board, 0, sizeof(tetris_board));
        tetris_score = 0;
        tetris_game_over = false;
        tetris_next_piece.type = esp_random() % 7;
        spawn_piece();
        tetris_last_fall_time = xTaskGetTickCount();

        // Allocate pixel buffer in PSRAM
        size_t buf_size = VISIBLE_FALL * CELL_SIZE * ORTHO_SIZE * CELL_SIZE * 2; // RGB565: 2 bytes/pixel
        tetris_canvas_pixels = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tetris_canvas_pixels) {
            ESP_LOGE("TETRIS", "Failed to alloc PSRAM for canvas");

            // Fallback or exit to menu
            ui_menu->page = GAMES_PAGE;
            return;
        }

        // Init draw buf metadata (small struct in internal SRAM)
        lv_draw_buf_init(&canvas_buf, VISIBLE_FALL * CELL_SIZE, ORTHO_SIZE * CELL_SIZE, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, tetris_canvas_pixels, buf_size);

        // Create canvas (wide horizontally for fall left->right)
        tetris_canvas = lv_canvas_create(ACTIVE_SCR);
        lv_canvas_set_draw_buf(tetris_canvas, &canvas_buf);
        lv_obj_set_size(tetris_canvas, VISIBLE_FALL * CELL_SIZE, ORTHO_SIZE * CELL_SIZE);
        lv_obj_align(tetris_canvas, LV_ALIGN_CENTER, 0, 0); // Center, adjust if needed for 210x100

        // Score label (position adjusted for layout)
        tetris_score_label = lv_label_create(ACTIVE_SCR);
        lv_label_set_text(tetris_score_label, "Score: 0");
        lv_obj_set_style_text_color(tetris_score_label, user_secondary_color, 0);
        lv_obj_align(tetris_score_label, LV_ALIGN_TOP_MID, 0, -20); // Above canvas, adjust

        // Game over label (hidden initially)
        tetris_game_over_label = lv_label_create(ACTIVE_SCR);
        lv_label_set_text(tetris_game_over_label, "");
        lv_obj_set_style_text_font(tetris_game_over_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(tetris_game_over_label, user_secondary_color, 0);
        lv_obj_set_style_text_align(tetris_game_over_label, LV_TEXT_ALIGN_CENTER, 0); // Center each line, not just the label
        lv_obj_align(tetris_game_over_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(tetris_game_over_label, LV_OBJ_FLAG_HIDDEN);

        draw_board();
        init = true;
    }

    // Handle game over
    if (tetris_game_over) {
        // Clear the board visually
        lv_canvas_fill_bg(tetris_canvas, user_primary_color, LV_OPA_COVER);
        lv_obj_invalidate(tetris_canvas);

        // Get old high score and compare to current
        uint32_t high_score = tetris_high_score_nvs_load();
        if (tetris_score > high_score) {
            // If new high score, save
            high_score = tetris_score;
            tetris_high_score_nvs_save(high_score);
        }

        char buf[41];
        snprintf(buf, sizeof(buf), "Game Over!\nScore: %" PRIu32 "\nHigh Score: %" PRIu32, tetris_score, high_score);
        lv_label_set_text(tetris_game_over_label, buf);
        lv_obj_remove_flag(tetris_game_over_label, LV_OBJ_FLAG_HIDDEN);

        // Any button to exit
        if (ui_btns->up_btn || ui_btns->down_btn || ui_btns->left_btn || ui_btns->right_btn || ui_btns->select_btn || ui_btns->home_btn) {
            // Cleanup
            lv_obj_delete(tetris_canvas);
            lv_obj_delete(tetris_score_label);
            lv_obj_delete(tetris_game_over_label);
            heap_caps_free(tetris_canvas_pixels); // Free PSRAM

            tetris_canvas = tetris_score_label = tetris_game_over_label = NULL;
            tetris_canvas_pixels = NULL;
            init = false;

            // Back to games menu
            lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = GAMES_PAGE;
        }
        return;
    }

    /* Input handling */
    bool moved = false;
    // Move piece right
    if (ui_btns->up_btn) {
        if (!check_collision(tetris_current_piece.x, tetris_current_piece.y - 1, tetris_current_piece.rotation)) {
            tetris_current_piece.y--;
            moved = true;
        }
    } else if (ui_btns->down_btn) { // Move piece left
        if (!check_collision(tetris_current_piece.x, tetris_current_piece.y + 1, tetris_current_piece.rotation)) {
            tetris_current_piece.y++;
            moved = true;
        }
    } else if (ui_btns->left_btn) { // Rotate piece
        int new_rot = (tetris_current_piece.rotation + 1) % 4;
        if (!check_collision(tetris_current_piece.x, tetris_current_piece.y, new_rot)) {
            tetris_current_piece.rotation = new_rot;
            moved = true;
        }
    } else if (ui_btns->select_btn) { // Hard drop (fast forward x)
        while (!check_collision(tetris_current_piece.x + 1, tetris_current_piece.y, tetris_current_piece.rotation)) {
            tetris_current_piece.x++;
        }

        place_piece();
        int lines = clear_lines();
        tetris_score += 100 * lines * lines; // Bonus for multi-lines
        spawn_piece();
        moved = true;
    } else if (ui_btns->home_btn) { // Exit to menu
        // Delete objects
        lv_obj_delete(tetris_canvas);
        lv_obj_delete(tetris_score_label);
        lv_obj_delete(tetris_game_over_label);
        heap_caps_free(tetris_canvas_pixels); // Free PSRAM

        // Reset statics
        tetris_canvas = tetris_score_label = tetris_game_over_label = tetris_canvas_pixels = NULL;
        init = false;

        // Show games menu
        lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = GAMES_PAGE;
        return;
    } else if (ui_btns->pwr_btn) { // Sleep
        // Delete objects
        lv_obj_delete(tetris_canvas);
        lv_obj_delete(tetris_score_label);
        lv_obj_delete(tetris_game_over_label);
        heap_caps_free(tetris_canvas_pixels); // Free PSRAM

        // Reset statics
        tetris_canvas = tetris_score_label = tetris_game_over_label = tetris_canvas_pixels = NULL;
        init = false;

        lcd_transition_back(false, ui_menu); // False = sleep
        return;
    }

    // Time-based fall (increase x)
    TickType_t now = xTaskGetTickCount();
    uint32_t fall_reduction = (tetris_score / 1000) * LEVEL_SPEED_INCREASE; // Speed up with level
    uint32_t fall_delay;

    if (fall_reduction + 100 >= FALL_DELAY_MS) { // Clamp before subtracting to avoid underflow
        fall_delay = 100; // Min speed
    } else {
        fall_delay = FALL_DELAY_MS - fall_reduction;
    }

    // Soft drop with right button
    if (ui_btns->right_btn) {
        fall_delay /= SOFT_DROP_MULTIPLIER;
    }

    if (now - tetris_last_fall_time >= pdMS_TO_TICKS(fall_delay)) {
        if (!check_collision(tetris_current_piece.x + 1, tetris_current_piece.y, tetris_current_piece.rotation)) {
            tetris_current_piece.x++;
            moved = true;
        } else {
            place_piece();
            int lines = clear_lines();
            tetris_score += 100 * lines * lines;
            spawn_piece();
            moved = true;
        }

        tetris_last_fall_time = now;
    }

    // Update UI if changed
    if (moved) {
        draw_board();
        char buf[16];
        snprintf(buf, sizeof(buf), "Score: %" PRIu32, tetris_score);
        lv_label_set_text(tetris_score_label, buf);
    }
}

/* ========== Shared game-over overlay (white text, black stroke) ========== */

// 8 directions for the text stroke: the overlay text is drawn once per offset
// in black behind a white copy so the readout pops against the game background
static const int8_t kGameOverStroke[8][2] = {
    {-1, -1}, {0, -1}, {1, -1},
    {-1,  0},          {1,  0},
    {-1,  1}, {0,  1}, {1,  1},
};

// Create the centered overlay: 8 black stroke copies (behind) + 1 white main
// label on top, all hidden until game over. main is created last to sit on top
static void game_over_overlay_create(lv_obj_t **main, lv_obj_t *stroke[8])
{
    for (int i = 0; i < 8; i++) {
        stroke[i] = lv_label_create(ACTIVE_SCR);
        lv_label_set_text(stroke[i], "");
        lv_obj_set_style_text_font(stroke[i], &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(stroke[i], lv_color_black(), 0);
        lv_obj_set_style_text_align(stroke[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(stroke[i], LV_ALIGN_CENTER, kGameOverStroke[i][0], kGameOverStroke[i][1]);
        lv_obj_add_flag(stroke[i], LV_OBJ_FLAG_HIDDEN);
    }

    *main = lv_label_create(ACTIVE_SCR);
    lv_label_set_text(*main, "");
    lv_obj_set_style_text_font(*main, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(*main, lv_color_white(), 0);
    lv_obj_set_style_text_align(*main, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(*main, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(*main, LV_OBJ_FLAG_HIDDEN);
}

// Set the same text on every copy and reveal the overlay
static void game_over_overlay_show(lv_obj_t *main, lv_obj_t *stroke[8], const char *txt)
{
    for (int i = 0; i < 8; i++) {
        lv_label_set_text(stroke[i], txt);
        lv_obj_remove_flag(stroke[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(main, txt);
    lv_obj_remove_flag(main, LV_OBJ_FLAG_HIDDEN);
}

// Hide every copy (used on restart)
static void game_over_overlay_hide(lv_obj_t *main, lv_obj_t *stroke[8])
{
    for (int i = 0; i < 8; i++) {
        lv_obj_add_flag(stroke[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(main, LV_OBJ_FLAG_HIDDEN);
}

// Delete every copy (used on exit/cleanup)
static void game_over_overlay_delete(lv_obj_t *main, lv_obj_t *stroke[8])
{
    lv_obj_delete(main);
    for (int i = 0; i < 8; i++) {
        lv_obj_delete(stroke[i]);
        stroke[i] = NULL;
    }
}

/* ========== T-Rex Runner Implementation ========== */

#define TREX_HIGH_SCORE_NS "trex"

// Macros for game config
#define TREX_CANVAS_W 220 // Canvas width in px
#define TREX_CANVAS_H 90 // Canvas height in px
#define TREX_FRAME_MS 50 // Physics/render tick (20 FPS via lv_timer)
#define TREX_GROUND_Y 78 // Ground line y inside canvas
#define TREX_DINO_X 10 // Fixed dino left edge
#define TREX_DINO_W 20 // Standing dino width
#define TREX_DINO_H 22 // Standing dino height
#define TREX_DUCK_W 26 // Ducking dino width
#define TREX_DUCK_H 14 // Ducking dino height
#define TREX_JUMP_V0_Q4 192 // Initial jump velocity (Q4 px/frame, positive = up)
#define TREX_GRAVITY_Q4 32 // Gravity per frame (Q4 px/frame^2)
#define TREX_FAST_DROP_Q4 96 // Extra downward velocity per down press while airborne
#define TREX_SPEED_BASE_Q4 80 // Starting scroll speed (Q4 = 5 px/frame)
#define TREX_SPEED_MAX_Q4 208 // Max scroll speed (Q4 = 13 px/frame)
#define TREX_DUCK_LINGER_MS 500 // Duck hold-over to bridge button auto-repeat gap
#define TREX_HITBOX_INSET 2 // Dino collision box inset for fairness
#define TREX_MAX_OBSTACLES 3 // Max simultaneous obstacles
#define TREX_BIRD_MIN_SCORE 300 // Birds spawn only at/after this score
#define TREX_GAP_BASE 60 // Min empty px between obstacles
#define TREX_GAP_RAND 60 // Random extra gap px
#define TREX_FIRST_SPAWN_PX 150 // Grace distance before the first obstacle

// Game colors (vibrant desert theme)
#define TREX_COL_SKY_TOP (lv_color_make(0x4F, 0x9E, 0xD6)) // Sky gradient (zenith)
#define TREX_COL_SKY_BOTTOM (lv_color_make(0xCD, 0xEA, 0xF5)) // Sky gradient (horizon)
#define TREX_COL_GROUND (lv_color_make(0xE4, 0xC8, 0x90)) // Sandy desert floor
#define TREX_COL_GROUND_DARK (lv_color_make(0xBE, 0x9A, 0x5A)) // Ground edge, dashes, pebbles
#define TREX_COL_GROUND_LIGHT (lv_color_make(0xF3, 0xDE, 0xB0)) // Sand speckle highlights
#define TREX_COL_DINO (lv_color_make(0x2E, 0x7D, 0x1B)) // Dark green T-Rex (cactus shade)
#define TREX_COL_CACTUS (lv_color_make(0x2E, 0x7D, 0x1B)) // Cactus green
#define TREX_COL_BIRD (lv_color_make(0x55, 0x55, 0x55)) // Pterodactyl slate gray
#define TREX_COL_HUD (lv_color_white()) // High-score readout text

// Obstacle types
typedef enum {
    TREX_OBST_CACTUS_SMALL = 0,
    TREX_OBST_CACTUS_LARGE,
    TREX_OBST_CACTUS_PAIR,
    TREX_OBST_BIRD_LOW, // Bottom on ground: jump over
    TREX_OBST_BIRD_HIGH, // Head height: duck under
} trex_obst_type_t;

typedef struct {
    int type; // trex_obst_type_t
    int32_t x_q4; // Left edge (Q4 subpixels)
    int y; // Top edge in canvas px
    int w, h; // Collision box size in px
} trex_obstacle_t;

/* Sprite bitmaps: one uint32_t per row, bit (w-1-x) = pixel at column x */

// Dino running, frame A (20x22): left leg tucked, right leg down
static const uint32_t trex_dino_run_a[TREX_DINO_H] = {
    0b00000000001111111110,
    0b00000000001011111110, // Eye
    0b00000000001111111110,
    0b00000000001111111110,
    0b00000000001111100000, // Mouth
    0b00000000001111111100,
    0b00000000011100000000, // Neck
    0b10000000111100000000, // Tail tip
    0b10000001111100000000,
    0b11000011111110000000,
    0b11000111111111100000, // Arm
    0b11101111111110000000,
    0b11111111111110000000,
    0b01111111111110000000,
    0b00111111111100000000,
    0b00011111111000000000,
    0b00001111111000000000,
    0b00000110111000000000, // Legs split
    0b00001110011000000000,
    0b00000000010000000000,
    0b00000000010000000000,
    0b00000000011000000000,
};

// Dino running, frame B (20x22): right leg tucked, left leg down
static const uint32_t trex_dino_run_b[TREX_DINO_H] = {
    0b00000000001111111110,
    0b00000000001011111110, // Eye
    0b00000000001111111110,
    0b00000000001111111110,
    0b00000000001111100000, // Mouth
    0b00000000001111111100,
    0b00000000011100000000, // Neck
    0b10000000111100000000, // Tail tip
    0b10000001111100000000,
    0b11000011111110000000,
    0b11000111111111100000, // Arm
    0b11101111111110000000,
    0b11111111111110000000,
    0b01111111111110000000,
    0b00111111111100000000,
    0b00011111111000000000,
    0b00001111111000000000,
    0b00000110111000000000, // Legs split
    0b00000110011100000000,
    0b00000100000000000000,
    0b00000100000000000000,
    0b00000110000000000000,
};

// Dino ducking, frame A (26x14)
static const uint32_t trex_dino_duck_a[TREX_DUCK_H] = {
    0b00000000000000000011111111,
    0b00000000000000000010111111, // Eye
    0b00000000000000000011111111,
    0b00000000000000000011111000, // Mouth
    0b11000000000000000111111000, // Tail tip
    0b11100001111111111111110000,
    0b11111111111111111111110000,
    0b01111111111111111111110000,
    0b00111111111111111111100000,
    0b00011111111111111111000000,
    0b00001111110011111100000000, // Legs split
    0b00000110000001100000000000,
    0b00000100000001000000000000,
    0b00000110000001100000000000,
};

// Dino ducking, frame B (26x14)
static const uint32_t trex_dino_duck_b[TREX_DUCK_H] = {
    0b00000000000000000011111111,
    0b00000000000000000010111111, // Eye
    0b00000000000000000011111111,
    0b00000000000000000011111000, // Mouth
    0b11000000000000000111111000, // Tail tip
    0b11100001111111111111110000,
    0b11111111111111111111110000,
    0b01111111111111111111110000,
    0b00111111111111111111100000,
    0b00011111111111111111000000,
    0b00001111110011111100000000, // Legs split
    0b00000110000001100000000000,
    0b00000010000000100000000000,
    0b00000011000000110000000000,
};

// Small cactus (8x16)
static const uint32_t trex_cactus_small[16] = {
    0b00011000,
    0b00011000,
    0b00011011,
    0b00011011,
    0b11011011,
    0b11011011,
    0b11011011,
    0b11011110,
    0b01111000,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
    0b00011000,
};

// Large cactus (11x22)
static const uint32_t trex_cactus_large[22] = {
    0b00001110000,
    0b00001110000,
    0b00001110000,
    0b00001110011,
    0b00001110011,
    0b11001110011,
    0b11001110011,
    0b11001110011,
    0b11001110111,
    0b11001111110,
    0b11001110000,
    0b11101110000,
    0b01111110000,
    0b00001110000,
    0b00001110000,
    0b00001110000,
    0b00001110000,
    0b00001110000,
    0b00001110000,
    0b00001110000,
    0b00001110000,
    0b00001110000,
};

// Pterodactyl, frame A (18x12): wing up
static const uint32_t trex_bird_a[12] = {
    0b000000000011000000,
    0b000000000011100000,
    0b000000000011110000,
    0b000000000011110000,
    0b001100000011111000, // Head crest
    0b011110001111111111,
    0b111111111111111111, // Beak + body
    0b011111111111110000,
    0b000111111110000000,
    0b000000000000000000,
    0b000000000000000000,
    0b000000000000000000,
};

// Pterodactyl, frame B (18x12): wing down
static const uint32_t trex_bird_b[12] = {
    0b000000000000000000,
    0b000000000000000000,
    0b000000000000000000,
    0b001100000000000000, // Head crest
    0b011110001111111111,
    0b111111111111111111, // Beak + body
    0b011111111111110000,
    0b000111111110000000,
    0b000000000011110000,
    0b000000000011110000,
    0b000000000011100000,
    0b000000000011000000,
};

// Cloud sprite (18x4): soft puff drawn in white, drifts as slow parallax
#define TREX_CLOUD_W 18
#define TREX_CLOUD_H 4
static const uint32_t trex_cloud[TREX_CLOUD_H] = {
    0b000000111111000000,
    0b000111111111111000,
    0b011111111111111110,
    0b111111111111111111,
};

// Game state (PSRAM: only touched at the 20 FPS tick, never from ISRs)
POLYCAST5_USE_PSRAM_BSS static uint32_t trex_score;
POLYCAST5_USE_PSRAM_BSS static uint32_t trex_high_score;
POLYCAST5_USE_PSRAM_BSS static uint32_t trex_dist_q4; // Distance travelled (Q4 px)
POLYCAST5_USE_PSRAM_BSS static int32_t trex_dino_h_q4; // Dino feet height above ground (Q4 px, 0 = grounded)
POLYCAST5_USE_PSRAM_BSS static int32_t trex_dino_vel_q4; // Vertical velocity (Q4 px/frame, positive = rising)
POLYCAST5_USE_PSRAM_BSS static bool trex_ducking;
POLYCAST5_USE_PSRAM_BSS static TickType_t trex_duck_last; // Last tick a down press was seen
POLYCAST5_USE_PSRAM_BSS static bool trex_jump_pending; // Buffered jump, consumed when grounded
POLYCAST5_USE_PSRAM_BSS static uint32_t trex_frame_count;
POLYCAST5_USE_PSRAM_BSS static bool trex_game_over;
POLYCAST5_USE_PSRAM_BSS static bool trex_game_over_handled; // High score + overlay done once
POLYCAST5_USE_PSRAM_BSS static TickType_t trex_game_over_tick; // When the game over overlay appeared
POLYCAST5_USE_PSRAM_BSS static bool trex_init;
POLYCAST5_USE_PSRAM_BSS static int trex_next_gap; // Empty px required before next spawn
POLYCAST5_USE_PSRAM_BSS static trex_obstacle_t trex_obstacles[TREX_MAX_OBSTACLES];
POLYCAST5_USE_PSRAM_BSS static int trex_obstacle_count;
POLYCAST5_USE_PSRAM_BSS static uint32_t trex_label_score; // Last score rendered on the label

// LVGL elements
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *trex_canvas;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *trex_score_label;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *trex_game_over_label;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *trex_game_over_stroke[8]; // 8-way black text stroke behind the overlay
POLYCAST5_USE_PSRAM_BSS static lv_timer_t *trex_timer;

POLYCAST5_USE_PSRAM_BSS static void *trex_canvas_pixels; // Raw pixel buffer in PSRAM

// Save score as high score
static void trex_high_score_nvs_save(uint32_t score)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(TREX_HIGH_SCORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trex_high_score_nvs_save nvs_open failed");
        return; // Handle not open - nothing to close
    }

    // Store high score as a uint32
    err = nvs_set_u32(h, HIGH_SCORE_KEY, score);
    if (err == ESP_OK) {
        // Commit to flash
        err = nvs_commit(h);

#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Saved T-Rex high score: %" PRIu32, score);
#endif
    } else {
        ESP_LOGE(TAG, "Failed to trex_high_score_nvs_save nvs_set_u32: %" PRIu32, score);
    }

    // Close NVS
    nvs_close(h);
}

// Load T-Rex high score
static uint32_t trex_high_score_nvs_load(void)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(TREX_HIGH_SCORE_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "trex_high_score_nvs_load NS DNE");
#endif
        return 0; // Handle not open - no high score saved yet
    }

    // Get the uint32
    uint32_t score = 0;
    err = nvs_get_u32(h, HIGH_SCORE_KEY, &score);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "Failed trex_high_score_nvs_load nvs_get_u32");
#endif
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Loaded T-Rex high score: %" PRIu32, score);
#endif
    }

    // Close NVS
    nvs_close(h);

    return score;
}

// Helper: Draw a packed 1bpp sprite in the given color, clipped to the canvas.
// Writes straight into the RGB565 buffer (stride == TREX_CANVAS_W, since the
// buffer is allocated W*H*2) to avoid lv_canvas_set_px's per-pixel overhead
static void trex_draw_sprite(int x, int y, const uint32_t *rows, int w, int h, lv_color_t color)
{
    uint16_t *base = (uint16_t *)trex_canvas_pixels;
    uint16_t c = lv_color_to_u16(color);
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= TREX_CANVAS_H) {
            continue;
        }

        uint32_t bits = rows[row];
        if (!bits) {
            continue; // Empty row
        }

        uint16_t *prow = base + py * TREX_CANVAS_W;
        for (int col = 0; col < w; col++) {
            if (bits & (1UL << (w - 1 - col))) { // Bit (w-1-col) = pixel at column col
                int px = x + col;
                if (px >= 0 && px < TREX_CANVAS_W) {
                    prow[px] = c;
                }
            }
        }
    }
}

// Helper: Fill a solid rectangle in the given color, clipped to the canvas
// (direct RGB565 writes, same fast path as trex_draw_sprite)
static void trex_fill_rect(int x, int y, int w, int h, lv_color_t color)
{
    uint16_t *base = (uint16_t *)trex_canvas_pixels;
    uint16_t c = lv_color_to_u16(color);
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > TREX_CANVAS_W) ? TREX_CANVAS_W : (x + w);
    int y1 = (y + h > TREX_CANVAS_H) ? TREX_CANVAS_H : (y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = base + yy * TREX_CANVAS_W;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = c;
        }
    }
}

// Helper: Reset all run state for a fresh game (init and restart)
static void trex_reset_run(void)
{
    trex_score = 0;
    trex_dist_q4 = 0;
    trex_dino_h_q4 = 0;
    trex_dino_vel_q4 = 0;
    trex_ducking = false;
    trex_duck_last = xTaskGetTickCount() - pdMS_TO_TICKS(TREX_DUCK_LINGER_MS) - 1; // Expired
    trex_jump_pending = false;
    trex_frame_count = 0;
    trex_obstacle_count = 0;
    trex_next_gap = TREX_GAP_BASE + 50 + (esp_random() % TREX_GAP_RAND);
    trex_game_over = false;
    trex_game_over_handled = false;
    trex_label_score = UINT32_MAX; // Force label refresh on first frame
}

// Helper: Spawn a new obstacle at the right edge
static void trex_spawn_obstacle(uint32_t speed_q4)
{
    if (trex_obstacle_count >= TREX_MAX_OBSTACLES) {
        return;
    }

    trex_obstacle_t *o = &trex_obstacles[trex_obstacle_count];
    uint32_t r = esp_random();

    // 25% birds once unlocked by score, else a cactus variant
    if (trex_score >= TREX_BIRD_MIN_SCORE && (r % 4) == 0) {
        if (r & 0x100) { // High bird: duck under
            o->type = TREX_OBST_BIRD_HIGH;
            o->y = TREX_GROUND_Y - 28;
        } else { // Low bird: jump over
            o->type = TREX_OBST_BIRD_LOW;
            o->y = TREX_GROUND_Y - 12;
        }
        o->w = 18;
        o->h = 12;
    } else {
        switch (r % 3) {
            case 0:
                o->type = TREX_OBST_CACTUS_SMALL;
                o->w = 8;
                o->h = 16;
                break;
            case 1:
                o->type = TREX_OBST_CACTUS_LARGE;
                o->w = 11;
                o->h = 22;
                break;
            default:
                o->type = TREX_OBST_CACTUS_PAIR; // Two small cacti side by side
                o->w = 17;
                o->h = 16;
                break;
        }
        o->y = TREX_GROUND_Y - o->h;
    }

    o->x_q4 = TREX_CANVAS_W << 4;
    trex_obstacle_count++;

    // Pick the empty gap required before the next spawn (wider when faster)
    trex_next_gap = TREX_GAP_BASE + (speed_q4 >> 4) * 10 + (esp_random() % TREX_GAP_RAND);
}

// Helper: Check dino AABB (inset for fairness) against all obstacles
static bool trex_check_collision(void)
{
    int h_px = trex_dino_h_q4 >> 4;
    int dx0, dx1, dy0, dy1;

    if (trex_ducking) {
        dx0 = TREX_DINO_X + TREX_HITBOX_INSET;
        dx1 = TREX_DINO_X + TREX_DUCK_W - TREX_HITBOX_INSET;
        dy0 = TREX_GROUND_Y - TREX_DUCK_H - h_px + TREX_HITBOX_INSET;
    } else {
        dx0 = TREX_DINO_X + TREX_HITBOX_INSET;
        dx1 = TREX_DINO_X + TREX_DINO_W - TREX_HITBOX_INSET;
        dy0 = TREX_GROUND_Y - TREX_DINO_H - h_px + TREX_HITBOX_INSET;
    }
    dy1 = TREX_GROUND_Y - h_px - TREX_HITBOX_INSET; // Feet

    for (int i = 0; i < trex_obstacle_count; i++) {
        trex_obstacle_t *o = &trex_obstacles[i];
        int ox0 = o->x_q4 >> 4;
        int ox1 = ox0 + o->w;
        int oy0 = o->y;
        int oy1 = o->y + o->h;

        if (dx0 < ox1 && dx1 > ox0 && dy0 < oy1 && dy1 > oy0) {
            return true;
        }
    }

    return false;
}

// Helper: Draw the whole frame (background, ground, dino, obstacles)
// Helper: Draw a sprite tiled for horizontal wrap-around (parallax scenery)
static void trex_draw_parallax(const uint32_t *spr, int w, int h, int base_x, int y, int period, uint32_t scroll, lv_color_t color)
{
    int cx = (int)(((uint32_t)base_x + (uint32_t)period - (scroll % (uint32_t)period)) % (uint32_t)period);
    trex_draw_sprite(cx - period, y, spr, w, h, color);
    trex_draw_sprite(cx, y, spr, w, h, color);
    trex_draw_sprite(cx + period, y, spr, w, h, color);
}

static void trex_draw_frame(void)
{
    // The full-canvas fills below issue many lv_canvas_set_px calls, each of
    // which would invalidate the canvas. Suppress invalidation for the whole
    // frame and trigger a single refresh at the end (same as the Flappy frame)
    lv_display_t *disp = lv_obj_get_display(trex_canvas);
    lv_display_enable_invalidation(disp, false);

    uint32_t off = trex_dist_q4 >> 4;

    // Sky: vertical gradient from a deeper blue at the top to a pale horizon
    for (int y = 0; y < TREX_GROUND_Y; y++) {
        uint8_t mix = (uint8_t)(255 - y * 255 / TREX_GROUND_Y);
        trex_fill_rect(0, y, TREX_CANVAS_W, 1, lv_color_mix(TREX_COL_SKY_TOP, TREX_COL_SKY_BOTTOM, mix));
    }

    // Slow-drifting clouds (parallax behind the action)
    uint32_t cloud_off = off / 3;
    trex_draw_parallax(trex_cloud, TREX_CLOUD_W, TREX_CLOUD_H, 20, 10, TREX_CANVAS_W + 80, cloud_off, lv_color_white());
    trex_draw_parallax(trex_cloud, TREX_CLOUD_W, TREX_CLOUD_H, 120, 26, TREX_CANVAS_W + 80, cloud_off, lv_color_white());
    trex_draw_parallax(trex_cloud, TREX_CLOUD_W, TREX_CLOUD_H, 210, 16, TREX_CANVAS_W + 80, cloud_off, lv_color_white());

    // Ground: sandy band, a packed dark surface, a scrolling motion dash, and
    // scattered pebbles/specks for texture (all scroll with the world)
    trex_fill_rect(0, TREX_GROUND_Y, TREX_CANVAS_W, TREX_CANVAS_H - TREX_GROUND_Y, TREX_COL_GROUND);
    trex_fill_rect(0, TREX_GROUND_Y, TREX_CANVAS_W, 1, TREX_COL_GROUND_DARK);
    for (int x = 0; x < TREX_CANVAS_W; x++) {
        if (((x + off) & 7) < 6) { // 6 px dash, 2 px gap
            lv_canvas_set_px(trex_canvas, x, TREX_GROUND_Y + 2, TREX_COL_GROUND_DARK, LV_OPA_COVER);
        }
    }
    int sand_top = TREX_GROUND_Y + 4;
    int sand_h = TREX_CANVAS_H - sand_top;
    for (int x = 0; x < TREX_CANVAS_W; x++) {
        uint32_t hsh = ((uint32_t)x + off) * 2654435761u; // Stable per world-column
        if ((hsh >> 28) == 0) { // ~1/16 columns: a 2 px dark pebble
            int py = sand_top + (int)((hsh >> 8) % (uint32_t)sand_h);
            lv_canvas_set_px(trex_canvas, x, py, TREX_COL_GROUND_DARK, LV_OPA_COVER);
            if (x + 1 < TREX_CANVAS_W) {
                lv_canvas_set_px(trex_canvas, x + 1, py, TREX_COL_GROUND_DARK, LV_OPA_COVER);
            }
        } else if ((hsh >> 28) == 1) { // ~1/16 columns: a light speck
            int py = sand_top + (int)((hsh >> 12) % (uint32_t)sand_h);
            lv_canvas_set_px(trex_canvas, x, py, TREX_COL_GROUND_LIGHT, LV_OPA_COVER);
        }
    }

    // Dino (legs animate only on the ground)
    int h_px = trex_dino_h_q4 >> 4;
    if (trex_ducking) {
        const uint32_t *frame = ((trex_frame_count / 3) & 1) ? trex_dino_duck_b : trex_dino_duck_a;
        trex_draw_sprite(TREX_DINO_X, TREX_GROUND_Y - TREX_DUCK_H - h_px, frame, TREX_DUCK_W, TREX_DUCK_H, TREX_COL_DINO);
    } else {
        const uint32_t *frame = (h_px > 0) ? trex_dino_run_a
                : (((trex_frame_count / 3) & 1) ? trex_dino_run_b : trex_dino_run_a);
        trex_draw_sprite(TREX_DINO_X, TREX_GROUND_Y - TREX_DINO_H - h_px, frame, TREX_DINO_W, TREX_DINO_H, TREX_COL_DINO);
    }

    // Obstacles
    for (int i = 0; i < trex_obstacle_count; i++) {
        trex_obstacle_t *o = &trex_obstacles[i];
        int ox = o->x_q4 >> 4;

        switch (o->type) {
            case TREX_OBST_CACTUS_SMALL:
                trex_draw_sprite(ox, o->y, trex_cactus_small, 8, 16, TREX_COL_CACTUS);
                break;
            case TREX_OBST_CACTUS_LARGE:
                trex_draw_sprite(ox, o->y, trex_cactus_large, 11, 22, TREX_COL_CACTUS);
                break;
            case TREX_OBST_CACTUS_PAIR:
                trex_draw_sprite(ox, o->y, trex_cactus_small, 8, 16, TREX_COL_CACTUS);
                trex_draw_sprite(ox + 9, o->y, trex_cactus_small, 8, 16, TREX_COL_CACTUS);
                break;
            default: { // Birds
                const uint32_t *frame = ((trex_frame_count / 4) & 1) ? trex_bird_b : trex_bird_a;
                trex_draw_sprite(ox, o->y, frame, 18, 12, TREX_COL_BIRD);
                break;
            }
        }
    }

    // White frame around the play area
    trex_fill_rect(0, 0, TREX_CANVAS_W, 1, lv_color_white());
    trex_fill_rect(0, TREX_CANVAS_H - 1, TREX_CANVAS_W, 1, lv_color_white());
    trex_fill_rect(0, 0, 1, TREX_CANVAS_H, lv_color_white());
    trex_fill_rect(TREX_CANVAS_W - 1, 0, 1, TREX_CANVAS_H, lv_color_white());

    lv_display_enable_invalidation(disp, true);
    lv_obj_invalidate(trex_canvas); // Refresh
}

// Helper: Delete the timer, LVGL objects and PSRAM buffer (all exit paths)
static void trex_cleanup(void)
{
    // Stop the frame timer before deleting the objects it draws to
    if (trex_timer) {
        lv_timer_del(trex_timer);
        trex_timer = NULL;
    }

    lv_obj_delete(trex_canvas);
    lv_obj_delete(trex_score_label);
    game_over_overlay_delete(trex_game_over_label, trex_game_over_stroke);
    heap_caps_free(trex_canvas_pixels); // Free PSRAM

    trex_canvas = trex_score_label = trex_game_over_label = NULL;
    trex_canvas_pixels = NULL;
    trex_init = false;
}

// 20 FPS game tick: physics, spawning, collision, rendering
static void trex_game_timer_cb(lv_timer_t *t)
{
    // Idle while not running or frozen on the game over frame
    if (!trex_init || trex_game_over) {
        return;
    }

    trex_frame_count++;

    TickType_t now = xTaskGetTickCount();

    // Sample gameplay buttons directly at the 50ms tick: waiting for the
    // 200ms page-handler poll adds up to 200ms of input lag (lcd_task only
    // misses these takes, so it must not auto-sleep on GAMES_TREX_PAGE)
    if (xUpButtonSemaphore && xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
        trex_jump_pending = true;
    }
    if (xSelectButtonSemaphore && xSemaphoreTake(xSelectButtonSemaphore, 0) == pdTRUE) {
        trex_jump_pending = true;
    }
    if (xDownButtonSemaphore && xSemaphoreTake(xDownButtonSemaphore, 0) == pdTRUE) {
        if (trex_dino_h_q4 > 0) {
            trex_dino_vel_q4 -= TREX_FAST_DROP_Q4; // Fast drop while airborne
        } else {
            trex_duck_last = now; // Duck
        }
    }

    // Duck only while grounded and a down press was seen recently
    trex_ducking = (trex_dino_h_q4 == 0) && (now - trex_duck_last < pdMS_TO_TICKS(TREX_DUCK_LINGER_MS));

    // Consume buffered jump when grounded (cancels duck)
    if (trex_jump_pending && trex_dino_h_q4 == 0) {
        trex_dino_vel_q4 = TREX_JUMP_V0_Q4;
        trex_ducking = false;
        trex_duck_last = now - pdMS_TO_TICKS(TREX_DUCK_LINGER_MS) - 1; // Expire duck
    }
    trex_jump_pending = false;

    // Gravity integration while airborne
    if (trex_dino_vel_q4 != 0 || trex_dino_h_q4 > 0) {
        trex_dino_h_q4 += trex_dino_vel_q4;
        trex_dino_vel_q4 -= TREX_GRAVITY_Q4;

        if (trex_dino_h_q4 <= 0) { // Landed
            trex_dino_h_q4 = 0;
            trex_dino_vel_q4 = 0;
        }
    }

    // Scroll speed ramps with score (difficulty), clamped to max
    uint32_t speed_q4 = TREX_SPEED_BASE_Q4 + trex_score / 8;
    if (speed_q4 > TREX_SPEED_MAX_Q4) {
        speed_q4 = TREX_SPEED_MAX_Q4;
    }

    // Distance and score (1 point per 10 px travelled)
    trex_dist_q4 += speed_q4;
    trex_score = trex_dist_q4 / 160;

    // Advance obstacles, dropping those fully off the left edge
    int alive = 0;
    for (int i = 0; i < trex_obstacle_count; i++) {
        trex_obstacles[i].x_q4 -= (int32_t)speed_q4;
        if ((trex_obstacles[i].x_q4 >> 4) + trex_obstacles[i].w > 0) {
            trex_obstacles[alive++] = trex_obstacles[i];
        }
    }
    trex_obstacle_count = alive;

    // Spawn when the gap to the right edge is large enough
    if (trex_obstacle_count == 0) {
        if (trex_dist_q4 > (TREX_FIRST_SPAWN_PX << 4)) { // Grace period at run start
            trex_spawn_obstacle(speed_q4);
        }
    } else {
        trex_obstacle_t *last = &trex_obstacles[trex_obstacle_count - 1];
        if (TREX_CANVAS_W - ((last->x_q4 >> 4) + last->w) >= trex_next_gap) {
            trex_spawn_obstacle(speed_q4);
        }
    }

    // Collision ends the run (frame below still drawn to show the crash)
    if (trex_check_collision()) {
        trex_game_over = true;
    }

    trex_draw_frame();

    // Refresh score label only when the value changes
    if (trex_score != trex_label_score) {
        trex_label_score = trex_score;
        char buf[32];
        snprintf(buf, sizeof(buf), "HI %05" PRIu32 " %05" PRIu32, trex_high_score, trex_score);
        lv_label_set_text(trex_score_label, buf);
    }
}

void lcd_games_trex_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu)
{
    static lv_draw_buf_t canvas_buf; // Metadata struct (small, internal SRAM)

    if (!trex_init) {
        // Load persisted high score once per session, reset run state
        trex_high_score = trex_high_score_nvs_load();
        trex_reset_run();

        // Allocate pixel buffer in PSRAM
        size_t buf_size = TREX_CANVAS_W * TREX_CANVAS_H * 2; // RGB565: 2 bytes/pixel
        trex_canvas_pixels = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!trex_canvas_pixels) {
            ESP_LOGE(TAG, "Failed to alloc PSRAM for T-Rex canvas");

            // Fallback or exit to menu
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = GAMES_PAGE;
            return;
        }

        // Init draw buf metadata (small struct in internal SRAM)
        lv_draw_buf_init(&canvas_buf, TREX_CANVAS_W, TREX_CANVAS_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, trex_canvas_pixels, buf_size);

        // Create canvas
        trex_canvas = lv_canvas_create(ACTIVE_SCR);
        lv_canvas_set_draw_buf(trex_canvas, &canvas_buf);
        lv_obj_set_size(trex_canvas, TREX_CANVAS_W, TREX_CANVAS_H);
        lv_obj_align(trex_canvas, LV_ALIGN_CENTER, 0, 4);

        // Score label: "HI <best> <current>"
        trex_score_label = lv_label_create(ACTIVE_SCR);
        char buf[32];
        snprintf(buf, sizeof(buf), "HI %05" PRIu32 " %05" PRIu32, trex_high_score, trex_score);
        lv_label_set_text(trex_score_label, buf);
        lv_obj_set_style_text_color(trex_score_label, TREX_COL_HUD, 0);
        lv_obj_align(trex_score_label, LV_ALIGN_TOP_MID, 0, 2);

        // Game over overlay: centered white text with an 8-way black stroke
        game_over_overlay_create(&trex_game_over_label, trex_game_over_stroke);

        trex_draw_frame();

        // Physics + render at 20 FPS (page handler only runs at the 200ms button poll)
        trex_timer = lv_timer_create(trex_game_timer_cb, TREX_FRAME_MS, NULL);

        trex_init = true;
    }

    // Handle game over
    if (trex_game_over) {
        // One-time: high score check + overlay text
        if (!trex_game_over_handled) {
            // If new high score, save
            if (trex_score > trex_high_score) {
                trex_high_score = trex_score;
                trex_high_score_nvs_save(trex_high_score);
            }

            char buf[64];
            snprintf(buf, sizeof(buf), "Game Over!\nScore: %" PRIu32 "\nHigh Score: %" PRIu32, trex_score, trex_high_score);
            game_over_overlay_show(trex_game_over_label, trex_game_over_stroke, buf);

            trex_game_over_handled = true;
            trex_game_over_tick = xTaskGetTickCount();
        }

        // Brief grace so a button still held from the crash is not consumed
        if (xTaskGetTickCount() - trex_game_over_tick < pdMS_TO_TICKS(600)) {
            return;
        }

        if (ui_btns->up_btn || ui_btns->select_btn) { // Restart (Chrome style)
            game_over_overlay_hide(trex_game_over_label, trex_game_over_stroke);
            trex_reset_run();
        } else if (ui_btns->down_btn || ui_btns->left_btn || ui_btns->right_btn || ui_btns->home_btn) { // Exit to menu
            trex_cleanup();

            // Back to games menu
            lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

            // Show arrows
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

            ui_menu->page = GAMES_PAGE;
        } else if (ui_btns->pwr_btn) { // Sleep
            trex_cleanup();

            lcd_transition_back(false, ui_menu); // False = sleep
        }
        return;
    }

    /* Input fallback: the 20 FPS timer samples the semaphores first, but the
       main loop's 200ms poll can win the race, in which case presses arrive
       here instead. Exit buttons (home/pwr) are only ever seen here. */
    if (ui_btns->up_btn || ui_btns->select_btn) { // Jump (buffered until grounded)
        trex_jump_pending = true;
    } else if (ui_btns->down_btn) { // Duck (grounded) or fast drop (airborne)
        if (trex_dino_h_q4 > 0) {
            trex_dino_vel_q4 -= TREX_FAST_DROP_Q4;
        } else {
            trex_duck_last = xTaskGetTickCount();
        }
    } else if (ui_btns->home_btn) { // Exit to menu
        trex_cleanup();

        // Show games menu
        lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Show arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = GAMES_PAGE;
    } else if (ui_btns->pwr_btn) { // Sleep
        trex_cleanup();

        lcd_transition_back(false, ui_menu); // False = sleep
    }
}

/* ========== Flappy Bird Implementation ========== */

#define FLAPPY_HIGH_SCORE_NS "flappy"

// Macros for game config
#define FLAPPY_CANVAS_W 220 // Canvas width in px
#define FLAPPY_CANVAS_H 90 // Canvas height in px
#define FLAPPY_FRAME_MS 50 // Physics/render tick (20 FPS via lv_timer)
#define FLAPPY_GROUND_Y 86 // Ground line y inside canvas
#define FLAPPY_BIRD_X 40 // Fixed bird left edge
#define FLAPPY_BIRD_W 16 // Bird sprite width
#define FLAPPY_BIRD_H 12 // Bird sprite height
#define FLAPPY_HITBOX_INSET 2 // Bird collision box inset for fairness
#define FLAPPY_GRAVITY_Q4 12 // Gravity per frame (Q4 px/frame^2, positive = down)
#define FLAPPY_FLAP_V0_Q4 (-88) // Flap impulse velocity (Q4 px/frame, negative = up)
#define FLAPPY_MAX_FALL_Q4 120 // Terminal fall speed (Q4 px/frame)
#define FLAPPY_MAX_PIPES 4 // Max simultaneous pipes
#define FLAPPY_PIPE_W 14 // Pipe column width (collision width)
#define FLAPPY_LIP_H 5 // Pipe rim height at the gap opening
#define FLAPPY_GAP_BASE 40 // Starting vertical gap height (px)
#define FLAPPY_GAP_MIN 26 // Min gap height when hardest (px)
#define FLAPPY_GAP_MARGIN 8 // Min gap distance from ceiling/ground (px)
#define FLAPPY_SPEED_BASE_Q4 40 // Starting scroll speed (Q4 = 2.5 px/frame)
#define FLAPPY_SPEED_MAX_Q4 110 // Max scroll speed (Q4 ≈ 6.9 px/frame)
#define FLAPPY_SPACING_BASE 120 // Starting horizontal spacing between pipes (px)
#define FLAPPY_SPACING_MIN 80 // Min spacing when hardest (px)
#define FLAPPY_FIRST_SPAWN_PX 90 // Grace distance before the first pipe

// Game colors (classic Flappy Bird palette)
#define FLAPPY_COL_SKY_TOP (lv_color_make(0x66, 0xB5, 0xE8)) // Sky gradient (zenith)
#define FLAPPY_COL_SKY_BOTTOM (lv_color_make(0xC6, 0xEA, 0xF8)) // Sky gradient (horizon)
#define FLAPPY_COL_HILL (lv_color_make(0x8F, 0xC9, 0x6B)) // Distant hills (mid green)
#define FLAPPY_COL_HILL_LIGHT (lv_color_make(0xB5, 0xDE, 0x8E)) // Sunlit hill crown
#define FLAPPY_COL_HILL_DARK (lv_color_make(0x5E, 0x9E, 0x3C)) // Shaded hill base
#define FLAPPY_COL_GROUND (lv_color_make(0xDE, 0xD8, 0x95)) // Tan ground band
#define FLAPPY_COL_GRASS (lv_color_make(0x5E, 0xC4, 0x28)) // Grass strip on top of the ground
#define FLAPPY_COL_GRASS_DARK (lv_color_make(0x46, 0x9E, 0x22)) // Grass shadow line
#define FLAPPY_COL_GROUND_DARK (lv_color_make(0xB8, 0x9A, 0x4E)) // Ground diagonal stripes
#define FLAPPY_COL_PIPE (lv_color_make(0x5E, 0xC4, 0x28)) // Pipe body green
#define FLAPPY_COL_PIPE_LIGHT (lv_color_make(0x9B, 0xE0, 0x55)) // Pipe highlight
#define FLAPPY_COL_PIPE_DARK (lv_color_make(0x3A, 0x86, 0x1E)) // Pipe outline/shadow
#define FLAPPY_COL_BIRD (lv_color_make(0xFF, 0xC4, 0x00)) // Bird body (gold)
#define FLAPPY_COL_BEAK (lv_color_make(0xF0, 0x60, 0x0E)) // Bird beak (orange-red)
#define FLAPPY_COL_WING (lv_color_make(0xE0, 0x93, 0x0F)) // Bird wing (darker gold)
#define FLAPPY_COL_OUTLINE (lv_color_make(0x2E, 0x22, 0x12)) // Bird dark outline
#define FLAPPY_COL_TEXT (lv_color_white()) // HUD/score text

typedef struct {
    int32_t x_q4; // Left edge (Q4 subpixels)
    int gap_y; // Top of the gap in canvas px (top pipe spans 0..gap_y)
    int gap_h; // Gap height in px
    bool scored; // Whether the bird has already passed this pipe
} flappy_pipe_t;

/* Sprite bitmaps: one uint32_t per row, bit (w-1-x) = pixel at column x */

/* Bird (16x12): hand-drawn layers composited in order — dark outline, gold
   body, white (big eye + belly), black pupil, orange beak, then the wing.
   The layers are mutually exclusive (except pupil over the eye and the wing
   over the body), so the baked outline stays crisp instead of a dilated halo. */

// Dark outline (silhouette border + eye underline)
static const uint32_t flappy_bird_outline[FLAPPY_BIRD_H] = {
    0b0001111110000000,
    0b0010000001100000,
    0b0100000000010000,
    0b1000000000001000,
    0b1000000000001000,
    0b1000000000010000,
    0b1000000111100000,
    0b1000000000110000,
    0b0100000000110000,
    0b0011000011000000,
    0b0000111100000000,
    0b0000000000000000,
};

// Gold body fill
static const uint32_t flappy_bird_body[FLAPPY_BIRD_H] = {
    0b0000000000000000,
    0b0001111110000000,
    0b0011111000000000,
    0b0111110000000000,
    0b0111110000000000,
    0b0111110000000000,
    0b0111111000000000,
    0b0100011111000000,
    0b0000000011000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};

// White: big eye sclera (upper front) + belly (lower)
static const uint32_t flappy_bird_white[FLAPPY_BIRD_H] = {
    0b0000000000000000,
    0b0000000000000000,
    0b0000000111100000,
    0b0000001111010000,
    0b0000001110010000,
    0b0000001111100000,
    0b0000000000000000,
    0b0011100000000000,
    0b0011111100000000,
    0b0000111100000000,
    0b0000000000000000,
    0b0000000000000000,
};

// Black pupil (centre of the eye)
static const uint32_t flappy_bird_pupil[FLAPPY_BIRD_H] = {
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000100000,
    0b0000000001100000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};

// Orange-red beak (front)
static const uint32_t flappy_bird_beak[FLAPPY_BIRD_H] = {
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000001100,
    0b0000000000011110,
    0b0000000000001100,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};

// Wing, frame A (raised) and frame B (lowered) — a darker-gold patch on the body
static const uint32_t flappy_bird_wing_a[FLAPPY_BIRD_H] = {
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0011100000000000,
    0b0011110000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};
static const uint32_t flappy_bird_wing_b[FLAPPY_BIRD_H] = {
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0011110000000000,
    0b0011100000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};

// Cloud sprite (20x5): soft puff drawn in white, drifts as slow parallax
#define FLAPPY_CLOUD_W 20
#define FLAPPY_CLOUD_H 5
static const uint32_t flappy_cloud[FLAPPY_CLOUD_H] = {
    0b00000001111110000000,
    0b00000111111111100000,
    0b00011111111111111000,
    0b01111111111111111110,
    0b11111111111111111111,
};

// Bush tile (32x10): rounded hump on a solid base, tiles seamlessly on the horizon
#define FLAPPY_HILL_W 32
#define FLAPPY_HILL_H 10
static const uint32_t flappy_hill[FLAPPY_HILL_H] = {
    0b00000000000001111110000000000000,
    0b00000000000111111111100000000000,
    0b00000000011111111111111000000000,
    0b00000001111111111111111110000000,
    0b00000111111111111111111111100000,
    0b00011111111111111111111111111000,
    0b01111111111111111111111111111110,
    0b11111111111111111111111111111111,
    0b11111111111111111111111111111111,
    0b11111111111111111111111111111111,
};

// Hill shading layers (same silhouette): a lighter sunlit crown on the upper
// rows and a darker shaded base, drawn over the mid-green hill so it isn't flat
static const uint32_t flappy_hill_light[FLAPPY_HILL_H] = {
    0b00000000000001111110000000000000,
    0b00000000000111111111100000000000,
    0b00000000011111111111111000000000,
    0b00000001111111111111111110000000,
    0, 0, 0, 0, 0, 0,
};
static const uint32_t flappy_hill_dark[FLAPPY_HILL_H] = {
    0, 0, 0, 0, 0, 0, 0,
    0b11111111111111111111111111111111,
    0b11111111111111111111111111111111,
    0b11111111111111111111111111111111,
};

// Game state (PSRAM: only touched at the 20 FPS tick, never from ISRs)
POLYCAST5_USE_PSRAM_BSS static uint32_t flappy_score;
POLYCAST5_USE_PSRAM_BSS static uint32_t flappy_high_score;
POLYCAST5_USE_PSRAM_BSS static uint32_t flappy_dist_q4; // Distance scrolled (Q4 px), drives ground + first spawn
POLYCAST5_USE_PSRAM_BSS static int32_t flappy_bird_y_q4; // Bird top edge y (Q4 px, grows downward)
POLYCAST5_USE_PSRAM_BSS static int32_t flappy_bird_vel_q4; // Vertical velocity (Q4 px/frame, positive = falling)
POLYCAST5_USE_PSRAM_BSS static uint32_t flappy_frame_count;
POLYCAST5_USE_PSRAM_BSS static bool flappy_started; // Bird hovers until the first flap
POLYCAST5_USE_PSRAM_BSS static bool flappy_game_over;
POLYCAST5_USE_PSRAM_BSS static bool flappy_game_over_handled; // High score + overlay done once
POLYCAST5_USE_PSRAM_BSS static TickType_t flappy_game_over_tick; // When the game over overlay appeared
POLYCAST5_USE_PSRAM_BSS static bool flappy_init;
POLYCAST5_USE_PSRAM_BSS static flappy_pipe_t flappy_pipes[FLAPPY_MAX_PIPES];
POLYCAST5_USE_PSRAM_BSS static int flappy_pipe_count;
POLYCAST5_USE_PSRAM_BSS static uint32_t flappy_label_score; // Last score rendered on the label

// LVGL elements
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *flappy_canvas;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *flappy_score_label;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *flappy_game_over_label;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *flappy_game_over_stroke[8]; // 8-way black text stroke behind the overlay
POLYCAST5_USE_PSRAM_BSS static lv_timer_t *flappy_timer;

POLYCAST5_USE_PSRAM_BSS static void *flappy_canvas_pixels; // Raw pixel buffer in PSRAM

// Save score as high score
static void flappy_high_score_nvs_save(uint32_t score)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(FLAPPY_HIGH_SCORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flappy_high_score_nvs_save nvs_open failed");
        return; // Handle not open - nothing to close
    }

    // Store high score as a uint32
    err = nvs_set_u32(h, HIGH_SCORE_KEY, score);
    if (err == ESP_OK) {
        // Commit to flash
        err = nvs_commit(h);

#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Saved Flappy high score: %" PRIu32, score);
#endif
    } else {
        ESP_LOGE(TAG, "Failed to flappy_high_score_nvs_save nvs_set_u32: %" PRIu32, score);
    }

    // Close NVS
    nvs_close(h);
}

// Load Flappy high score
static uint32_t flappy_high_score_nvs_load(void)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(FLAPPY_HIGH_SCORE_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "flappy_high_score_nvs_load NS DNE");
#endif
        return 0; // Handle not open - no high score saved yet
    }

    // Get the uint32
    uint32_t score = 0;
    err = nvs_get_u32(h, HIGH_SCORE_KEY, &score);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGE(TAG, "Failed flappy_high_score_nvs_load nvs_get_u32");
#endif
    } else {
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Loaded Flappy high score: %" PRIu32, score);
#endif
    }

    // Close NVS
    nvs_close(h);

    return score;
}

// Helper: Draw a packed 1bpp sprite in the given color, clipped to the canvas.
// Writes straight into the RGB565 buffer (stride == FLAPPY_CANVAS_W, since the
// buffer is allocated W*H*2) to avoid lv_canvas_set_px's per-pixel overhead
static void flappy_draw_sprite(int x, int y, const uint32_t *rows, int w, int h, lv_color_t color)
{
    uint16_t *base = (uint16_t *)flappy_canvas_pixels;
    uint16_t c = lv_color_to_u16(color);
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= FLAPPY_CANVAS_H) {
            continue;
        }

        uint32_t bits = rows[row];
        if (!bits) {
            continue; // Empty row
        }

        uint16_t *prow = base + py * FLAPPY_CANVAS_W;
        for (int col = 0; col < w; col++) {
            if (bits & (1UL << (w - 1 - col))) { // Bit (w-1-col) = pixel at column col
                int px = x + col;
                if (px >= 0 && px < FLAPPY_CANVAS_W) {
                    prow[px] = c;
                }
            }
        }
    }
}

// Helper: Fill a solid rectangle in the given color, clipped to the canvas
// (direct RGB565 writes, same fast path as flappy_draw_sprite)
static void flappy_fill_rect(int x, int y, int w, int h, lv_color_t color)
{
    uint16_t *base = (uint16_t *)flappy_canvas_pixels;
    uint16_t c = lv_color_to_u16(color);
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > FLAPPY_CANVAS_W) ? FLAPPY_CANVAS_W : (x + w);
    int y1 = (y + h > FLAPPY_CANVAS_H) ? FLAPPY_CANVAS_H : (y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = base + yy * FLAPPY_CANVAS_W;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = c;
        }
    }
}

// Helper: Draw a vertical pipe segment with a left highlight and dark edges
static void flappy_fill_shaded_col(int x, int y, int w, int h)
{
    if (h <= 0) {
        return;
    }
    flappy_fill_rect(x, y, w, h, FLAPPY_COL_PIPE); // Body
    flappy_fill_rect(x + 1, y, 2, h, FLAPPY_COL_PIPE_LIGHT); // Highlight near the left
    flappy_fill_rect(x, y, 1, h, FLAPPY_COL_PIPE_DARK); // Left outline
    flappy_fill_rect(x + w - 1, y, 1, h, FLAPPY_COL_PIPE_DARK); // Right shadow
}

// Helper: Draw a pipe cap (wider rim) shaded like the shaft with dark edges
static void flappy_fill_cap(int x, int y, int w, int h)
{
    if (h <= 0) {
        return;
    }
    flappy_fill_shaded_col(x, y, w, h);
    flappy_fill_rect(x, y, w, 1, FLAPPY_COL_PIPE_DARK); // Top edge
    flappy_fill_rect(x, y + h - 1, w, 1, FLAPPY_COL_PIPE_DARK); // Bottom edge
}

// Helper: Draw one pipe (narrow shaft + a wider rim at the gap opening)
static void flappy_draw_pipe(const flappy_pipe_t *p)
{
    int px = p->x_q4 >> 4;
    int shaft_x = px + 2;
    int shaft_w = FLAPPY_PIPE_W - 4;

    int top_h = p->gap_y; // Top pipe spans 0 .. gap_y
    int bot_y = p->gap_y + p->gap_h; // Bottom pipe spans bot_y .. ground
    int bot_h = FLAPPY_GROUND_Y - bot_y;

    // Top pipe: shaft then a rim at the bottom (gap-facing) edge
    if (top_h > 0) {
        int shaft_h = top_h - FLAPPY_LIP_H;
        if (shaft_h > 0) {
            flappy_fill_shaded_col(shaft_x, 0, shaft_w, shaft_h);
            flappy_fill_cap(px, shaft_h, FLAPPY_PIPE_W, FLAPPY_LIP_H);
        } else {
            flappy_fill_cap(px, 0, FLAPPY_PIPE_W, top_h); // Too short for a separate rim
        }
    }

    // Bottom pipe: a rim at the top (gap-facing) edge then the shaft
    if (bot_h > 0) {
        int rim_h = (bot_h < FLAPPY_LIP_H) ? bot_h : FLAPPY_LIP_H;
        flappy_fill_cap(px, bot_y, FLAPPY_PIPE_W, rim_h);
        int shaft_h = bot_h - rim_h;
        if (shaft_h > 0) {
            flappy_fill_shaded_col(shaft_x, bot_y + rim_h, shaft_w, shaft_h);
        }
    }
}

// Helper: Draw a sprite tiled for horizontal wrap-around (parallax scenery)
static void flappy_draw_parallax(const uint32_t *spr, int w, int h, int base_x, int y, int period, uint32_t scroll, lv_color_t color)
{
    int cx = (int)(((uint32_t)base_x + (uint32_t)period - (scroll % (uint32_t)period)) % (uint32_t)period);
    flappy_draw_sprite(cx - period, y, spr, w, h, color);
    flappy_draw_sprite(cx, y, spr, w, h, color);
    flappy_draw_sprite(cx + period, y, spr, w, h, color);
}

// Helper: Tile the bush sprite across the horizon, scrolling with the world
static void flappy_draw_hills(uint32_t scroll)
{
    int y = FLAPPY_GROUND_Y - FLAPPY_HILL_H;
    int start = -(int)(scroll % (uint32_t)FLAPPY_HILL_W);
    for (int x = start; x < FLAPPY_CANVAS_W; x += FLAPPY_HILL_W) {
        flappy_draw_sprite(x, y, flappy_hill, FLAPPY_HILL_W, FLAPPY_HILL_H, FLAPPY_COL_HILL);
        flappy_draw_sprite(x, y, flappy_hill_light, FLAPPY_HILL_W, FLAPPY_HILL_H, FLAPPY_COL_HILL_LIGHT);
        flappy_draw_sprite(x, y, flappy_hill_dark, FLAPPY_HILL_W, FLAPPY_HILL_H, FLAPPY_COL_HILL_DARK);
    }
}

// Helper: Draw the whole frame (background, pipes, ground, bird)
static void flappy_draw_frame(void)
{
    // The full-canvas fills below issue many lv_canvas_set_px calls, each of
    // which would invalidate the canvas. Suppress invalidation for the whole
    // frame and trigger a single refresh at the end
    lv_display_t *disp = lv_obj_get_display(flappy_canvas);
    lv_display_enable_invalidation(disp, false);

    uint32_t off = flappy_dist_q4 >> 4;

    // Sky: vertical gradient from a deeper teal at the top to a lighter horizon
    for (int y = 0; y < FLAPPY_GROUND_Y; y++) {
        uint8_t mix = (uint8_t)(255 - y * 255 / FLAPPY_GROUND_Y);
        flappy_fill_rect(0, y, FLAPPY_CANVAS_W, 1, lv_color_mix(FLAPPY_COL_SKY_TOP, FLAPPY_COL_SKY_BOTTOM, mix));
    }

    // Slow clouds and a row of bushes on the horizon (parallax depth, behind pipes)
    uint32_t cloud_off = off / 3;
    flappy_draw_parallax(flappy_cloud, FLAPPY_CLOUD_W, FLAPPY_CLOUD_H, 30, 12, FLAPPY_CANVAS_W + 80, cloud_off, lv_color_white());
    flappy_draw_parallax(flappy_cloud, FLAPPY_CLOUD_W, FLAPPY_CLOUD_H, 150, 24, FLAPPY_CANVAS_W + 80, cloud_off, lv_color_white());
    flappy_draw_hills(off / 2);

    // Pipes
    for (int i = 0; i < flappy_pipe_count; i++) {
        flappy_draw_pipe(&flappy_pipes[i]);
    }

    // Ground: grass strip with a shadow line, tan base, scrolling diagonal stripes
    flappy_fill_rect(0, FLAPPY_GROUND_Y, FLAPPY_CANVAS_W, 1, FLAPPY_COL_GRASS);
    flappy_fill_rect(0, FLAPPY_GROUND_Y + 1, FLAPPY_CANVAS_W, 1, FLAPPY_COL_GRASS_DARK);
    flappy_fill_rect(0, FLAPPY_GROUND_Y + 2, FLAPPY_CANVAS_W, FLAPPY_CANVAS_H - FLAPPY_GROUND_Y - 2, FLAPPY_COL_GROUND);
    for (int y = FLAPPY_GROUND_Y + 2; y < FLAPPY_CANVAS_H; y++) {
        for (int x = 0; x < FLAPPY_CANVAS_W; x++) {
            if (((uint32_t)x + off + (uint32_t)(y - FLAPPY_GROUND_Y)) % 6 < 2) { // Diagonal stripe
                lv_canvas_set_px(flappy_canvas, x, y, FLAPPY_COL_GROUND_DARK, LV_OPA_COVER);
            }
        }
    }

    // Bird: outline, gold body, white (eye + belly), pupil, beak, flapping wing
    int by = flappy_bird_y_q4 >> 4;
    const uint32_t *wing = ((flappy_frame_count / 3) & 1) ? flappy_bird_wing_b : flappy_bird_wing_a;
    flappy_draw_sprite(FLAPPY_BIRD_X, by, flappy_bird_outline, FLAPPY_BIRD_W, FLAPPY_BIRD_H, FLAPPY_COL_OUTLINE);
    flappy_draw_sprite(FLAPPY_BIRD_X, by, flappy_bird_body, FLAPPY_BIRD_W, FLAPPY_BIRD_H, FLAPPY_COL_BIRD);
    flappy_draw_sprite(FLAPPY_BIRD_X, by, flappy_bird_white, FLAPPY_BIRD_W, FLAPPY_BIRD_H, lv_color_white());
    flappy_draw_sprite(FLAPPY_BIRD_X, by, flappy_bird_pupil, FLAPPY_BIRD_W, FLAPPY_BIRD_H, lv_color_black());
    flappy_draw_sprite(FLAPPY_BIRD_X, by, flappy_bird_beak, FLAPPY_BIRD_W, FLAPPY_BIRD_H, FLAPPY_COL_BEAK);
    flappy_draw_sprite(FLAPPY_BIRD_X, by, wing, FLAPPY_BIRD_W, FLAPPY_BIRD_H, FLAPPY_COL_WING);

    // White frame around the play area
    flappy_fill_rect(0, 0, FLAPPY_CANVAS_W, 1, lv_color_white());
    flappy_fill_rect(0, FLAPPY_CANVAS_H - 1, FLAPPY_CANVAS_W, 1, lv_color_white());
    flappy_fill_rect(0, 0, 1, FLAPPY_CANVAS_H, lv_color_white());
    flappy_fill_rect(FLAPPY_CANVAS_W - 1, 0, 1, FLAPPY_CANVAS_H, lv_color_white());

    // Re-enable invalidation and issue one refresh for the whole canvas
    lv_display_enable_invalidation(disp, true);
    lv_obj_invalidate(flappy_canvas); // Refresh
}

// Helper: Reset all run state for a fresh game (init and restart)
static void flappy_reset_run(void)
{
    flappy_score = 0;
    flappy_dist_q4 = 0;
    flappy_bird_y_q4 = ((FLAPPY_GROUND_Y - FLAPPY_BIRD_H) / 2) << 4; // Centered vertically
    flappy_bird_vel_q4 = 0;
    flappy_frame_count = 0;
    flappy_pipe_count = 0;
    flappy_started = false;
    flappy_game_over = false;
    flappy_game_over_handled = false;
    flappy_label_score = UINT32_MAX; // Force label refresh on first frame
}

// Helper: Spawn a new pipe at the right edge with a score-scaled gap
static void flappy_spawn_pipe(void)
{
    if (flappy_pipe_count >= FLAPPY_MAX_PIPES) {
        return;
    }

    flappy_pipe_t *p = &flappy_pipes[flappy_pipe_count];

    // Gap shrinks with score (harder), clamped to a passable minimum
    int gap_h = FLAPPY_GAP_BASE - (int)(flappy_score / 4);
    if (gap_h < FLAPPY_GAP_MIN) {
        gap_h = FLAPPY_GAP_MIN;
    }

    // Random gap position, kept clear of the ceiling and ground
    int range = FLAPPY_GROUND_Y - gap_h - 2 * FLAPPY_GAP_MARGIN;
    if (range < 1) {
        range = 1;
    }
    p->gap_y = FLAPPY_GAP_MARGIN + (int)(esp_random() % (uint32_t)range);
    p->gap_h = gap_h;
    p->x_q4 = FLAPPY_CANVAS_W << 4;
    p->scored = false;
    flappy_pipe_count++;
}

// Helper: Check the bird AABB (inset for fairness) against pipes and the ground
static bool flappy_check_collision(void)
{
    int by = flappy_bird_y_q4 >> 4;

    // Hit the ground
    if (by + FLAPPY_BIRD_H >= FLAPPY_GROUND_Y) {
        return true;
    }

    int bx0 = FLAPPY_BIRD_X + FLAPPY_HITBOX_INSET;
    int bx1 = FLAPPY_BIRD_X + FLAPPY_BIRD_W - FLAPPY_HITBOX_INSET;
    int by0 = by + FLAPPY_HITBOX_INSET;
    int by1 = by + FLAPPY_BIRD_H - FLAPPY_HITBOX_INSET;

    for (int i = 0; i < flappy_pipe_count; i++) {
        flappy_pipe_t *p = &flappy_pipes[i];
        int px0 = p->x_q4 >> 4;
        int px1 = px0 + FLAPPY_PIPE_W;

        if (bx1 > px0 && bx0 < px1) { // Horizontal overlap with this pipe column
            if (by0 < p->gap_y || by1 > p->gap_y + p->gap_h) { // Above or below the gap
                return true;
            }
        }
    }

    return false;
}

// Helper: Delete the timer, LVGL objects and PSRAM buffer (all exit paths)
static void flappy_cleanup(void)
{
    // Stop the frame timer before deleting the objects it draws to
    if (flappy_timer) {
        lv_timer_del(flappy_timer);
        flappy_timer = NULL;
    }

    lv_obj_delete(flappy_canvas);
    lv_obj_delete(flappy_score_label);
    game_over_overlay_delete(flappy_game_over_label, flappy_game_over_stroke);
    heap_caps_free(flappy_canvas_pixels); // Free PSRAM

    flappy_canvas = flappy_score_label = flappy_game_over_label = NULL;
    flappy_canvas_pixels = NULL;
    flappy_init = false;
}

// 20 FPS game tick: input, physics, spawning, collision, rendering
static void flappy_game_timer_cb(lv_timer_t *t)
{
    // Idle while not running or frozen on the game over frame
    if (!flappy_init || flappy_game_over) {
        return;
    }

    flappy_frame_count++;

    // Sample the flap buttons directly at the 50ms tick: waiting for the
    // 200ms page-handler poll adds up to 200ms of input lag (lcd_task only
    // misses these takes, so it must not auto-sleep on GAMES_FLAPPY_PAGE)
    bool flap = false;
    if (xUpButtonSemaphore && xSemaphoreTake(xUpButtonSemaphore, 0) == pdTRUE) {
        flap = true;
    }
    if (xSelectButtonSemaphore && xSemaphoreTake(xSelectButtonSemaphore, 0) == pdTRUE) {
        flap = true;
    }
    if (flap) {
        flappy_started = true; // First flap launches the run
        flappy_bird_vel_q4 = FLAPPY_FLAP_V0_Q4;
    }

    // Bird hovers (wings flapping) until the first flap
    if (!flappy_started) {
        flappy_draw_frame();
        return;
    }

    // Difficulty: scroll speed ramps with score, clamped to max
    uint32_t speed_q4 = FLAPPY_SPEED_BASE_Q4 + flappy_score * 2;
    if (speed_q4 > FLAPPY_SPEED_MAX_Q4) {
        speed_q4 = FLAPPY_SPEED_MAX_Q4;
    }

    // Gravity integration, clamped to terminal velocity
    flappy_bird_vel_q4 += FLAPPY_GRAVITY_Q4;
    if (flappy_bird_vel_q4 > FLAPPY_MAX_FALL_Q4) {
        flappy_bird_vel_q4 = FLAPPY_MAX_FALL_Q4;
    }
    flappy_bird_y_q4 += flappy_bird_vel_q4;

    // Clamp at the ceiling (not fatal, just blocks)
    if (flappy_bird_y_q4 < 0) {
        flappy_bird_y_q4 = 0;
        flappy_bird_vel_q4 = 0;
    }

    // Distance drives the scrolling ground and the first-spawn grace
    flappy_dist_q4 += speed_q4;

    // Advance pipes, score those passed, drop those off the left edge
    int alive = 0;
    for (int i = 0; i < flappy_pipe_count; i++) {
        flappy_pipes[i].x_q4 -= (int32_t)speed_q4;

        // Score once the pipe is fully behind the bird
        if (!flappy_pipes[i].scored && (flappy_pipes[i].x_q4 >> 4) + FLAPPY_PIPE_W < FLAPPY_BIRD_X) {
            flappy_pipes[i].scored = true;
            flappy_score++;
        }

        if ((flappy_pipes[i].x_q4 >> 4) + FLAPPY_PIPE_W > 0) {
            flappy_pipes[alive++] = flappy_pipes[i];
        }
    }
    flappy_pipe_count = alive;

    // Spawn when the gap to the right edge is large enough (tighter when faster)
    if (flappy_pipe_count == 0) {
        if (flappy_dist_q4 > (FLAPPY_FIRST_SPAWN_PX << 4)) { // Grace period at run start
            flappy_spawn_pipe();
        }
    } else {
        flappy_pipe_t *last = &flappy_pipes[flappy_pipe_count - 1];
        int spacing = FLAPPY_SPACING_BASE - (int)(flappy_score / 2);
        if (spacing < FLAPPY_SPACING_MIN) {
            spacing = FLAPPY_SPACING_MIN;
        }
        if (FLAPPY_CANVAS_W - ((last->x_q4 >> 4) + FLAPPY_PIPE_W) >= spacing) {
            flappy_spawn_pipe();
        }
    }

    // Collision ends the run (frame below still drawn to show the crash)
    if (flappy_check_collision()) {
        flappy_game_over = true;
    }

    flappy_draw_frame();

    // Refresh score label only when the value changes
    if (flappy_score != flappy_label_score) {
        flappy_label_score = flappy_score;
        char buf[32];
        snprintf(buf, sizeof(buf), "HI %05" PRIu32 " %05" PRIu32, flappy_high_score, flappy_score);
        lv_label_set_text(flappy_score_label, buf);
    }
}

void lcd_games_flappy_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu)
{
    POLYCAST5_USE_PSRAM_BSS static lv_draw_buf_t canvas_buf; // Metadata struct (PSRAM)

    if (!flappy_init) {
        // Load persisted high score once per session, reset run state
        flappy_high_score = flappy_high_score_nvs_load();
        flappy_reset_run();

        // Allocate pixel buffer in PSRAM
        size_t buf_size = FLAPPY_CANVAS_W * FLAPPY_CANVAS_H * 2; // RGB565: 2 bytes/pixel
        flappy_canvas_pixels = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!flappy_canvas_pixels) {
            ESP_LOGE(TAG, "Failed to alloc PSRAM for Flappy canvas");

            // Fallback: return to the games menu
            lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = GAMES_PAGE;
            return;
        }

        // Init draw buf metadata (small struct in internal SRAM)
        lv_draw_buf_init(&canvas_buf, FLAPPY_CANVAS_W, FLAPPY_CANVAS_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, flappy_canvas_pixels, buf_size);

        // Create canvas
        flappy_canvas = lv_canvas_create(ACTIVE_SCR);
        lv_canvas_set_draw_buf(flappy_canvas, &canvas_buf);
        lv_obj_set_size(flappy_canvas, FLAPPY_CANVAS_W, FLAPPY_CANVAS_H);
        lv_obj_align(flappy_canvas, LV_ALIGN_CENTER, 0, 4);

        // Score label: "HI <best> <current>"
        flappy_score_label = lv_label_create(ACTIVE_SCR);
        char buf[32];
        snprintf(buf, sizeof(buf), "HI %05" PRIu32 " %05" PRIu32, flappy_high_score, flappy_score);
        lv_label_set_text(flappy_score_label, buf);
        lv_obj_set_style_text_color(flappy_score_label, FLAPPY_COL_TEXT, 0);
        lv_obj_align(flappy_score_label, LV_ALIGN_TOP_MID, 0, 2);

        // Game over overlay: centered white text with an 8-way black stroke
        game_over_overlay_create(&flappy_game_over_label, flappy_game_over_stroke);

        flappy_draw_frame();

        // Physics + render at 20 FPS (page handler only runs at the 200ms button poll)
        flappy_timer = lv_timer_create(flappy_game_timer_cb, FLAPPY_FRAME_MS, NULL);

        flappy_init = true;
    }

    // Handle game over
    if (flappy_game_over) {
        // One-time: high score check + overlay text
        if (!flappy_game_over_handled) {
            // If new high score, save
            if (flappy_score > flappy_high_score) {
                flappy_high_score = flappy_score;
                flappy_high_score_nvs_save(flappy_high_score);
            }

            char buf[64];
            snprintf(buf, sizeof(buf), "Game Over!\nScore: %" PRIu32 "\nHigh Score: %" PRIu32, flappy_score, flappy_high_score);
            game_over_overlay_show(flappy_game_over_label, flappy_game_over_stroke, buf);

            flappy_game_over_handled = true;
            flappy_game_over_tick = xTaskGetTickCount();
        }

        // Brief grace so a button still held from the crash is not consumed
        if (xTaskGetTickCount() - flappy_game_over_tick < pdMS_TO_TICKS(600)) {
            return;
        }

        if (ui_btns->up_btn || ui_btns->select_btn) { // Restart
            game_over_overlay_hide(flappy_game_over_label, flappy_game_over_stroke);
            flappy_reset_run();

            // Refresh the readout to 0 for the hover before the first flap
            // (the hover tick returns before the timer's own label update)
            char buf[32];
            snprintf(buf, sizeof(buf), "HI %05" PRIu32 " %05" PRIu32, flappy_high_score, flappy_score);
            lv_label_set_text(flappy_score_label, buf);
        } else if (ui_btns->down_btn || ui_btns->left_btn || ui_btns->right_btn || ui_btns->home_btn) { // Exit to menu
            flappy_cleanup();

            // Back to games menu
            lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

            // Show arrows
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

            ui_menu->page = GAMES_PAGE;
        } else if (ui_btns->pwr_btn) { // Sleep
            flappy_cleanup();

            lcd_transition_back(false, ui_menu); // False = sleep
        }
        return;
    }

    /* Input fallback: the 20 FPS timer samples the semaphores first, but the
       main loop's 200ms poll can win the race, in which case presses arrive
       here instead. Exit buttons (home/pwr) are only ever seen here. */
    if (ui_btns->up_btn || ui_btns->select_btn) { // Flap
        flappy_started = true;
        flappy_bird_vel_q4 = FLAPPY_FLAP_V0_Q4;
    } else if (ui_btns->home_btn) { // Exit to menu
        flappy_cleanup();

        // Show games menu
        lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Show arrows
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = GAMES_PAGE;
    } else if (ui_btns->pwr_btn) { // Sleep
        flappy_cleanup();

        lcd_transition_back(false, ui_menu); // False = sleep
    }
}

