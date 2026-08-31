/*

DOOM - an 8-bit, Wolfenstein-3D-style raycaster mini-FPS for the PolyCast5 games menu.

Entry point: lcd_games_doom_page() is the page handler that lcd_task runs while
ui_menu->page == GAMES_DOOM_PAGE. On first entry it allocates a PSRAM framebuffer +
an lv_canvas, procedurally generates the wall textures / enemy sprites / weapon,
builds the first level and starts an lv_timer that drives the game at ~25 FPS. On
exit (Home / power / back to menu) everything is deleted and freed.

Rendering (doom_render_all, once per tick, written straight into the PSRAM
framebuffer that backs the canvas - never via lv_canvas_set_px, which is far too
slow for a full screen):
  1. doom_render_walls()    - the raycaster. For each of the 240 screen columns it
                              casts one ray and DDA-steps through the tile grid
                              until it hits a wall, then draws a vertical textured
                              slice whose height is 1/distance. The hit distance is
                              saved in doom_zbuffer[x].
  2. doom_render_sprites()  - billboarded enemies, projected with the inverse camera
                              matrix, drawn far-to-near and clipped per column
                              against the z-buffer so walls occlude them.
  3. doom_render_crosshair() / doom_render_gun() - aim reticle + weapon overlay.
The canvas is the top DOOM_VIEW_H rows; a thin HUD text strip sits just below it.

World: a DOOM_MAP_W x DOOM_MAP_H tile grid (0 = empty, 1..N = wall texture id,
9 = exit door). doom_generate_level() builds a fresh random map each level (an open
arena with scattered pillars) and guarantees solvability with a BFS flood fill from
the player start - the exit and every enemy spawn are placed only on reachable
cells. Win by reaching the exit OR clearing every enemy.

Input: movement and turning read the raw held-button globals (gpio_up/down/left/
right/select_btn_held), sampled every tick, NOT the auto-repeat semaphores, so
holding a button moves smoothly. The page handler's 200ms poll only handles leaving.

Difficulty scales with the level (enemy count / speed / health, all bounded so
enemies stay killable and dodgeable; ammo scales sub-linearly with the swarm). The
highest level reached is persisted to NVS as the high score.

Memory: per the project's PSRAM-first strategy the framebuffer, all generated art
and every large array live in PSRAM (POLYCAST5_USE_PSRAM_BSS / MALLOC_CAP_SPIRAM);
internal SRAM use is limited to small, short-lived stack locals.

*/

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_err.h"
#include "nvs.h"

#include "polycast5_macros.h"
#include "gpio_task.h"
#include "lcd_utils.h"
#include "lcd_games.h"
#include "lcd_doom_assets.h"

#define TAG "DOOM"

/* Raw held button state (declared in gpio_task.c). The short-press semaphores
 * only auto-repeat, so smooth hold-to-move/turn reads these directly instead. */
extern volatile bool gpio_select_btn_held;
extern volatile bool gpio_up_btn_held;
extern volatile bool gpio_down_btn_held;
extern volatile bool gpio_left_btn_held;
extern volatile bool gpio_right_btn_held;

/* ---- Tunables -------------------------------------------------------------- */
#define DOOM_VIEW_W 240          // 3D viewport width (full screen width)
#define DOOM_VIEW_H 121          // 3D viewport height; thin HUD text line below
#define DOOM_HORIZON (DOOM_VIEW_H / 2)
#define DOOM_FRAME_MS 40         // ~25 FPS game tick

#define DOOM_TEX_SIZE 64         // Wall texture size (power of two for fast mask)
#define DOOM_NUM_TEX 10          // Indexed by tile value (0..9)
#define DOOM_SPR 40              // Enemy sprite is DOOM_SPR x DOOM_SPR
#define DOOM_ENEMY_TYPES 3       // Random variants: 0 = red imp, 1 = cacodemon, 2 = knife humanoid
#define DOOM_GUN_W 84            // Weapon sprite (3/4 angled pistol)
#define DOOM_GUN_H 80
#define DOOM_GUN_MUZZLE_SX 22    // Muzzle tip in sprite space (for the flash)
#define DOOM_GUN_MUZZLE_SY 33
#define DOOM_GUN_OFF_X 48        // Shift right of centre (held in the right hand)
#define DOOM_GUN_OFF_Y (-3)      // Nudge the weapon up so the barrel keeps reading as aimed at the crosshair

#define DOOM_MOVE_SPEED 0.085f
#define DOOM_ROT_SPEED  0.060f
#define DOOM_WALL_MARGIN 0.18f
#define DOOM_PERP_MIN 0.05f

#define DOOM_ENEMY_SPEED 0.024f
#define DOOM_ENEMY_RADIUS 0.40f
#define DOOM_ENEMY_SIGHT 12.0f
#define DOOM_ENEMY_ATTACK_RANGE 1.3f
#define DOOM_ENEMY_HEALTH 30
#define DOOM_ENEMY_DMG 8
#define DOOM_ENEMY_ATTACK_COOLDOWN_MS 850
#define DOOM_ENEMY_DEATH_MS 360

#define DOOM_GUN_DMG 16
#define DOOM_FIRE_COOLDOWN_MS 300
#define DOOM_HITSCAN_RANGE 18.0f
#define DOOM_MUZZLE_MS 80
#define DOOM_HURT_FLASH_MS 350   // HUD flashes red this long after an enemy hit

#define DOOM_START_HEALTH 100
#define DOOM_START_AMMO 50
#define DOOM_AMMO_PER_ENEMY 1    // +rounds per spawned enemy; fewer than it takes to kill one,
                                 // so bigger swarms force conservation (can't clear them all)

// Per-level difficulty scaling (level 1 = base values above)
#define DOOM_BASE_ENEMIES 6
#define DOOM_ENEMIES_PER_LEVEL 2
#define DOOM_ENEMY_SPEED_PER_LEVEL 0.006f
#define DOOM_ENEMY_SPEED_MAX 0.060f
#define DOOM_ENEMY_HP_PER_LEVEL 4
#define DOOM_ENEMY_HP_MAX 60     // Cap HP so enemies stay killable; difficulty comes from count

// Enemy states
#define DOOM_E_DEAD  0
#define DOOM_E_ALIVE 1
#define DOOM_E_DYING 2

typedef struct {
    float x, y;          // Position (tile units)
    int health;
    uint8_t type;
    uint8_t state;       // DOOM_E_*
    bool near_attack;    // Within attack range this frame (drives mouth-open frame)
    uint16_t hurt_ms;    // Remaining hit-flash time
    uint16_t dying_ms;   // Remaining death-shrink time
    TickType_t last_attack;
    float dist;          // Scratch: squared distance to player (render sort)
} doom_enemy_t;

/* ---- State (everything that can live in PSRAM, does) ----------------------- */
POLYCAST5_USE_PSRAM_BSS static uint8_t doom_map[DOOM_MAP_H][DOOM_MAP_W];
POLYCAST5_USE_PSRAM_BSS static float doom_zbuffer[DOOM_VIEW_W];
POLYCAST5_USE_PSRAM_BSS static uint16_t doom_bg_row[DOOM_VIEW_H]; // Ceiling/floor gradient
POLYCAST5_USE_PSRAM_BSS static doom_enemy_t doom_enemies[DOOM_MAX_ENEMIES];
POLYCAST5_USE_PSRAM_BSS static int doom_enemy_count;

POLYCAST5_USE_PSRAM_BSS static float doom_posX, doom_posY;
POLYCAST5_USE_PSRAM_BSS static float doom_dirX, doom_dirY;
POLYCAST5_USE_PSRAM_BSS static float doom_planeX, doom_planeY;
POLYCAST5_USE_PSRAM_BSS static int doom_health, doom_ammo;
POLYCAST5_USE_PSRAM_BSS static float doom_bob_phase;
POLYCAST5_USE_PSRAM_BSS static uint16_t doom_muzzle_ms;
POLYCAST5_USE_PSRAM_BSS static uint16_t doom_hurt_ms;   // >0 = HUD flashes red (just took damage)
POLYCAST5_USE_PSRAM_BSS static bool doom_moved;
POLYCAST5_USE_PSRAM_BSS static bool doom_prev_select;
POLYCAST5_USE_PSRAM_BSS static TickType_t doom_fire_last;

POLYCAST5_USE_PSRAM_BSS static bool doom_init;
POLYCAST5_USE_PSRAM_BSS static bool doom_game_over;
POLYCAST5_USE_PSRAM_BSS static bool doom_game_won;
POLYCAST5_USE_PSRAM_BSS static bool doom_state_handled;
POLYCAST5_USE_PSRAM_BSS static bool doom_hud_dirty;
POLYCAST5_USE_PSRAM_BSS static TickType_t doom_over_tick;
POLYCAST5_USE_PSRAM_BSS static TickType_t doom_hint_until; // hide the level-1 hint after this tick
POLYCAST5_USE_PSRAM_BSS static bool doom_hint_active;

// Level progression + per-level difficulty, regenerated for every level (PSRAM)
POLYCAST5_USE_PSRAM_BSS static int doom_level;
POLYCAST5_USE_PSRAM_BSS static uint32_t doom_high_level; // highest level CLEARED (NVS-persisted)
POLYCAST5_USE_PSRAM_BSS static float doom_enemy_speed;
POLYCAST5_USE_PSRAM_BSS static int doom_enemy_hp;
POLYCAST5_USE_PSRAM_BSS static int doom_start_x, doom_start_y;
POLYCAST5_USE_PSRAM_BSS static doom_spawn_t doom_spawn_list[DOOM_MAX_ENEMIES];
POLYCAST5_USE_PSRAM_BSS static int doom_spawn_n;
// Flood-fill scratch for connectivity-checked generation
POLYCAST5_USE_PSRAM_BSS static int16_t doom_dist[DOOM_MAP_W * DOOM_MAP_H];
POLYCAST5_USE_PSRAM_BSS static int16_t doom_bfsq[DOOM_MAP_W * DOOM_MAP_H];

// Generated assets (PSRAM)
POLYCAST5_USE_PSRAM_BSS static uint8_t *doom_tex[DOOM_NUM_TEX];
POLYCAST5_USE_PSRAM_BSS static uint8_t *doom_demon_spr[DOOM_ENEMY_TYPES][2]; // [type][0=idle,1=attack]
POLYCAST5_USE_PSRAM_BSS static uint8_t *doom_gun;

// Framebuffer + LVGL objects
POLYCAST5_USE_PSRAM_BSS static uint16_t *doom_fb;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *doom_canvas;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *doom_hud_label;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *doom_overlay_label;
POLYCAST5_USE_PSRAM_BSS static lv_obj_t *doom_hint_label;    // "Find the exit!" on level 1
POLYCAST5_USE_PSRAM_BSS static lv_timer_t *doom_timer;

/* ---- Small helpers --------------------------------------------------------- */

// Darken an RGB565 colour by halving brightness `level` times (side/distance shade)
static inline uint16_t doom_shade(uint16_t c, int level)
{
    while (level-- > 0) {
        c = (uint16_t)((c >> 1) & 0x7BEF);
    }
    return c;
}

// Deterministic hash for texture speckle (stable across runs, no esp_random churn)
static inline uint32_t doom_hash(uint32_t a)
{
    a ^= a << 13;
    a ^= a >> 17;
    a ^= a << 5;
    return a;
}

// Read a map tile; out-of-bounds returns a solid border tile so rays and
// movement always hit a wall at the edge of the map.
static inline uint8_t doom_tile(int x, int y)
{
    if (x < 0 || y < 0 || x >= DOOM_MAP_W || y >= DOOM_MAP_H) {
        return DOOM_TILE_BORDER;
    }
    return doom_map[y][x];
}

// True if a tile blocks movement and rays - anything but empty (the exit door is
// solid too, so you walk up to it rather than through it).
static inline bool doom_is_wall(int x, int y)
{
    return doom_tile(x, y) != DOOM_TILE_EMPTY; // Exit door is solid too (touch to win)
}

// Unpack two palette indices and linearly blend them (t: 0..256). Used for the
// ceiling/floor gradient, computed once at init.
static uint16_t doom_blend565(uint8_t a_idx, uint8_t b_idx, int t)
{
    uint16_t a = doom_palette[a_idx], b = doom_palette[b_idx];
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + ((br - ar) * t >> 8);
    int g = ag + ((bg - ag) * t >> 8);
    int bl = ab + ((bb - ab) * t >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* ---- High score: highest level reached, persisted in NVS ------------------- */
#define DOOM_HIGH_SCORE_NS "doom"
#define DOOM_HIGH_SCORE_KEY "level"

static void doom_high_score_save(uint32_t level)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOOM_HIGH_SCORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "doom_high_score_save nvs_open failed");
        return; // Handle not open - nothing to close
    }
    err = nvs_set_u32(h, DOOM_HIGH_SCORE_KEY, level);
    if (err == ESP_OK) {
        nvs_commit(h);
#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Saved Doom high level: %" PRIu32, level);
#endif
    } else {
        ESP_LOGE(TAG, "Failed doom_high_score_save nvs_set_u32");
    }
    nvs_close(h);
}

static uint32_t doom_high_score_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DOOM_HIGH_SCORE_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return 0; // Namespace does not exist yet - no high score saved
    }
    uint32_t level = 0;
    nvs_get_u32(h, DOOM_HIGH_SCORE_KEY, &level);
    nvs_close(h);
    return level;
}

// Called when a level is CLEARED. If that level beats the saved best (the
// highest level ever cleared), persist it and return true so the overlay can
// show "NEW BEST!". Death never calls this, so dying can't set a record.
static bool doom_record_cleared_level(void)
{
    if ((uint32_t)doom_level > doom_high_level) {
        doom_high_level = (uint32_t)doom_level;
        doom_high_score_save(doom_high_level);
        return true;
    }
    return false;
}

/* ---- Random level generation ----------------------------------------------- */

// Uniform random in 0..n-1
static int doom_rand(int n)
{
    return n > 0 ? (int)(esp_random() % (uint32_t)n) : 0;
}

// Build a fresh random level for the current doom_level: an open arena with
// sparse pillars, a random player start, an exit door and enemy spawns.
// Connectivity is guaranteed by flood-filling from the start and placing the
// exit + spawns only on cells reachable from it, so every level is solvable.
// Enemy count / speed / health scale with doom_level.
static void doom_generate_level(void)
{
    const int W = DOOM_MAP_W, H = DOOM_MAP_H, N = W * H;
    const int lvl = doom_level;

    // Difficulty scaling
    doom_enemy_speed = DOOM_ENEMY_SPEED + DOOM_ENEMY_SPEED_PER_LEVEL * (lvl - 1);
    if (doom_enemy_speed > DOOM_ENEMY_SPEED_MAX) doom_enemy_speed = DOOM_ENEMY_SPEED_MAX;
    doom_enemy_hp = DOOM_ENEMY_HEALTH + DOOM_ENEMY_HP_PER_LEVEL * (lvl - 1);
    if (doom_enemy_hp > DOOM_ENEMY_HP_MAX) doom_enemy_hp = DOOM_ENEMY_HP_MAX; // stay killable
    int want = DOOM_BASE_ENEMIES + DOOM_ENEMIES_PER_LEVEL * (lvl - 1);
    if (want > DOOM_MAX_ENEMIES) want = DOOM_MAX_ENEMIES;

    // 1. Open arena with a solid stone border
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            doom_map[y][x] = (x == 0 || y == 0 || x == W - 1 || y == H - 1) ? DOOM_TILE_BORDER : DOOM_TILE_EMPTY;

    // 2. Scatter sparse obstacles. Single pillars (plus the odd 2-tile nub) never
    //    seal off an open arena, so the playfield stays connected.
    int obstacles = 26 + doom_rand(14);
    for (int k = 0; k < obstacles; k++) {
        int x = 2 + doom_rand(W - 4);
        int y = 2 + doom_rand(H - 4);
        uint8_t tile = (uint8_t)(1 + doom_rand(4));
        doom_map[y][x] = tile;
        if (doom_rand(2) == 0) {
            int nx = x + (doom_rand(2) ? 1 : -1);
            int ny = y + (doom_rand(2) ? 1 : -1);
            if (nx > 0 && ny > 0 && nx < W - 1 && ny < H - 1) doom_map[ny][nx] = tile;
        }
    }

    // 3. Player start: a random interior cell with a cleared 3x3 pocket
    int sx = 2 + doom_rand(W - 4);
    int sy = 2 + doom_rand(H - 4);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            doom_map[sy + dy][sx + dx] = DOOM_TILE_EMPTY;
    doom_start_x = sx;
    doom_start_y = sy;

    // 4. BFS flood-fill from the start over empty cells (walls block) -> distances
    for (int i = 0; i < N; i++) doom_dist[i] = -1;
    int head = 0, tail = 0, far = sy * W + sx, fard = 0;
    doom_dist[far] = 0;
    doom_bfsq[tail++] = (int16_t)far;
    const int ddx[4] = {1, -1, 0, 0}, ddy[4] = {0, 0, 1, -1};
    while (head < tail) {
        int cur = doom_bfsq[head++];
        int cx = cur % W, cy = cur / W;
        if (doom_dist[cur] > fard) { fard = doom_dist[cur]; far = cur; }
        for (int d = 0; d < 4; d++) {
            int nx = cx + ddx[d], ny = cy + ddy[d];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            int ni = ny * W + nx;
            if (doom_dist[ni] != -1) continue;
            if (doom_map[ny][nx] != DOOM_TILE_EMPTY) continue;
            doom_dist[ni] = (int16_t)(doom_dist[cur] + 1);
            doom_bfsq[tail++] = (int16_t)ni;
        }
    }

    // 5. Exit door at the farthest reachable cell (always reachable by construction)
    doom_map[far / W][far % W] = DOOM_TILE_EXIT;
    doom_dist[far] = -1; // now solid: not a spawn candidate

    // 6. Enemy spawns on reachable empty cells, kept a few tiles from the start
    doom_spawn_n = 0;
    for (int guard = 0; guard < 4000 && doom_spawn_n < want; guard++) {
        int x = 1 + doom_rand(W - 2);
        int y = 1 + doom_rand(H - 2);
        int idx = y * W + x;
        if (doom_dist[idx] < 5) continue;          // unreachable (-1) or too close to start
        if (doom_map[y][x] != DOOM_TILE_EMPTY) continue;
        bool dup = false;
        for (int j = 0; j < doom_spawn_n; j++)
            if (doom_spawn_list[j].x == x && doom_spawn_list[j].y == y) { dup = true; break; }
        if (dup) continue;
        doom_spawn_list[doom_spawn_n].x = (uint8_t)x;
        doom_spawn_list[doom_spawn_n].y = (uint8_t)y;
        doom_spawn_list[doom_spawn_n].type = (uint8_t)doom_rand(DOOM_ENEMY_TYPES);
        doom_spawn_n++;
    }

    // Fallback: if a tightly-boxed start left no cell >= 5 tiles away, place one
    // enemy on the first reachable cell found (row-major scan) so a level always has an enemy.
    if (doom_spawn_n == 0) {
        for (int i = 0; i < N; i++) {
            int x = i % W, y = i / W;
            if (doom_dist[i] >= 1 && doom_map[y][x] == DOOM_TILE_EMPTY) {
                doom_spawn_list[0].x = (uint8_t)x;
                doom_spawn_list[0].y = (uint8_t)y;
                doom_spawn_list[0].type = (uint8_t)doom_rand(DOOM_ENEMY_TYPES);
                doom_spawn_n = 1;
                break;
            }
        }
    }
}

/* ---- Procedural texture generation (into PSRAM) ---------------------------- */
// Brick wall: offset courses of bricks with mortar lines + per-brick colour noise.
static void doom_gen_brick(uint8_t *t)
{
    for (int y = 0; y < DOOM_TEX_SIZE; y++) {
        int row = y / 16;                      // 4 courses of bricks
        int xoff = (row & 1) ? 16 : 0;         // Offset every other course
        for (int x = 0; x < DOOM_TEX_SIZE; x++) {
            int bx = (x + xoff) % 32;          // Brick width 32
            int by = y % 16;
            uint8_t idx;
            if (by < 2 || bx < 2) {
                idx = PAL_MORTAR;              // Mortar lines
            } else {
                uint32_t h = doom_hash((uint32_t)(row * 7 + (x + xoff) / 32) * 131u + 17u);
                idx = ((h & 3) == 0) ? PAL_DBROWN : (((h & 3) == 1) ? PAL_LBROWN : PAL_BROWN);
                if ((doom_hash((uint32_t)(x * 53 + y * 131)) & 7) == 0) {
                    idx = PAL_DBROWN;          // Speckle
                }
            }
            t[y * DOOM_TEX_SIZE + x] = idx;
        }
    }
}

// Stone wall: grey speckle with block seams; mossy=true overlays green patches.
static void doom_gen_stone(uint8_t *t, bool mossy)
{
    for (int y = 0; y < DOOM_TEX_SIZE; y++) {
        for (int x = 0; x < DOOM_TEX_SIZE; x++) {
            uint8_t idx = PAL_GRAY;
            uint32_t h = doom_hash((uint32_t)(x * 131 + y * 977));
            if ((h & 7) == 0) idx = PAL_DGRAY;
            else if ((h & 7) == 1) idx = PAL_LGRAY;
            if ((x % 32) == 0 || (y % 32) == 0) idx = PAL_DGRAY; // Block seams
            if (mossy) {
                uint32_t m = doom_hash((uint32_t)((x / 4) * 61 + (y / 4) * 199));
                if ((m % 5) < 2) {
                    idx = (doom_hash((uint32_t)(x * 7 + y * 13)) & 1) ? PAL_GREEN : PAL_DGREEN;
                }
            }
            t[y * DOOM_TEX_SIZE + x] = idx;
        }
    }
}

// Tech/metal wall: 32px panels with a dark frame and corner bolts.
static void doom_gen_metal(uint8_t *t)
{
    for (int y = 0; y < DOOM_TEX_SIZE; y++) {
        for (int x = 0; x < DOOM_TEX_SIZE; x++) {
            int px = x % 32, py = y % 32;
            uint8_t idx = PAL_STEEL;
            if (px < 2 || py < 2 || px > 29 || py > 29) {
                idx = PAL_DSTEEL;              // Panel frame
            } else if (((px < 5) || (px > 26)) && ((py < 5) || (py > 26))) {
                idx = PAL_LGRAY;               // Corner bolts
            }
            t[y * DOOM_TEX_SIZE + x] = idx;
        }
    }
}

// Exit-door texture: teal panel with a glowing green centre strip + accent bars.
static void doom_gen_exit(uint8_t *t)
{
    for (int y = 0; y < DOOM_TEX_SIZE; y++) {
        for (int x = 0; x < DOOM_TEX_SIZE; x++) {
            uint8_t idx = PAL_DTEAL;           // Frame
            if (x >= 6 && x < 58 && y >= 6 && y < 58) idx = PAL_TEAL; // Door panel
            if (x >= 28 && x < 36 && y >= 10 && y < 54) idx = PAL_BGREEN; // Glow strip
            if ((y == 20 || y == 44) && x >= 10 && x < 54) idx = PAL_BGREEN; // Accent bars
            t[y * DOOM_TEX_SIZE + x] = idx;
        }
    }
}

/* ---- Procedural sprite generation: three enemy types share the pixel + limb --
 * helpers below. 0 = red horned imp, 1 = cacodemon (floating eye), 2 = pale
 * knife-wielding humanoid. The weapon sprite (doom_gen_gun) follows them. */

// Bounds-checked pixel write into a DOOM_SPR x DOOM_SPR sprite buffer.
static inline void doom_spr_px(uint8_t *s, int x, int y, uint8_t idx)
{
    if (x >= 0 && x < DOOM_SPR && y >= 0 && y < DOOM_SPR) s[y * DOOM_SPR + x] = idx;
}

// Filled ellipse body part: upper-left highlight, rim/lower shadow for volume
static void doom_demon_limb(uint8_t *s, float cx, float cy, float rx, float ry,
                            uint8_t base, uint8_t dark, uint8_t hi)
{
    for (int y = (int)(cy - ry); y <= (int)(cy + ry); y++) {
        for (int x = (int)(cx - rx); x <= (int)(cx + rx); x++) {
            float nx = (x - cx) / rx, ny = (y - cy) / ry;
            float d = nx * nx + ny * ny;
            if (d > 1.0f) continue;
            uint8_t c = base;
            if (d > 0.66f) c = dark;                      // rim shadow
            else if (nx < -0.15f && ny < -0.15f) c = hi;  // upper-left highlight
            doom_spr_px(s, x, y, c);
        }
    }
}

// Shared lower body: legs, feet and torso (one colour). Arms + head are per-type.
static void doom_humanoid_body(uint8_t *s, uint8_t base, uint8_t dark, uint8_t hi, uint8_t foot)
{
    const int cx = (DOOM_SPR - 1) / 2;

    // Legs + feet
    doom_demon_limb(s, cx - 5, 32, 4.0f, 7.0f, base, dark, hi);
    doom_demon_limb(s, cx + 5, 32, 4.0f, 7.0f, base, dark, hi);
    for (int x = -2; x <= 2; x++) {
        doom_spr_px(s, cx - 5 + x, 38, foot);
        doom_spr_px(s, cx + 5 + x, 38, foot);
        doom_spr_px(s, cx - 5 + x, 39, PAL_BLACK);
        doom_spr_px(s, cx + 5 + x, 39, PAL_BLACK);
    }

    // Torso: broad shoulders tapering to the waist, with muscle shading
    doom_demon_limb(s, cx, 16, 12.0f, 5.0f, base, dark, hi);
    doom_demon_limb(s, cx, 23, 10.0f, 9.0f, base, dark, hi);
    for (int y = 19; y <= 29; y++) doom_spr_px(s, cx, y, dark);          // sternum line
    doom_spr_px(s, cx - 5, 20, dark); doom_spr_px(s, cx + 5, 20, dark);  // pecs
    doom_spr_px(s, cx - 4, 25, dark); doom_spr_px(s, cx + 4, 25, dark);  // abs
}

// Two arms in `base` colour (raised when attacking), ending in a `tip` mark
// (claws for the imp, bare hands for the human)
static void doom_humanoid_arms(uint8_t *s, bool attack, uint8_t base, uint8_t dark, uint8_t hi, uint8_t tip)
{
    const int cx = (DOOM_SPR - 1) / 2;
    if (attack) {
        doom_demon_limb(s, cx - 13, 14, 3.5f, 7.0f, base, dark, hi);
        doom_demon_limb(s, cx + 13, 14, 3.5f, 7.0f, base, dark, hi);
        for (int k = 0; k < 3; k++) {
            doom_spr_px(s, cx - 15 + k * 2, 7, tip);
            doom_spr_px(s, cx + 11 + k * 2, 7, tip);
        }
    } else {
        doom_demon_limb(s, cx - 12, 22, 3.5f, 8.0f, base, dark, hi);
        doom_demon_limb(s, cx + 12, 22, 3.5f, 8.0f, base, dark, hi);
        for (int k = 0; k < 3; k++) {
            doom_spr_px(s, cx - 14 + k * 2, 29, tip);
            doom_spr_px(s, cx + 10 + k * 2, 29, tip);
        }
    }
}

// Type 0: red horned imp (humanoid demon)
static void doom_gen_imp(uint8_t *s, bool attack)
{
    for (int i = 0; i < DOOM_SPR * DOOM_SPR; i++) s[i] = PAL_KEY;
    const int cx = (DOOM_SPR - 1) / 2;

    doom_humanoid_body(s, PAL_RED, PAL_DRED, PAL_BRED, PAL_DGRAY);
    doom_humanoid_arms(s, attack, PAL_RED, PAL_DRED, PAL_BRED, PAL_LGRAY); // red arms, grey claws

    // Head + horns + brow ridge
    doom_demon_limb(s, cx, 9, 7.0f, 7.0f, PAL_RED, PAL_DRED, PAL_BRED);
    doom_spr_px(s, cx - 7, 4, PAL_LGRAY); doom_spr_px(s, cx - 8, 3, PAL_LGRAY); doom_spr_px(s, cx - 9, 2, PAL_LGRAY);
    doom_spr_px(s, cx + 7, 4, PAL_LGRAY); doom_spr_px(s, cx + 8, 3, PAL_LGRAY); doom_spr_px(s, cx + 9, 2, PAL_LGRAY);
    for (int x = -5; x <= 5; x++) doom_spr_px(s, cx + x, 6, PAL_DRED);

    // Glowing yellow eyes
    for (int e = 0; e < 2; e++) {
        int ex = cx + (e ? 3 : -3);
        doom_spr_px(s, ex, 8, PAL_YELLOW);
        doom_spr_px(s, ex + (e ? 1 : -1), 8, PAL_YELLOW);
        doom_spr_px(s, ex, 7, PAL_BLACK);
    }

    int my = 12;
    if (attack) {
        for (int yy = 0; yy <= 4; yy++)
            for (int xx = -4; xx <= 4; xx++)
                if (xx * xx + (yy - 2) * (yy - 2) * 3 <= 16) doom_spr_px(s, cx + xx, my + yy, PAL_BLACK);
        doom_spr_px(s, cx - 3, my, PAL_WHITE);     doom_spr_px(s, cx + 3, my, PAL_WHITE);
        doom_spr_px(s, cx - 2, my + 3, PAL_WHITE); doom_spr_px(s, cx + 2, my + 3, PAL_WHITE);
    } else {
        for (int xx = -4; xx <= 4; xx++) doom_spr_px(s, cx + xx, my, PAL_MAROON);
        doom_spr_px(s, cx - 3, my + 1, PAL_WHITE);
        doom_spr_px(s, cx + 3, my + 1, PAL_WHITE);
    }
}

// Type 1: cacodemon -- a floating sphere with spikes, one big eye and a fanged maw
static void doom_gen_caco(uint8_t *s, bool attack)
{
    for (int i = 0; i < DOOM_SPR * DOOM_SPR; i++) s[i] = PAL_KEY;
    const float cx = (DOOM_SPR - 1) / 2.0f;
    const float cy = (DOOM_SPR - 1) / 2.0f;
    const float R = DOOM_SPR / 2.0f - 3.0f;

    // Spikes around the upper hemisphere (body is drawn over their roots)
    const int NSP = 9;
    for (int k = 0; k < NSP; k++) {
        float a = -3.1416f + k * (3.1416f / (NSP - 1)); // left -> top -> right
        for (int r = (int)R; r <= (int)R + 4; r++) {
            int px = (int)(cx + cosf(a) * r);
            int py = (int)(cy + sinf(a) * r);
            doom_spr_px(s, px, py, (r >= (int)R + 2) ? PAL_LGRAY : PAL_DGRAY);
        }
    }

    // Body sphere (bumpy red)
    for (int y = (int)(cy - R); y <= (int)(cy + R); y++) {
        for (int x = (int)(cx - R); x <= (int)(cx + R); x++) {
            float dx = x - cx, dy = y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d > R) continue;
            uint8_t c = PAL_RED;
            if (d > R - 3.0f) c = PAL_DRED;                        // rim
            else if (dx < -2.0f && dy < -2.0f) c = PAL_PINK;       // highlight
            else if (dx * 0.4f + dy * 0.7f > 6.0f) c = PAL_DRED;   // lower-right shade
            else if (((x * 3 + y * 5) & 7) == 0) c = PAL_DRED;     // bumpy texture
            doom_spr_px(s, x, y, c);
        }
    }

    // Big central eye + red brow
    int ey = (int)(cy - 3);
    doom_demon_limb(s, cx, ey, 6.0f, 5.0f, PAL_WHITE, PAL_LGRAY, PAL_WHITE); // sclera
    doom_demon_limb(s, cx, ey, 3.2f, 3.2f, PAL_GREEN, PAL_DGREEN, PAL_BGREEN); // iris
    for (int yy = -1; yy <= 1; yy++)
        for (int xx = -1; xx <= 1; xx++) doom_spr_px(s, (int)cx + xx, ey + yy, PAL_BLACK); // pupil
    for (int xx = -6; xx <= 6; xx++) doom_spr_px(s, (int)cx + xx, ey - 5, PAL_DRED);       // brow

    // Wide fanged maw at the bottom
    int my = (int)(cy + 8);
    if (attack) {
        for (int yy = -1; yy <= 5; yy++)
            for (int xx = -9; xx <= 9; xx++)
                if (xx * xx * 25 + (yy - 2) * (yy - 2) * 81 <= 81 * 25) doom_spr_px(s, (int)cx + xx, my + yy, PAL_BLACK);
        for (int xx = -8; xx <= 8; xx += 3) {
            doom_spr_px(s, (int)cx + xx, my - 1, PAL_WHITE);
            doom_spr_px(s, (int)cx + xx + 1, my + 4, PAL_WHITE);
        }
    } else {
        for (int xx = -8; xx <= 8; xx++) doom_spr_px(s, (int)cx + xx, my, PAL_MAROON);
        for (int xx = -7; xx <= 7; xx += 3) doom_spr_px(s, (int)cx + xx, my, PAL_WHITE);
    }
}

// Type 2: pale-skinned humanoid wielding a knife (clearly visible vs the demons)
static void doom_gen_human(uint8_t *s, bool attack)
{
    for (int i = 0; i < DOOM_SPR * DOOM_SPR; i++) s[i] = PAL_KEY;
    const int cx = (DOOM_SPR - 1) / 2;

    doom_humanoid_body(s, PAL_GREEN, PAL_DGREEN, PAL_LBROWN, PAL_DGRAY);
    doom_humanoid_arms(s, attack, PAL_FLESH, PAL_DFLESH, PAL_WHITE, PAL_FLESH); // pale bare arms
    for (int x = -8; x <= 8; x++) doom_spr_px(s, cx + x, 28, PAL_DBROWN); // belt

    // Knife in the (right) hand: raised in the attack frame, lowered when idle
    if (attack) {
        for (int yy = 2; yy <= 10; yy++) doom_spr_px(s, cx + 13, yy, PAL_LGRAY);
        for (int yy = 3; yy <= 9; yy++)  doom_spr_px(s, cx + 14, yy, PAL_WHITE);
        doom_spr_px(s, cx + 13, 11, PAL_DBROWN); doom_spr_px(s, cx + 13, 12, PAL_DBROWN);
    } else {
        for (int yy = 27; yy <= 34; yy++) doom_spr_px(s, cx + 14, yy, PAL_LGRAY);
        doom_spr_px(s, cx + 14, 26, PAL_DBROWN);
    }

    // Head: pale face + brown hair
    doom_demon_limb(s, cx, 9, 5.5f, 6.0f, PAL_FLESH, PAL_DFLESH, PAL_WHITE);
    for (int x = -5; x <= 5; x++)
        for (int yy = 3; yy <= 5; yy++)
            if (x * x + (yy - 5) * (yy - 5) * 2 <= 25) doom_spr_px(s, cx + x, yy, PAL_DBROWN);
    doom_spr_px(s, cx - 2, 9, PAL_BLACK);
    doom_spr_px(s, cx + 2, 9, PAL_BLACK);
    if (attack) {
        doom_spr_px(s, cx - 1, 12, PAL_MAROON); doom_spr_px(s, cx, 12, PAL_BLACK);
        doom_spr_px(s, cx + 1, 12, PAL_MAROON); doom_spr_px(s, cx, 13, PAL_BLACK);
    } else {
        for (int xx = -2; xx <= 2; xx++) doom_spr_px(s, cx + xx, 12, PAL_DFLESH);
    }
}

// Bounds-checked pixel write into the DOOM_GUN_W x DOOM_GUN_H weapon sprite.
static inline void doom_gun_px(uint8_t *g, int x, int y, uint8_t idx)
{
    if (x >= 0 && x < DOOM_GUN_W && y >= 0 && y < DOOM_GUN_H) g[y * DOOM_GUN_W + x] = idx;
}

// Pistol at a 3/4 angle: the slide+barrel form a long shaded bar that points UP
// (forward, into the scene) but leans, so its elongated profile and side face
// read clearly as a gun. The grip + fist drop toward the viewer. Slide, grip and
// hand are each drawn as a rotated bar (project each pixel onto the bar's axis).
static void doom_gen_gun(uint8_t *g)
{
    for (int i = 0; i < DOOM_GUN_W * DOOM_GUN_H; i++) g[i] = PAL_KEY;

    // Slide long-axis: muzzle (upper-left) -> base (lower-right). The muzzle sits
    // up and to the left of the grip so the barrel points up-and-left at a shallow
    // angle, lined up so it reads as aiming at the centre crosshair.
    const float Mx = 22, My = 33;   // muzzle tip
    const float Bx = 46, By = 50;   // base of the slide (grip starts here)
    const float Gx = 44, Gy = 74;   // grip bottom
    float dx = Bx - Mx, dy = By - My;
    float len = sqrtf(dx * dx + dy * dy);
    float ux = dx / len, uy = dy / len;

    // ---- Slide + barrel: a long bar, lit near edge + dark far side face ----
    for (int y = 0; y < DOOM_GUN_H; y++) {
        for (int x = 0; x < DOOM_GUN_W; x++) {
            float wx = x - Mx, wy = y - My;
            float t = (wx * dx + wy * dy) / (len * len);   // 0 muzzle .. 1 base
            if (t < 0.0f || t > 1.0f) continue;
            float s = (dx * wy - dy * wx) / len;           // signed perp distance
            float hw = 4.5f + 4.0f * t;                    // barrel thin -> slide thick
            if (s < -hw || s > hw) continue;
            uint8_t idx;
            if (s <= -hw + 1.5f)     idx = PAL_LGRAY;   // near-edge highlight
            else if (s >= hw - 2.5f) idx = PAL_DSTEEL;  // far side face (3/4 depth)
            else if (s >= hw - 4.5f) idx = PAL_DGRAY;
            else                     idx = PAL_GRAY;
            if (t > 0.55f && t < 0.72f && s > -2.0f && s < 2.5f) idx = PAL_BLACK; // ejection port
            doom_gun_px(g, x, y, idx);
        }
    }

    // ---- Muzzle cap, bore + front sight ----
    {
        int mx = (int)Mx, my = (int)My;
        for (int yy = -2; yy <= 2; yy++)
            for (int xx = -3; xx <= 3; xx++)
                if (xx * xx + yy * yy * 2 <= 9) doom_gun_px(g, mx + xx, my + yy, PAL_DSTEEL);
        doom_gun_px(g, mx, my, PAL_BLACK); doom_gun_px(g, mx - 1, my, PAL_BLACK);
        doom_gun_px(g, mx, my - 3, PAL_DGRAY);   // front sight
    }

    // ---- Trigger + guard hint on the underside ----
    {
        float px = Mx + dx * 0.82f, py = My + dy * 0.82f;
        int tx = (int)(px - uy * 7.0f), ty = (int)(py + ux * 7.0f);
        for (int k = 0; k < 5; k++) doom_gun_px(g, tx, ty + k, (k < 2) ? PAL_DSTEEL : PAL_DGRAY);
    }

    // ---- Grip: from the slide base down toward the viewer ----
    {
        float gdx = Gx - Bx, gdy = Gy - By, glen = sqrtf(gdx * gdx + gdy * gdy);
        for (int y = 0; y < DOOM_GUN_H; y++) {
            for (int x = 0; x < DOOM_GUN_W; x++) {
                float wx = x - Bx, wy = y - By;
                float t = (wx * gdx + wy * gdy) / (glen * glen);
                if (t < 0.0f || t > 1.0f) continue;
                float s = (gdx * wy - gdy * wx) / glen;
                float hw = 7.0f - 2.0f * t;
                if (s < -hw || s > hw) continue;
                uint8_t idx = PAL_DBROWN;
                if (s <= -hw + 1.5f)         idx = PAL_BROWN;
                else if (s >= hw - 2.0f)     idx = PAL_BLACK;
                else if (((x + y) % 3) == 0) idx = PAL_DGRAY;   // checkering
                doom_gun_px(g, x, y, idx);
            }
        }
    }

    // ---- Hand / fist wrapping the grip (prominent, on top) ----
    {
        float hdx = Gx - Bx, hdy = Gy - By, hlen = sqrtf(hdx * hdx + hdy * hdy);
        for (int y = 0; y < DOOM_GUN_H; y++) {
            for (int x = 0; x < DOOM_GUN_W; x++) {
                float wx = x - Bx, wy = y - By;
                float t = (wx * hdx + wy * hdy) / (hlen * hlen);
                if (t < 0.18f || t > 1.0f) continue;            // start below the body
                float s = (hdx * wy - hdy * wx) / hlen;
                float hw = 10.0f - 2.5f * t;
                if (s < -hw || s > hw) continue;
                uint8_t idx = PAL_FLESH;
                if (s >= hw - 2.0f)      idx = PAL_DFLESH;   // shadow edge
                else if ((y % 5) == 0)   idx = PAL_DFLESH;   // finger grooves
                doom_gun_px(g, x, y, idx);
            }
        }
        // Thumb on the near side of the grip
        for (int y = 58; y < 70; y++)
            for (int x = (int)Bx - 12; x <= (int)Bx - 8; x++)
                doom_gun_px(g, x, y, (y % 2) ? PAL_FLESH : PAL_DFLESH);
    }
}

// Allocate + generate all textures, sprites and the bg gradient. Returns false
// on any allocation failure (caller frees + bails to the menu).
static bool doom_gen_assets(void)
{
    // Ceiling/floor gradient (static across the run)
    for (int y = 0; y < DOOM_VIEW_H; y++) {
        if (y < DOOM_HORIZON) {
            int t = (y * 256) / DOOM_HORIZON;                 // Top(dark) -> horizon
            doom_bg_row[y] = doom_blend565(PAL_CEIL_LO, PAL_CEIL_HI, t);
        } else {
            int t = ((y - DOOM_HORIZON) * 256) / (DOOM_VIEW_H - DOOM_HORIZON);
            doom_bg_row[y] = doom_blend565(PAL_FLOOR_HI, PAL_FLOOR_LO, t); // Horizon -> bottom(dark)
        }
    }

    for (int i = 0; i < DOOM_NUM_TEX; i++) doom_tex[i] = NULL;

    // Textures for the tile ids actually used by the map
    const uint8_t used[] = {1, 2, 3, 4, DOOM_TILE_EXIT};
    for (size_t i = 0; i < sizeof(used); i++) {
        uint8_t id = used[i];
        doom_tex[id] = heap_caps_malloc(DOOM_TEX_SIZE * DOOM_TEX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!doom_tex[id]) return false;
    }
    doom_gen_brick(doom_tex[1]);
    doom_gen_stone(doom_tex[2], false);
    doom_gen_metal(doom_tex[3]);
    doom_gen_stone(doom_tex[4], true);
    doom_gen_exit(doom_tex[DOOM_TILE_EXIT]);

    for (int t = 0; t < DOOM_ENEMY_TYPES; t++) {
        for (int f = 0; f < 2; f++) {
            doom_demon_spr[t][f] = heap_caps_malloc(DOOM_SPR * DOOM_SPR, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!doom_demon_spr[t][f]) return false;
            bool atk = (f == 1);
            if (t == 0)      doom_gen_imp(doom_demon_spr[t][f], atk);   // red imp
            else if (t == 1) doom_gen_caco(doom_demon_spr[t][f], atk);  // cacodemon
            else             doom_gen_human(doom_demon_spr[t][f], atk); // pale knife humanoid
        }
    }

    doom_gun = heap_caps_malloc(DOOM_GUN_W * DOOM_GUN_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!doom_gun) return false;
    doom_gen_gun(doom_gun);

    return true;
}

// Free all generated textures, sprites and the gun (NULL-safe, so it also tidies
// up after a partial allocation failure). Pairs with doom_gen_assets.
static void doom_free_assets(void)
{
    for (int i = 0; i < DOOM_NUM_TEX; i++) {
        if (doom_tex[i]) { heap_caps_free(doom_tex[i]); doom_tex[i] = NULL; }
    }
    for (int t = 0; t < DOOM_ENEMY_TYPES; t++) {
        for (int f = 0; f < 2; f++) {
            if (doom_demon_spr[t][f]) { heap_caps_free(doom_demon_spr[t][f]); doom_demon_spr[t][f] = NULL; }
        }
    }
    if (doom_gun) { heap_caps_free(doom_gun); doom_gun = NULL; }
}

/* ---- Run reset ------------------------------------------------------------- */

// Reset all per-run state for the current level: player pose, full health, ammo
// scaled to the swarm size, cleared flags, and the level's enemies spawned from
// doom_spawn_list. Also (re)shows the "Find the exit!" hint on level 1.
static void doom_reset_run(void)
{
    doom_posX = doom_start_x + 0.5f;
    doom_posY = doom_start_y + 0.5f;
    doom_dirX = 1.0f; doom_dirY = 0.0f;          // Facing +X
    doom_planeX = 0.0f; doom_planeY = 0.66f;     // ~66 deg FOV

    // Refill health + ammo and reset transient combat / animation state
    doom_health = DOOM_START_HEALTH;
    // Ammo scales with the swarm but sub-linearly, so high levels force you to
    // conserve / rush the exit rather than wipe out every enemy
    doom_ammo = DOOM_START_AMMO + doom_spawn_n * DOOM_AMMO_PER_ENEMY;
    doom_bob_phase = 0.0f;
    doom_muzzle_ms = 0;
    doom_hurt_ms = 0;
    doom_moved = false;
    // Seed from the live button so the menu's select-to-enter (still held) is
    // not seen as a fresh fire edge on the first tick
    doom_prev_select = gpio_select_btn_held;
    doom_fire_last = 0;

    // Clear the per-run flags so the tick loop resumes and the HUD repaints
    doom_game_over = false;
    doom_game_won = false;
    doom_state_handled = false;
    doom_hud_dirty = true;

    // Spawn this level's enemies from the generated list, all at full (scaled) health
    doom_enemy_count = doom_spawn_n;
    if (doom_enemy_count > DOOM_MAX_ENEMIES) doom_enemy_count = DOOM_MAX_ENEMIES;
    for (int i = 0; i < doom_enemy_count; i++) {
        doom_enemies[i].x = doom_spawn_list[i].x + 0.5f;
        doom_enemies[i].y = doom_spawn_list[i].y + 0.5f;
        doom_enemies[i].type = doom_spawn_list[i].type;
        doom_enemies[i].health = doom_enemy_hp;
        doom_enemies[i].state = DOOM_E_ALIVE;
        doom_enemies[i].near_attack = false;
        doom_enemies[i].hurt_ms = 0;
        doom_enemies[i].dying_ms = 0;
        doom_enemies[i].last_attack = 0;
    }

    // Level 1 only: show the "Find the exit!" hint for the first ~2 seconds
    if (doom_hint_label) {
        if (doom_level == 1) {
            lv_obj_remove_flag(doom_hint_label, LV_OBJ_FLAG_HIDDEN);
            doom_hint_until = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
            doom_hint_active = true;
        } else {
            lv_obj_add_flag(doom_hint_label, LV_OBJ_FLAG_HIDDEN);
            doom_hint_active = false;
        }
    }
}

// Number of enemies not fully dead (alive or mid-death-animation) - drives the
// HUD readout and the "clear them all" win condition.
static int doom_alive_count(void)
{
    int n = 0;
    for (int i = 0; i < doom_enemy_count; i++) {
        if (doom_enemies[i].state != DOOM_E_DEAD) n++;
    }
    return n;
}

/* ---- Input / movement ------------------------------------------------------ */
// Turn the camera by angle a: rotate the direction and camera-plane vectors
// together (small per-tick steps, so plain trig per call is fine - no table).
static void doom_rotate(float a)
{
    float cs = cosf(a), sn = sinf(a);
    float odx = doom_dirX;
    doom_dirX = doom_dirX * cs - doom_dirY * sn;
    doom_dirY = odx * sn + doom_dirY * cs;
    float opx = doom_planeX;
    doom_planeX = doom_planeX * cs - doom_planeY * sn;
    doom_planeY = opx * sn + doom_planeY * cs;
}

// Move with per-axis wall collision so you slide along a wall instead of sticking;
// the margin keeps the camera off the wall face.
static void doom_try_move(float dx, float dy)
{
    // Try X: only commit if the cell a small margin ahead in x is not a wall
    float nx = doom_posX + dx;
    float cx = nx + (dx > 0 ? DOOM_WALL_MARGIN : -DOOM_WALL_MARGIN);
    if (!doom_is_wall((int)cx, (int)doom_posY)) doom_posX = nx;

    // ...then Y independently, so a blocked axis still lets you slide along the other
    float ny = doom_posY + dy;
    float cy = ny + (dy > 0 ? DOOM_WALL_MARGIN : -DOOM_WALL_MARGIN);
    if (!doom_is_wall((int)doom_posX, (int)cy)) doom_posY = ny;
}

// Fire the weapon: rate-limited + ammo-gated. A hitscan ray marches straight
// ahead and the first live enemy within radius takes damage (a wall stops it).
static void doom_fire(void)
{
    TickType_t now = xTaskGetTickCount();
    if (now - doom_fire_last < pdMS_TO_TICKS(DOOM_FIRE_COOLDOWN_MS)) return;
    if (doom_ammo <= 0) return;

    doom_fire_last = now;
    doom_ammo--;
    doom_muzzle_ms = DOOM_MUZZLE_MS;
    doom_hud_dirty = true;

    // Ray-march straight ahead; first enemy within radius (and not behind a wall) is hit
    for (float t = 0.0f; t < DOOM_HITSCAN_RANGE; t += 0.08f) {
        float px = doom_posX + doom_dirX * t;
        float py = doom_posY + doom_dirY * t;
        if (doom_is_wall((int)px, (int)py)) return; // Blocked by a wall
        for (int i = 0; i < doom_enemy_count; i++) {
            doom_enemy_t *e = &doom_enemies[i];
            if (e->state != DOOM_E_ALIVE) continue;
            float dx = px - e->x, dy = py - e->y;
            if (dx * dx + dy * dy < DOOM_ENEMY_RADIUS * DOOM_ENEMY_RADIUS) {
                e->health -= DOOM_GUN_DMG;
                e->hurt_ms = 120;
                if (e->health <= 0) {
                    e->state = DOOM_E_DYING;
                    e->dying_ms = DOOM_ENEMY_DEATH_MS;
                }
                return;
            }
        }
    }
}

// Sample the held buttons each tick: left/right turn, up/down move (with
// collision), and select fires once per press (rising edge).
static void doom_input(void)
{
    doom_moved = false;

    // Facing +X on a y-down map: turning left must rotate dir toward -Y, which
    // is a negative angle (doom_rotate(+a) rotates toward +Y = the player's right)
    if (gpio_left_btn_held)  doom_rotate(-DOOM_ROT_SPEED);
    if (gpio_right_btn_held) doom_rotate(DOOM_ROT_SPEED);

    // Move forward/back along the facing direction (with wall collision)
    float mv = 0.0f;
    if (gpio_up_btn_held)   mv += DOOM_MOVE_SPEED;
    if (gpio_down_btn_held) mv -= DOOM_MOVE_SPEED;
    if (mv != 0.0f) {
        doom_try_move(doom_dirX * mv, doom_dirY * mv);
        doom_moved = true;
    }

    // Fire once per press: edge-detect against last tick's select state
    bool sel = gpio_select_btn_held;
    if (sel && !doom_prev_select) doom_fire();
    doom_prev_select = sel;
}

/* ---- Enemy AI -------------------------------------------------------------- */

// Advance every enemy one tick, then evaluate the win conditions. Per enemy: tick
// the hit-flash and death timers, and if alive, chase the player and attack on a
// cooldown once adjacent.
static void doom_update_enemies(uint32_t dt_ms)
{
    TickType_t now = xTaskGetTickCount();

    for (int i = 0; i < doom_enemy_count; i++) {
        doom_enemy_t *e = &doom_enemies[i];
        if (e->state == DOOM_E_DEAD) continue;

        // Tick down the white hit-flash
        if (e->hurt_ms > 0) e->hurt_ms = (e->hurt_ms > dt_ms) ? (uint16_t)(e->hurt_ms - dt_ms) : 0;

        // Dying: run the death animation, then mark fully dead (drops the live count)
        if (e->state == DOOM_E_DYING) {
            if (e->dying_ms > dt_ms) e->dying_ms = (uint16_t)(e->dying_ms - dt_ms);
            else { e->state = DOOM_E_DEAD; doom_hud_dirty = true; } // refresh the ENEMY count
            continue;
        }

        // Alive
        float dx = doom_posX - e->x, dy = doom_posY - e->y;
        float d = sqrtf(dx * dx + dy * dy);
        e->near_attack = (d < DOOM_ENEMY_ATTACK_RANGE);

        // Within sight: chase the player until adjacent, then attack on a cooldown
        if (d < DOOM_ENEMY_SIGHT && d > 0.001f) {
            if (d > DOOM_ENEMY_ATTACK_RANGE) {
                // Advance toward the player, per-axis so it slides along walls
                float ux = dx / d, uy = dy / d;
                float nx = e->x + ux * doom_enemy_speed;
                float ny = e->y + uy * doom_enemy_speed;
                if (!doom_is_wall((int)nx, (int)e->y)) e->x = nx;
                if (!doom_is_wall((int)e->x, (int)ny)) e->y = ny;
            } else if (now - e->last_attack >= pdMS_TO_TICKS(DOOM_ENEMY_ATTACK_COOLDOWN_MS)) {
                e->last_attack = now;
                doom_health -= DOOM_ENEMY_DMG;
                doom_hurt_ms = DOOM_HURT_FLASH_MS; // flash the HUD red on the hit
                doom_hud_dirty = true;
                if (doom_health <= 0) {
                    doom_health = 0;
                    doom_game_over = true;
                }
            }
        }
    }

    // Win on either objective: reach the exit door, or clear every imp. Skipped
    // if a lethal hit landed this tick, so a simultaneous death takes priority.
    // The clear-all win requires the level to actually have had enemies.
    if (!doom_game_over) {
        int fx = (int)(doom_posX + doom_dirX * 0.6f);
        int fy = (int)(doom_posY + doom_dirY * 0.6f);
        if (doom_tile(fx, fy) == DOOM_TILE_EXIT) doom_game_won = true;
        if (doom_enemy_count > 0 && doom_alive_count() == 0) doom_game_won = true;
    }
}

/* ---- Rendering ------------------------------------------------------------- */

// The raycaster core. For each screen column, cast one ray from the player and
// DDA-step through the tile grid until it hits a wall, then draw a vertical
// textured slice sized by 1/distance (near walls tall, far walls short). EW vs NS
// faces and far distances are darkened for depth, and the perpendicular hit
// distance is stored in doom_zbuffer[x] so sprites can be occluded by nearer walls.
// Ceiling and floor fill the gaps above/below the slice from a precomputed gradient.
static void doom_render_walls(void)
{
    for (int x = 0; x < DOOM_VIEW_W; x++) {
        // Ray direction for this column: camera dir + plane * cameraX, where
        // cameraX runs from -1 (left screen edge) to +1 (right edge)
        float cameraX = 2.0f * x / DOOM_VIEW_W - 1.0f;
        float rdx = doom_dirX + doom_planeX * cameraX;
        float rdy = doom_dirY + doom_planeY * cameraX;

        // DDA setup: the tile we start in, and how far along the ray it is from
        // one x/y grid line to the next (1 / ray component)
        int mapX = (int)doom_posX, mapY = (int)doom_posY;
        float ddx = (rdx == 0.0f) ? 1e30f : fabsf(1.0f / rdx);
        float ddy = (rdy == 0.0f) ? 1e30f : fabsf(1.0f / rdy);

        // Step direction (+/-1) per axis and the ray length to the first grid line
        int stepX, stepY;
        float sideX, sideY;
        if (rdx < 0) { stepX = -1; sideX = (doom_posX - mapX) * ddx; }
        else         { stepX = 1;  sideX = (mapX + 1.0f - doom_posX) * ddx; }
        if (rdy < 0) { stepY = -1; sideY = (doom_posY - mapY) * ddy; }
        else         { stepY = 1;  sideY = (mapY + 1.0f - doom_posY) * ddy; }

        // Walk the grid, always advancing whichever side line is nearer, until the
        // ray lands in a solid tile. side = 0 means an NS face was hit, 1 an EW face.
        // guard caps the walk so a degenerate map can never hang the loop.
        int side = 0, tile = DOOM_TILE_BORDER, guard = 0;
        while (guard++ < 64) {
            if (sideX < sideY) { sideX += ddx; mapX += stepX; side = 0; }
            else               { sideY += ddy; mapY += stepY; side = 1; }
            tile = doom_tile(mapX, mapY);
            if (tile != DOOM_TILE_EMPTY) break;
        }

        // Perpendicular (not Euclidean) distance to the wall - avoids fisheye.
        // Clamp it so a point-blank wall can't blow up the slice height; stash it
        // in the z-buffer so sprites can be occluded by this column's wall.
        float perp = (side == 0) ? (sideX - ddx) : (sideY - ddy);
        if (perp < DOOM_PERP_MIN) perp = DOOM_PERP_MIN;
        doom_zbuffer[x] = perp;

        // Slice height = viewport / distance (near walls fill the screen); centre
        // it on the horizon, then clip the drawn span [ds, de] to the viewport
        int lineH = (int)(DOOM_VIEW_H / perp);
        if (lineH < 1) lineH = 1;
        int drawStart = -lineH / 2 + DOOM_HORIZON;
        int drawEnd = lineH / 2 + DOOM_HORIZON;
        int ds = drawStart < 0 ? 0 : drawStart;
        int de = drawEnd >= DOOM_VIEW_H ? DOOM_VIEW_H - 1 : drawEnd;

        // Exactly where along the tile the ray hit -> the texture column to sample
        // (fractional part). The flips stop textures mirroring on opposite faces.
        float wallX = (side == 0) ? (doom_posY + perp * rdy) : (doom_posX + perp * rdx);
        wallX -= floorf(wallX);
        int texX = (int)(wallX * DOOM_TEX_SIZE);
        if (side == 0 && rdx > 0) texX = DOOM_TEX_SIZE - texX - 1;
        if (side == 1 && rdy < 0) texX = DOOM_TEX_SIZE - texX - 1;
        if (texX < 0) texX = 0;
        if (texX >= DOOM_TEX_SIZE) texX = DOOM_TEX_SIZE - 1;

        // Texture for the hit tile (fall back to brick if a tile id has none)
        uint8_t *tex = (tile < DOOM_NUM_TEX && doom_tex[tile]) ? doom_tex[tile] : doom_tex[1];

        int shadeLvl = (side == 1) ? 1 : 0;        // EW faces darker than NS
        if (perp > 6.0f) shadeLvl++;               // Distance fog
        if (perp > 11.0f) shadeLvl++;

        // Fixed-point texture sampling (no per-pixel divide)
        uint32_t step = ((uint32_t)DOOM_TEX_SIZE << 16) / (uint32_t)lineH;
        int startOff = ds - DOOM_HORIZON + lineH / 2; // >= 0
        if (startOff < 0) startOff = 0;
        uint32_t texPos = (uint32_t)startOff * step;

        uint16_t *col = doom_fb + x;               // Column base; stride = DOOM_VIEW_W

        for (int y = 0; y < ds; y++) col[y * DOOM_VIEW_W] = doom_bg_row[y]; // Ceiling
        for (int y = ds; y <= de; y++) {
            int texY = (int)((texPos >> 16) & (DOOM_TEX_SIZE - 1));
            texPos += step;
            uint16_t c = doom_palette[tex[texY * DOOM_TEX_SIZE + texX]];
            if (shadeLvl) c = doom_shade(c, shadeLvl);
            col[y * DOOM_VIEW_W] = c;
        }
        for (int y = de + 1; y < DOOM_VIEW_H; y++) col[y * DOOM_VIEW_W] = doom_bg_row[y]; // Floor
    }
}

// Billboarded enemies. Each is transformed by the inverse camera matrix into a
// screen x + depth, drawn far-to-near (painter's order), scaled by 1/depth, and
// clipped per column against doom_zbuffer so walls hide enemies behind them.
// Transparent texels are skipped; a hit-flash whitens the sprite and dying
// enemies shrink as they fade out.
static void doom_render_sprites(void)
{
    // Collect drawable enemies, far-to-near (painter's order)
    int order[DOOM_MAX_ENEMIES], n = 0;
    for (int i = 0; i < doom_enemy_count; i++) {
        if (doom_enemies[i].state == DOOM_E_DEAD) continue;
        float dx = doom_posX - doom_enemies[i].x, dy = doom_posY - doom_enemies[i].y;
        doom_enemies[i].dist = dx * dx + dy * dy;
        order[n++] = i;
    }
    for (int a = 1; a < n; a++) {                  // Insertion sort, dist desc
        int key = order[a];
        float kd = doom_enemies[key].dist;
        int b = a - 1;
        while (b >= 0 && doom_enemies[order[b]].dist < kd) { order[b + 1] = order[b]; b--; }
        order[b + 1] = key;
    }

    // Inverse determinant of the camera matrix - maps a world offset into camera
    // space (sideways position + depth) in the loop below
    float invDet = 1.0f / (doom_planeX * doom_dirY - doom_dirX * doom_planeY);

    for (int k = 0; k < n; k++) {
        doom_enemy_t *e = &doom_enemies[order[k]];
        // Enemy position relative to the player, transformed into camera space:
        // tx = sideways offset, ty = depth (how far into the screen it is)
        float spx = e->x - doom_posX, spy = e->y - doom_posY;
        float tx = invDet * (doom_dirY * spx - doom_dirX * spy);
        float ty = invDet * (-doom_planeY * spx + doom_planeX * spy); // Depth
        if (ty <= 0.05f) continue;                 // Behind the camera

        // Project to a screen column, and a size that scales with 1/depth (square)
        int screenX = (int)((DOOM_VIEW_W / 2) * (1.0f + tx / ty));
        int spriteH = abs((int)(DOOM_VIEW_H / ty));
        int spriteW = spriteH;
        if (e->state == DOOM_E_DYING) {            // Shrink as it dies
            int sc = (e->dying_ms * 100) / DOOM_ENEMY_DEATH_MS;
            spriteH = spriteH * sc / 100;
            spriteW = spriteW * sc / 100;
        }
        // Cap the projected size: there is no player-enemy collision, so walking
        // onto an enemy makes 1/depth explode and would spin a multi-million-
        // iteration draw loop. A sprite this tall already overfills the viewport.
        if (spriteH > DOOM_VIEW_H * 2) spriteH = DOOM_VIEW_H * 2;
        if (spriteW > DOOM_VIEW_H * 2) spriteW = DOOM_VIEW_H * 2;
        if (spriteH < 1 || spriteW < 1) continue;

        // Billboard top-left, plus the frame to draw (attack/dying pose), whether
        // it is hit-flashing, and a distance shade for far enemies
        int startY = -spriteH / 2 + DOOM_HORIZON;
        int startX = -spriteW / 2 + screenX;
        bool flash = (e->hurt_ms > 0);
        int et = (e->type < DOOM_ENEMY_TYPES) ? e->type : 0;
        int ef = (e->near_attack || e->state == DOOM_E_DYING) ? 1 : 0;
        const uint8_t *spr = doom_demon_spr[et][ef];
        int shadeLvl = (ty > 7.0f) ? 1 : 0;

        // Draw column by column: skip columns off-screen or hidden behind a nearer
        // wall (z-test), map screen->texture coords, and blit the non-transparent
        // texels (white while hit-flashing, distance-shaded otherwise)
        for (int sx = 0; sx < spriteW; sx++) {
            int px = startX + sx;
            if (px < 0 || px >= DOOM_VIEW_W) continue;
            if (ty >= doom_zbuffer[px]) continue;  // Occluded by a wall
            int texX = sx * DOOM_SPR / spriteW;
            for (int sy = 0; sy < spriteH; sy++) {
                int py = startY + sy;
                if (py < 0 || py >= DOOM_VIEW_H) continue;
                int texY = sy * DOOM_SPR / spriteH;
                uint8_t pi = spr[texY * DOOM_SPR + texX];
                if (pi == PAL_KEY) continue;
                uint16_t c = flash ? doom_palette[PAL_WHITE] : doom_palette[pi];
                if (!flash && shadeLvl) c = doom_shade(c, shadeLvl);
                doom_fb[py * DOOM_VIEW_W + px] = c;
            }
        }
    }
}

// Fill a clipped solid disc into the framebuffer (used for the muzzle flash).
static void doom_fill_disc(int cx, int cy, int r, uint8_t pi)
{
    uint16_t c = doom_palette[pi];
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= DOOM_VIEW_H) continue;
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int px = cx + dx;
            if (px < 0 || px >= DOOM_VIEW_W) continue;
            doom_fb[py * DOOM_VIEW_W + px] = c;
        }
    }
}

// Draw the weapon at the bottom-centre with a walking bob, plus a three-ring
// muzzle-flash burst at the barrel tip while firing.
static void doom_render_gun(void)
{
    // Walk-bob offset; the gun sits bottom-centre, nudged right (held in hand)
    int bobx = (int)(sinf(doom_bob_phase) * 4.0f);
    int boby = (int)(fabsf(cosf(doom_bob_phase)) * 3.0f);
    int gx = (DOOM_VIEW_W - DOOM_GUN_W) / 2 + DOOM_GUN_OFF_X + bobx;
    int gy = DOOM_VIEW_H - DOOM_GUN_H + DOOM_GUN_OFF_Y + boby;

    for (int sy = 0; sy < DOOM_GUN_H; sy++) {
        int py = gy + sy;
        if (py < 0 || py >= DOOM_VIEW_H) continue;
        for (int sx = 0; sx < DOOM_GUN_W; sx++) {
            int px = gx + sx;
            if (px < 0 || px >= DOOM_VIEW_W) continue;
            uint8_t pi = doom_gun[sy * DOOM_GUN_W + sx];
            if (pi == PAL_KEY) continue;
            doom_fb[py * DOOM_VIEW_W + px] = doom_palette[pi];
        }
    }

    if (doom_muzzle_ms > 0) {
        // Burst at the barrel tip (top of the gun sprite)
        int fx = gx + DOOM_GUN_MUZZLE_SX;
        int fy = gy + DOOM_GUN_MUZZLE_SY;
        doom_fill_disc(fx, fy, 11, PAL_ORANGE);
        doom_fill_disc(fx, fy, 7, PAL_YELLOW);
        doom_fill_disc(fx, fy, 3, PAL_WHITE);
    }
}

// Bounds-checked pixel write into the view framebuffer.
static inline void doom_fb_set(int x, int y, uint16_t c)
{
    if (x >= 0 && x < DOOM_VIEW_W && y >= 0 && y < DOOM_VIEW_H) doom_fb[y * DOOM_VIEW_W + x] = c;
}

// Tiny white crosshair at the aim point. The hitscan travels straight down the
// camera axis, so a shot always lands at the centre column on the horizon row.
static void doom_render_crosshair(void)
{
    int cx = DOOM_VIEW_W / 2;
    int cy = DOOM_HORIZON;
    uint16_t w = doom_palette[PAL_WHITE];
    uint16_t k = doom_palette[PAL_BLACK];

    // Dark halo flanking each arm so the cross stays visible on light walls
    for (int d = 2; d <= 4; d++) {
        doom_fb_set(cx + d, cy - 1, k); doom_fb_set(cx + d, cy + 1, k);
        doom_fb_set(cx - d, cy - 1, k); doom_fb_set(cx - d, cy + 1, k);
        doom_fb_set(cx - 1, cy + d, k); doom_fb_set(cx + 1, cy + d, k);
        doom_fb_set(cx - 1, cy - d, k); doom_fb_set(cx + 1, cy - d, k);
    }
    doom_fb_set(cx + 5, cy, k); doom_fb_set(cx - 5, cy, k);
    doom_fb_set(cx, cy + 5, k); doom_fb_set(cx, cy - 5, k);

    // White arms with a small centre gap, plus a centre dot
    for (int d = 2; d <= 4; d++) {
        doom_fb_set(cx + d, cy, w); doom_fb_set(cx - d, cy, w);
        doom_fb_set(cx, cy + d, w); doom_fb_set(cx, cy - d, w);
    }
    doom_fb_set(cx, cy, w);
}

// Compose one full frame into the PSRAM framebuffer (walls -> sprites -> crosshair
// -> gun), then flag the canvas so LVGL flushes it to the panel.
static void doom_render_all(void)
{
    doom_render_walls();
    doom_render_sprites();
    doom_render_crosshair();
    doom_render_gun();
    lv_obj_invalidate(doom_canvas);
}

// Refresh the bottom HUD line: level / health / ammo / live enemy count.
static void doom_update_hud(void)
{
    // Flash the stat line red briefly when an enemy hit just landed
    lv_color_t col = (doom_hurt_ms > 0) ? lv_color_hex(0xFF3030) : user_secondary_color;
    lv_obj_set_style_text_color(doom_hud_label, col, 0);

    char buf[48];
    snprintf(buf, sizeof(buf), "LVL %d   HP %d   AMMO %d   ENEMY %d",
             doom_level, doom_health, doom_ammo, doom_alive_count());
    lv_label_set_text(doom_hud_label, buf);
}

/* ---- Cleanup --------------------------------------------------------------- */

// Tear down the game: stop the timer, delete the LVGL objects, free the
// framebuffer + generated art, and clear the init-once gate so a re-entry rebuilds.
static void doom_cleanup(void)
{
    if (doom_timer) {
        lv_timer_del(doom_timer);
        doom_timer = NULL;
    }
    lv_obj_delete(doom_canvas);
    lv_obj_delete(doom_hud_label);
    lv_obj_delete(doom_overlay_label);
    lv_obj_delete(doom_hint_label);
    heap_caps_free(doom_fb);
    doom_free_assets();

    doom_canvas = doom_hud_label = doom_overlay_label = doom_hint_label = NULL;
    doom_hint_active = false;
    doom_fb = NULL;
    doom_init = false;
}

/* ---- Game tick ------------------------------------------------------------- */

// One game tick (~25 FPS, driven by the lv_timer): sample held-button input, step
// the enemy AI, advance the weapon-bob / muzzle-flash timers, render the frame and
// refresh the HUD if it changed. Idles once the level is won or lost (the page
// handler then shows the overlay and waits for input).
static void doom_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!doom_init) return;

    // Auto-hide the level-1 objective hint once its window elapses
    if (doom_hint_active && xTaskGetTickCount() >= doom_hint_until) {
        lv_obj_add_flag(doom_hint_label, LV_OBJ_FLAG_HIDDEN);
        doom_hint_active = false;
    }

    if (doom_game_over || doom_game_won) return;

    doom_input();
    doom_update_enemies(DOOM_FRAME_MS);

    if (doom_moved) doom_bob_phase += 0.5f;
    if (doom_muzzle_ms > 0) {
        doom_muzzle_ms = (doom_muzzle_ms > DOOM_FRAME_MS) ? (uint16_t)(doom_muzzle_ms - DOOM_FRAME_MS) : 0;
    }
    if (doom_hurt_ms > 0) {
        doom_hurt_ms = (doom_hurt_ms > DOOM_FRAME_MS) ? (uint16_t)(doom_hurt_ms - DOOM_FRAME_MS) : 0;
        if (doom_hurt_ms == 0) doom_hud_dirty = true; // flash over -> repaint HUD in normal colour
    }

    doom_render_all();

    if (doom_hud_dirty) {
        doom_update_hud();
        doom_hud_dirty = false;
    }
}

/* ---- Page handler ---------------------------------------------------------- */
// Clean up the game and return to the games menu, restoring its nav arrows.
static void doom_exit_to_menu(ui_menu_t *ui_menu, games_menu_t *games_menu)
{
    doom_cleanup();
    lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
    ui_menu->page = GAMES_PAGE;
}

// The page handler, called by lcd_task each frame while on GAMES_DOOM_PAGE. It has
// three phases:
//   - first entry (init-once gate): allocate the framebuffer, build the canvas +
//     HUD/overlay/hint labels, generate level 1 and start the game-tick timer;
//   - game over / level cleared: show the overlay and wait for a button to continue
//     (advance on a win, retry on death) or Home to leave;
//   - active play: only watch for Home/power to exit - movement and firing happen
//     in the timer callback from the raw held-button state.
void lcd_games_doom_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, games_menu_t *games_menu)
{
    POLYCAST5_USE_PSRAM_BSS static lv_draw_buf_t canvas_buf;

    if (!doom_init) {
        // If picking this page as a hotkey
        if (!lv_obj_has_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN)) {
            lcd_hotkey_save_page_as_hotkey(ui_menu); // Save as a hotkey
        }
        
        // Framebuffer in PSRAM (RGB565)
        size_t buf_size = (size_t)DOOM_VIEW_W * DOOM_VIEW_H * 2;
        doom_fb = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!doom_fb) {
            ESP_LOGE(TAG, "Failed to alloc PSRAM for Doom framebuffer");
            lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = GAMES_PAGE;
            return;
        }

        if (!doom_gen_assets()) {
            ESP_LOGE(TAG, "Failed to alloc PSRAM for Doom assets");
            doom_free_assets();
            heap_caps_free(doom_fb);
            doom_fb = NULL;
            lv_obj_remove_flag(games_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = GAMES_PAGE;
            return;
        }

        lv_draw_buf_init(&canvas_buf, DOOM_VIEW_W, DOOM_VIEW_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, doom_fb, buf_size);

        doom_canvas = lv_canvas_create(ACTIVE_SCR);
        lv_canvas_set_draw_buf(doom_canvas, &canvas_buf);
        lv_obj_set_size(doom_canvas, DOOM_VIEW_W, DOOM_VIEW_H);
        lv_obj_align(doom_canvas, LV_ALIGN_TOP_MID, 0, 0);

        // HUD line below the viewport
        doom_hud_label = lv_label_create(ACTIVE_SCR);
        lv_obj_set_style_text_font(doom_hud_label, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(doom_hud_label, user_secondary_color, 0);
        // Centre the HUD text vertically within the thin strip below the viewport
        int hud_strip = VER_RES - DOOM_VIEW_H;
        int hud_gap = (hud_strip - (int)lv_font_montserrat_12.line_height) / 2;
        if (hud_gap < 0) hud_gap = 0;
        lv_obj_align(doom_hud_label, LV_ALIGN_BOTTOM_MID, 0, -hud_gap);

        // Centered overlay for death / level-complete (hidden during play)
        doom_overlay_label = lv_label_create(ACTIVE_SCR);
        lv_label_set_text(doom_overlay_label, "");
        lv_obj_set_style_text_font(doom_overlay_label, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(doom_overlay_label, user_secondary_color, 0);
        lv_obj_set_style_text_align(doom_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(doom_overlay_label, LV_ALIGN_CENTER, 0, -10);
        lv_obj_add_flag(doom_overlay_label, LV_OBJ_FLAG_HIDDEN);

        // Level-1 objective hint at the top (shown for ~2s by doom_reset_run)
        doom_hint_label = lv_label_create(ACTIVE_SCR);
        lv_label_set_text(doom_hint_label, "Find the exit!");
        lv_obj_set_style_text_font(doom_hint_label, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(doom_hint_label, lv_color_white(), 0);
        lv_obj_align(doom_hint_label, LV_ALIGN_TOP_MID, 0, 3);
        lv_obj_add_flag(doom_hint_label, LV_OBJ_FLAG_HIDDEN);

        doom_high_level = doom_high_score_load(); // best = highest level ever cleared
        doom_level = 1;
        doom_generate_level();
        doom_reset_run();
        doom_render_all();
        doom_update_hud();

        doom_timer = lv_timer_create(doom_timer_cb, DOOM_FRAME_MS, NULL);
        doom_init = true;
    }

    // Death / win: freeze the world, show overlay, wait for input
    if (doom_game_over || doom_game_won) {
        if (!doom_state_handled) {
            // Clearing a higher level than ever before is a new record (updates
            // doom_high_level before the text is built so "Best" reflects it)
            bool new_best = doom_game_won && doom_record_cleared_level();
            char buf[96];
            const char *best_tag = new_best ? "\nNEW BEST!" : "";
            if (doom_game_won) {
                snprintf(buf, sizeof(buf), "LEVEL %d CLEARED\nBest: %d%s\nHome: menu   Else: next", doom_level, (int)doom_high_level, best_tag);
            } else {
                snprintf(buf, sizeof(buf), "YOU DIED\nLevel %d   Best: %d%s\nHome: menu   Else: retry", doom_level, (int)doom_high_level, best_tag);
            }
            lv_label_set_text(doom_overlay_label, buf);
            lv_obj_remove_flag(doom_overlay_label, LV_OBJ_FLAG_HIDDEN);
            doom_state_handled = true;
            doom_over_tick = xTaskGetTickCount();
        }

        // Grace so a button still held from the final moment is not consumed
        if (xTaskGetTickCount() - doom_over_tick < pdMS_TO_TICKS(600)) {
            return;
        }

        if (ui_btns->home_btn) {
            doom_exit_to_menu(ui_menu, games_menu);             // Home -> back to games menu
        } else if (ui_btns->pwr_btn) {
            doom_cleanup();
            lcd_transition_back(false, ui_menu);                // Power -> sleep
        } else if (ui_btns->up_btn || ui_btns->down_btn || ui_btns->left_btn ||
                   ui_btns->right_btn || ui_btns->select_btn) { // Any other button -> continue
            lv_obj_add_flag(doom_overlay_label, LV_OBJ_FLAG_HIDDEN);
            if (doom_game_won) doom_level++; // win -> next, harder level (death retries the same)
            doom_generate_level();
            doom_reset_run();
            doom_render_all();
            doom_update_hud();
        }
        return;
    }

    // Active play: movement/fire are sampled in the timer from raw held-state.
    // The 200ms poll only handles leaving the game.
    if (ui_btns->home_btn) {
        doom_exit_to_menu(ui_menu, games_menu);
    } else if (ui_btns->pwr_btn) {
        doom_cleanup();
        lcd_transition_back(false, ui_menu);
    }
}
