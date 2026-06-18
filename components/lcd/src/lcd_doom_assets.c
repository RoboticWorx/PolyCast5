#include "lcd_doom_assets.h"

/* RGB888 -> RGB565, matching LVGL's native 16-bit packing (lv_color_to_u16) */
#define DOOM_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Palette: order must match the enum in lcd_doom_assets.h
const uint16_t doom_palette[DOOM_PAL_SIZE] = {
    [PAL_KEY]      = DOOM_RGB(255,   0, 255), // transparency key
    [PAL_BLACK]    = DOOM_RGB(  0,   0,   0),
    [PAL_CEIL_HI]  = DOOM_RGB( 40,  40,  55),
    [PAL_CEIL_LO]  = DOOM_RGB( 14,  14,  24),
    [PAL_FLOOR_HI] = DOOM_RGB( 74,  58,  46),
    [PAL_FLOOR_LO] = DOOM_RGB( 30,  22,  18),
    [PAL_DGRAY]    = DOOM_RGB( 60,  60,  66),
    [PAL_GRAY]     = DOOM_RGB(110, 110, 116),
    [PAL_LGRAY]    = DOOM_RGB(170, 170, 176),
    [PAL_WHITE]    = DOOM_RGB(235, 235, 235),
    [PAL_DBROWN]   = DOOM_RGB( 70,  45,  25),
    [PAL_BROWN]    = DOOM_RGB(120,  80,  45),
    [PAL_LBROWN]   = DOOM_RGB(165, 120,  75),
    [PAL_MORTAR]   = DOOM_RGB(150, 150, 140),
    [PAL_DRED]     = DOOM_RGB( 90,  15,  15),
    [PAL_RED]      = DOOM_RGB(170,  30,  25),
    [PAL_BRED]     = DOOM_RGB(230,  50,  40),
    [PAL_MAROON]   = DOOM_RGB( 60,  10,  10),
    [PAL_PINK]     = DOOM_RGB(220, 120, 120),
    [PAL_DGREEN]   = DOOM_RGB( 20,  70,  30),
    [PAL_GREEN]    = DOOM_RGB( 40, 130,  55),
    [PAL_BGREEN]   = DOOM_RGB( 80, 200,  90),
    [PAL_TEAL]     = DOOM_RGB( 40, 180, 170),
    [PAL_DTEAL]    = DOOM_RGB( 20,  90,  90),
    [PAL_BLUE]     = DOOM_RGB( 40,  70, 160),
    [PAL_DBLUE]    = DOOM_RGB( 20,  35,  90),
    [PAL_YELLOW]   = DOOM_RGB(235, 210,  60),
    [PAL_ORANGE]   = DOOM_RGB(240, 140,  40),
    [PAL_STEEL]    = DOOM_RGB( 95, 100, 115),
    [PAL_DSTEEL]   = DOOM_RGB( 55,  60,  72),
    [PAL_FLESH]    = DOOM_RGB(200, 150, 110),
    [PAL_DFLESH]   = DOOM_RGB(140,  95,  70),
};

/* Note: the level map and enemy spawns are generated procedurally at runtime
 * (doom_generate_level in lcd_doom.c), so no static feature/spawn tables here. */
