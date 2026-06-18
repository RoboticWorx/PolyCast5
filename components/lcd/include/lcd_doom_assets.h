#ifndef LCD_DOOM_ASSETS_H
#define LCD_DOOM_ASSETS_H

#include <stdint.h>

/*
 * Static (flash/.rodata) data for the Doom raycaster: the colour palette plus
 * the map/spawn type definitions. The level map and enemy spawns are generated
 * procedurally each level at runtime (doom_generate_level in lcd_doom.c); wall
 * textures and sprites are likewise generated into PSRAM at game init.
 */

/* ---- Palette: 32 indexed colours -> RGB565 ---------------------------------
 * Index 0 is the sprite transparency key and is never blitted. */
#define DOOM_PAL_SIZE 32

enum {
    PAL_KEY = 0,   // Sprite transparency key (magenta, never drawn)
    PAL_BLACK,
    PAL_CEIL_HI,   // Ceiling near the horizon
    PAL_CEIL_LO,   // Ceiling at the top (darker)
    PAL_FLOOR_HI,  // Floor near the horizon
    PAL_FLOOR_LO,  // Floor at the bottom (darker)
    PAL_DGRAY,
    PAL_GRAY,
    PAL_LGRAY,
    PAL_WHITE,
    PAL_DBROWN,
    PAL_BROWN,
    PAL_LBROWN,
    PAL_MORTAR,
    PAL_DRED,
    PAL_RED,
    PAL_BRED,
    PAL_MAROON,
    PAL_PINK,
    PAL_DGREEN,
    PAL_GREEN,
    PAL_BGREEN,
    PAL_TEAL,
    PAL_DTEAL,
    PAL_BLUE,
    PAL_DBLUE,
    PAL_YELLOW,
    PAL_ORANGE,
    PAL_STEEL,
    PAL_DSTEEL,
    PAL_FLESH,
    PAL_DFLESH,
};

extern const uint16_t doom_palette[DOOM_PAL_SIZE];

/* ---- Map -------------------------------------------------------------------
 * Tile values: 0 = empty, 1..8 = solid wall (texture id), 9 = exit door.
 * The map is generated each level at runtime (see doom_generate_level). */
#define DOOM_MAP_W 24
#define DOOM_MAP_H 24

#define DOOM_TILE_EMPTY 0
#define DOOM_TILE_BORDER 2
#define DOOM_TILE_EXIT  9

/* ---- Enemy spawns (generated at runtime into a doom_spawn_t list) ---------- */
#define DOOM_MAX_ENEMIES 32

typedef struct {
    uint8_t x;
    uint8_t y;
    uint8_t type; // 0 = red imp, 1 = cacodemon, 2 = knife humanoid
} doom_spawn_t;

#endif // LCD_DOOM_ASSETS_H
