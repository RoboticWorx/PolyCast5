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
#include "lcd_utils.h"
#include "lcd_games.h"

#define TAG "LCD_GAMES"

#define HIGH_SCORE_NS "tetris"
#define HIGH_SCORE_KEY "score"

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

// Game state
static int tetris_board[ORTHO_SIZE][FALL_SIZE] = {0}; // board[row][col], col increases right (fall dir)
static tetris_piece_t tetris_current_piece;
static tetris_piece_t tetris_next_piece;
static uint32_t tetris_score = 0;
static bool tetris_game_over = false;
static TickType_t tetris_last_fall_time = 0;

// LVGL elements
static lv_obj_t *tetris_canvas = NULL;
static lv_obj_t *tetris_score_label = NULL;
static lv_obj_t *tetris_game_over_label = NULL;

static void *tetris_canvas_pixels = NULL; // Raw pixel buffer in PSRAM

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

void lcd_games_tetris_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, tools_menu_t *tools_menu)
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
            ui_menu->page = TOOLS_PAGE;
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

            // Back to tools menu
            lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = TOOLS_PAGE;
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

        // Show tools menu
        lv_obj_remove_flag(tools_menu->main_list, LV_OBJ_FLAG_HIDDEN);

        // Hide right arrow
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);

        ui_menu->page = TOOLS_PAGE;
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

/* ========== T-Rex Runner Implementation ========== */

