#include "lcd_anim_matrix.h"

#ifdef POLYCAST5_EN_MATRIX_RAIN_ANIM

#include <string.h>
#include <math.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR (POLYCAST5_USE_PSRAM_BSS)

#include "lvgl.h"

#include "lcd_anim_matrix_glyphs.h" // s_glyph_src, MTX_GLYPH_COUNT/SRC_W/SRC_H

static const char *TAG = "LCD_MATRIX";

// Output canvas (physical LCD area)
#define MTX_CANVAS_W 240
#define MTX_CANVAS_H 135

// 6 x 9 tiles divide 240 x 135 exactly; 6 px is the measured pitch
#define MTX_CELL_W   6
#define MTX_CELL_H   9
#define MTX_COLS     (MTX_CANVAS_W / MTX_CELL_W) // 40
#define MTX_ROWS     (MTX_CANVAS_H / MTX_CELL_H) // 15

// Art fills cols 0..4 / rows 1..7; col 5 and rows 0,8 are gutters
#define MTX_GLYPH_W  (MTX_SRC_W / 2) // 5
#define MTX_GLYPH_H  (MTX_SRC_H / 2) // 7
#define MTX_GLYPH_X0 0
#define MTX_GLYPH_Y0 1

#define MTX_STREAMS  2  // Independent falling streams per column (brightness = max)
#define MTX_LEVELS   64 // Brightness quantisation; see mtx_build_palette()
#define MTX_FRAME_PERIOD 50 // ~20 fps: the full-screen flush floor, not a preference
#define MTX_GLOW_LEVEL 52 // At or above this a head renders from the dilated ROM

// Falloff LUT over d in [-0.375, +23.625) rows, d = distance behind the head
#define MTX_FALL_N    384 // Must outlast the longest tail or the top row snaps to black
#define MTX_FALL_BIAS 96 // Q8 rows: 0.375 * 256, the negative half of the domain
// A dormant stream parks here. The LUT reaches back MTX_FALL_N/16 = 24 rows, so 40 rows
// below the screen puts every row past its end and the stream contributes nothing
#define MTX_HEAD_PARKED (((int32_t)MTX_ROWS + 40) << 8)

static lv_obj_t     *s_canvas;    // Our LVGL canvas (parented to the homescreen); NULL = uninitialized
static lv_draw_buf_t s_draw_buf;  // LVGL descriptor wrapping s_pixels as RGB565
static uint8_t      *s_pixels;    // PSRAM framebuffer, MTX_CANVAS_W * H * 2 bytes
static size_t        s_buf_size;  // Byte size of s_pixels
static int           s_stride_px; // Row stride in PIXELS (the draw-buf stride is in bytes)
static lv_timer_t   *s_timer;     // Drives one sim step + one render every MTX_FRAME_PERIOD
static uint32_t      s_last_tick; // lv_tick_get() at the previous frame, for time-based motion

// Two streams per column; a cell takes the brighter of the two
typedef struct {
    int32_t  head_q8;  // Head position in ROWS, Q8. Starts negative (above the screen)
    uint32_t rng;      // Per-stream xorshift32 state; must never be zero
    uint16_t step_q16; // Fall speed in rows per MILLISECOND, Q16
    int16_t  wait_ms;  // Milliseconds until respawn while dormant; <= 0 means now
    uint8_t  tail_cls; // 0..3, selects a row of s_falloff (tail length)
    uint8_t  dim;      // Column brightness scale 0..255
    uint8_t  mut_rate; // Glyph mutation threshold 0..255
    int8_t   head_row; // Last integer row the head lit; -1 = none yet
} mtx_stream_t;

// Aligned so s_stream[c] is one 32-byte cache line rather than straddling two
POLYCAST5_USE_PSRAM_BSS static mtx_stream_t s_stream[MTX_COLS][MTX_STREAMS]
        __attribute__((aligned(32)));

POLYCAST5_USE_PSRAM_BSS static uint8_t  s_cell_glyph[MTX_ROWS][MTX_COLS]; // Glyph index per cell
POLYCAST5_USE_PSRAM_BSS static uint8_t  s_cell_level[MTX_ROWS][MTX_COLS]; // Brightness 0..63 this frame
POLYCAST5_USE_PSRAM_BSS static uint16_t s_cell_prev[MTX_ROWS][MTX_COLS];  // Last painted (glyph << 8) | level
POLYCAST5_USE_PSRAM_BSS static uint8_t  s_falloff[4][MTX_FALL_N];         // Tail shape per class
// One uint16 per CELL row (9 per glyph, gutters included), 2 bits per pixel, LSB = leftmost
POLYCAST5_USE_PSRAM_BSS static uint16_t s_glyph_rom[MTX_GLYPH_COUNT][MTX_CELL_H];
POLYCAST5_USE_PSRAM_BSS static uint16_t s_glyph_glow[MTX_GLYPH_COUNT][MTX_CELL_H]; // Same, dilated one pixel

// Four RGB565 colours per level by coverage. Internal SRAM: hottest table here
static uint16_t s_pal[MTX_LEVELS][4];

/* ===========================================================================
 * Tuning tables
 * ========================================================================= */

// Rows per MILLISECOND, Q16. Primes, so no two columns ever re-phase
static const uint16_t MTX_STEP_Q16[16] = {
    617,  653,  691,  727,  769,  811,  853,  887,
    929,  971, 1019, 1061, 1103, 1151, 1201, 1249
};

static const uint8_t MTX_TAIL_PICK[8] = {0, 1, 1, 2, 2, 2, 3, 3}; // Weighted 1/2/3/2 draw
static const uint8_t MTX_TAIL_EXT[4]  = {5, 9, 13, 15}; // Trail rows past the last row: ~3x tau[], so edit both together

// Applied pre-quantisation, so a tier caps the whole ramp at ((255*tier)>>8)>>2 - giving
// 63/63/63/59/53/46/37/29, so the bottom three never glow. That reads as depth, not dimming
static const uint8_t MTX_DIM_TIER[8] = {255, 255, 255, 238, 215, 185, 150, 120};

// Dormant ms before re-entering. The occupancy knob: shorter fills the screen but
// eats the empty columns that give the rain its gaps
#define MTX_RESPAWN_WAIT_MS(rng) ((int16_t)(40 + ((rng) % 450)))

/* ===========================================================================
 * Init-time table building (all the float in this file lives here)
 * ========================================================================= */

// Green saturates first, red climbs throughout, blue stays 0 until t~0.45. Red hugs
// 0 low down or it reads olive-brown in RGB565
static void mtx_build_palette(void)
{
    // Fitted to the old frames, then lifted for the panel. Scale the middle stops to
    // retune brightness; the endpoints stay put
    static const float   stop_t[8]    = {0.00f, 0.15f, 0.30f, 0.45f, 0.60f, 0.75f, 0.88f, 1.00f};
    static const uint8_t stop_c[8][3] = {
        {  0,   0,   0}, {  0,  60,   0}, {  6, 120,   0}, { 26, 175,  10},
        { 60, 215,  40}, {100, 242,  75}, {150, 252, 125}, {205, 255, 195},
    };
    static const uint8_t alpha[4] = {0, 110, 195, 255}; // Coverage -> opacity; 1 is a stroke edge

    for (int l = 0; l < MTX_LEVELS; l++) {
        float t = (float)l / (float)(MTX_LEVELS - 1);

        int k = 0;
        while (k < 6 && t > stop_t[k + 1]) k++;
        float span = stop_t[k + 1] - stop_t[k];
        float f = (span > 0.0f) ? (t - stop_t[k]) / span : 0.0f;

        float cr = (float)stop_c[k][0] + ((float)stop_c[k + 1][0] - (float)stop_c[k][0]) * f;
        float cg = (float)stop_c[k][1] + ((float)stop_c[k + 1][1] - (float)stop_c[k][1]) * f;
        float cb = (float)stop_c[k][2] + ((float)stop_c[k + 1][2] - (float)stop_c[k][2]) * f;

        uint16_t pal16[4];
        for (int c = 0; c < 4; c++) {
            float a = (float)alpha[c] / 255.0f;
            uint8_t rr = (uint8_t)(cr * a + 0.5f);
            uint8_t gg = (uint8_t)(cg * a + 0.5f);
            uint8_t bb = (uint8_t)(cb * a + 0.5f);
            pal16[c] = lv_color_to_u16(lv_color_make(rr, gg, bb));
        }

        // Coverage 0 stays black at every level, so each tile self-clears as it paints
        for (int c = 0; c < 4; c++) {
            s_pal[l][c] = pal16[c];
        }
    }
}

// The negative domain is only 0.375 rows: it buys sub-cell glide, not a fade-in
static void mtx_build_falloff(void)
{
    static const float tau[4] = {1.1f, 2.0f, 3.2f, 5.0f}; // Decay length in rows

    for (int cls = 0; cls < 4; cls++) {
        for (int i = 0; i < MTX_FALL_N; i++) {
            // 16 is the Q8 step per entry, i.e. one entry per 1/16 row, and is the exact
            // inverse of the >> 4 that indexes this table in mtx_levels()
            float d = (float)(i * 16 - MTX_FALL_BIAS) / 256.0f; // rows
            float v;
            if (d < 0.0f) {
                float u = 1.0f + d / 0.375f; // 0.375 is MTX_FALL_BIAS/256: 0 at the far edge, 1 at the head
                v = u * u;
            } else {
                v = expf(-d / tau[cls]);
            }
            s_falloff[cls][i] = (uint8_t)(255.0f * v + 0.5f);
        }
    }
}

// Downsample the 10x14 art to the 5x7 2bpp ROM, then dilate it for the glow copy.
// Authoring at 2x is what gives the glyphs soft edges
static void mtx_build_glyphs(void)
{
    // n = set subpixels in a 2x2 source block (0..4) -> 2bpp coverage. n=1 maps to 0, so a
    // lone subpixel is dropped rather than rendered as a stray speck
    static const uint8_t cov_of_count[5] = {0, 0, 1, 2, 3};

    for (int g = 0; g < MTX_GLYPH_COUNT; g++) {
        // CELL space (6x9), not glyph space (5x7): the ROM holds the whole tile so mtx_paint
        // can blit every row, and this memset is what leaves the gutters black
        uint8_t cov[MTX_CELL_H][MTX_CELL_W];
        memset(cov, 0, sizeof(cov));

        // Output pixel (gx,gy) averages the four source pixels (2gx+dx, 2gy+dy)
        for (int gy = 0; gy < MTX_GLYPH_H; gy++) {
            for (int gx = 0; gx < MTX_GLYPH_W; gx++) {
                int n = 0;
                for (int dy = 0; dy < 2; dy++) {
                    uint16_t src = s_glyph_src[g][gy * 2 + dy];
                    for (int dx = 0; dx < 2; dx++) {
                        // Source art is MSB-first: 10 bits right-aligned in the uint16,
                        // bit 9 = column 0, so column x is bit (MTX_SRC_W - 1 - x)
                        if (src & (1u << (MTX_SRC_W - 1 - (gx * 2 + dx)))) n++;
                    }
                }
                // Offset the 5x7 art into the 6x9 tile; col 5 and rows 0,8 stay as gutters
                cov[MTX_GLYPH_Y0 + gy][MTX_GLYPH_X0 + gx] = cov_of_count[n];
            }
        }

        // Empty pixels beside a SOLID one (>= 2, never the anti-aliased fringe) light at
        // coverage 1. Every test reads cov and writes glow, so growth is exactly one pixel
        // deep and cannot cascade or depend on scan order. Clipped, so it never bleeds out
        uint8_t glow[MTX_CELL_H][MTX_CELL_W];
        memcpy(glow, cov, sizeof(glow));
        for (int y = 0; y < MTX_CELL_H; y++) {
            for (int x = 0; x < MTX_CELL_W; x++) {
                if (cov[y][x] != 0) continue;
                if ((y > 0              && cov[y - 1][x] >= 2) ||
                    (y < MTX_CELL_H - 1 && cov[y + 1][x] >= 2) ||
                    (x > 0              && cov[y][x - 1] >= 2) ||
                    (x < MTX_CELL_W - 1 && cov[y][x + 1] >= 2)) {
                    glow[y][x] = 1;
                }
            }
        }

        // Pack LSB-first: pixel x takes bits 2x..2x+1, so pixel 0 (leftmost) is the LOW
        // pair - the reverse of the source art above. 6 px = 12 live bits, 15..12 unused
        for (int y = 0; y < MTX_CELL_H; y++) {
            uint16_t bits = 0, gbits = 0;
            for (int x = 0; x < MTX_CELL_W; x++) {
                bits  |= (uint16_t)(cov[y][x]  & 3) << (2 * x);
                gbits |= (uint16_t)(glow[y][x] & 3) << (2 * x);
            }
            s_glyph_rom[g][y]  = bits;
            s_glyph_glow[g][y] = gbits;
        }
    }
}

/* ===========================================================================
 * Simulation
 * ========================================================================= */

static inline uint32_t mtx_xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

// Fresh parameters for a stream that is (re)entering the screen
static void mtx_respawn(mtx_stream_t *s)
{
    s->rng      = mtx_xorshift32(s->rng);
    s->step_q16 = MTX_STEP_Q16[s->rng & 15];
    s->tail_cls = MTX_TAIL_PICK[(s->rng >> 4) & 7];
    s->dim      = MTX_DIM_TIER[(s->rng >> 7) & 7];
    s->mut_rate = 24 + (uint8_t)((s->rng >> 10) & 63);
    s->head_q8  = -(int32_t)(256 + ((s->rng >> 16) & 0x3FF)); // 256 = 1.0 row in Q8, so -1.0 .. -5.0 rows
    s->head_row = -1;
}

// Seed everything so frame 1 is already a full screen of rain
static void mtx_seed(void)
{
    for (int c = 0; c < MTX_COLS; c++) {
        for (int k = 0; k < MTX_STREAMS; k++) {
            mtx_stream_t *s = &s_stream[c][k];

            // xorshift32 seeded with 0 emits 0 forever, and esp_random() can return 0
            uint32_t seed = esp_random();
            s->rng = seed ? seed : (0xA5A5A5A5u ^ (uint32_t)(c * MTX_STREAMS + k + 1));

            mtx_respawn(s);

            // Scatter the heads over the screen plus the approach. Span is this stream's
            // own finish threshold, so a seed can never start past it and park unseen
            s->rng     = mtx_xorshift32(s->rng);
            int32_t span = (int32_t)((MTX_ROWS + MTX_TAIL_EXT[s->tail_cls]) << 8);
            s->head_q8 = (int32_t)(s->rng % (uint32_t)(span + (5 << 8))) - (5 << 8);
            int start_row = s->head_q8 >> 8; // Level with the head; no catch-up to do
            s->head_row = (int8_t)(start_row < -1 ? -1 : start_row);

            s->rng     = mtx_xorshift32(s->rng); // Stagger stream 1 so columns differ
            s->wait_ms = (k == 1 && (s->rng & 1)) ? MTX_RESPAWN_WAIT_MS(s->rng) : 0;
            if (s->wait_ms > 0) s->head_q8 = MTX_HEAD_PARKED;
        }
    }

    for (int r = 0; r < MTX_ROWS; r++) {
        for (int c = 0; c < MTX_COLS; c++) {
            s_stream[c][0].rng = mtx_xorshift32(s_stream[c][0].rng);
            s_cell_glyph[r][c] = (uint8_t)(s_stream[c][0].rng & (MTX_GLYPH_COUNT - 1));
        }
    }
}

// Advance every stream, stamp a glyph at each row a head crosses, mutate occasionally
static void mtx_advance(uint32_t dt)
{
    for (int c = 0; c < MTX_COLS; c++) {
        for (int k = 0; k < MTX_STREAMS; k++) {
            mtx_stream_t *s = &s_stream[c][k];

            if (s->wait_ms > 0) {
                s->wait_ms -= (int16_t)dt;
                if (s->wait_ms > 0) continue; // Still dormant; parked, so it paints nothing
                mtx_respawn(s);
            }

            // Q16 rows/ms times ms lands in Q8 after the shift; no divide
            s->head_q8 += ((int32_t)s->step_q16 * (int32_t)dt) >> 8;

            int nr = s->head_q8 >> 8; // Arithmetic shift: a negative head floors, not truncates
            if (nr > MTX_ROWS) nr = MTX_ROWS; // Nothing to stamp past the last row
            while (s->head_row < nr) { // A while, not an if: a long dt crosses several rows
                s->head_row++;
                if ((unsigned)s->head_row < (unsigned)MTX_ROWS) {
                    s->rng = mtx_xorshift32(s->rng);
                    s_cell_glyph[s->head_row][c] = (uint8_t)(s->rng & (MTX_GLYPH_COUNT - 1));
                }
            }

            if (s->head_q8 > (int32_t)((MTX_ROWS + MTX_TAIL_EXT[s->tail_cls]) << 8)) {
                s->rng     = mtx_xorshift32(s->rng);
                s->wait_ms = MTX_RESPAWN_WAIT_MS(s->rng); // The only % in the hot path
                s->head_q8 = MTX_HEAD_PARKED;
                continue;
            }

            s->rng = mtx_xorshift32(s->rng); // Per-stream rate: some columns churn, some hold
            if ((s->rng & 255) < s->mut_rate) { // mut_rate is 0..255, so this is a probability
                // 16 random bits scaled to 0..MTX_ROWS-1 without a divide
                int r = (int)((((s->rng >> 8) & 0xFFFF) * MTX_ROWS) >> 16);
                s_cell_glyph[r][c] = (uint8_t)((s->rng >> 24) & (MTX_GLYPH_COUNT - 1));
            }
        }
    }
}

// Column-major so both streams' scalars stay in registers down the whole column
static void mtx_levels(void)
{
    for (int c = 0; c < MTX_COLS; c++) {
        const mtx_stream_t *s0 = &s_stream[c][0];
        const mtx_stream_t *s1 = &s_stream[c][1];
        const uint8_t *f0 = s_falloff[s0->tail_cls];
        const uint8_t *f1 = s_falloff[s1->tail_cls];
        int32_t  h0 = s0->head_q8, h1 = s1->head_q8;
        uint32_t d0 = s0->dim,     d1 = s1->dim;

        for (int r = 0; r < MTX_ROWS; r++) {
            int32_t  rq = (int32_t)r << 8;
            uint32_t b  = 0;

            // Q8 row delta -> 1/16-row LUT step, biased so d = -0.375 rows lands at index 0.
            // The unsigned cast makes a negative index huge, so one compare does both bounds
            int32_t i0 = (h0 - rq + MTX_FALL_BIAS) >> 4;
            if ((uint32_t)i0 < (uint32_t)MTX_FALL_N) {
                uint32_t v = ((uint32_t)f0[i0] * d0) >> 8;
                if (v > b) b = v;
            }
            int32_t i1 = (h1 - rq + MTX_FALL_BIAS) >> 4;
            if ((uint32_t)i1 < (uint32_t)MTX_FALL_N) {
                uint32_t v = ((uint32_t)f1[i1] * d1) >> 8;
                if (v > b) b = v; // The brighter stream wins
            }

            s_cell_level[r][c] = (uint8_t)(b >> 2); // b is 0..254; >> 2 gives a 0..63 palette index
        }
    }
}

/* ===========================================================================
 * Render
 * ========================================================================= */

// Straight into the framebuffer, no LVGL draw calls. Cell-ROW-major matters: a
// 32-byte line spans 2.67 cells, so column-major would cost ~1.6x the PSRAM traffic
static void mtx_paint(void)
{
    const uint32_t step = (uint32_t)s_stride_px / 2; // uint32 words per pixel row

    for (int r = 0; r < MTX_ROWS; r++) {
        for (int c = 0; c < MTX_COLS; c++) {
            uint8_t  l   = s_cell_level[r][c];
            uint8_t  g   = s_cell_glyph[r][c];
            uint16_t key = ((uint16_t)g << 8) | l; // Glyph included so a mutation is never missed
            if (key == s_cell_prev[r][c]) continue; // Nothing changed here
            s_cell_prev[r][c] = key;

            const uint16_t *art = (l >= MTX_GLOW_LEVEL) ? s_glyph_glow[g] : s_glyph_rom[g];
            const uint16_t *pp  = s_pal[l];

            // Offset is r*9*480 + c*12, both multiples of 4, so these stores are aligned
            uint32_t *dst = (uint32_t *)(s_pixels
                          + (size_t)(r * MTX_CELL_H) * (size_t)(s_stride_px * 2)
                          + (size_t)c * (MTX_CELL_W * 2));

            // Two pixels per 32-bit store. The C5 is little-endian, so the low half lands at
            // the lower address = the left pixel, matching bits 0..1 being cell column 0
            for (int y = 0; y < MTX_CELL_H; y++) {
                uint16_t bits = art[y];
                dst[0] = (uint32_t)pp[ bits        & 3] | ((uint32_t)pp[(bits >>  2) & 3] << 16);
                dst[1] = (uint32_t)pp[(bits >>  4) & 3] | ((uint32_t)pp[(bits >>  6) & 3] << 16);
                dst[2] = (uint32_t)pp[(bits >>  8) & 3] | ((uint32_t)pp[(bits >> 10) & 3] << 16);
                dst += step;
            }
        }
    }
}

// Runs on lcd_task via lv_timer_handler(), so everything here must stay non-blocking
static void mtx_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_canvas || !s_pixels) return; // Init failed or not run yet: nothing to drive

    // Time-based, so a late frame advances further instead of slowing the rain. The
    // clamp stops a long stall teleporting every column off-screen
    uint32_t now = lv_tick_get();
    uint32_t dt  = now - s_last_tick;
    if (dt < 8) dt = 8;
    else if (dt > 150) dt = 150;
    s_last_tick = now;

    mtx_advance(dt);
    mtx_levels();

    mtx_paint();                 // Writes the framebuffer directly (no LVGL invalidation)
    lv_obj_invalidate(s_canvas); // Schedule the single canvas redraw for this frame

    // Partial invalidation is deliberately NOT used: a narrow rect needs ~45 SPI window
    // setups against a full flush's 2, and LVGL widens it past 32 areas anyway
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

bool lcd_anim_matrix_init(lv_obj_t *parent)
{
    if (s_canvas) return true; // Already initialized; repeat calls are no-ops

    s_buf_size = (size_t)MTX_CANVAS_W * MTX_CANVAS_H * 2; // RGB565 = 2 bytes/pixel
    // PSRAM is fine: st7789 CPU-copies into its own staging buffer before SPI
    s_pixels = heap_caps_malloc(s_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pixels) {
        ESP_LOGE(TAG, "Failed to alloc PSRAM for matrix canvas");
        return false; // Caller falls back to another animation
    }

    // Checked: a failure memzeroes the descriptor, leaving s_stride_px 0 and every row
    // offset in mtx_paint collapsed. It cannot fail today, but the blit trusts it
    if (lv_draw_buf_init(&s_draw_buf, MTX_CANVAS_W, MTX_CANVAS_H, LV_COLOR_FORMAT_RGB565,
            LV_STRIDE_AUTO, s_pixels, s_buf_size) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to init matrix draw buffer");
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    s_stride_px = s_draw_buf.header.stride / 2; // LVGL reports stride in bytes; we index in pixels

    // The blit stores 32 bits at a time, so bail rather than go unaligned
    if ((((uintptr_t)s_pixels | (uintptr_t)s_draw_buf.header.stride) & 3u) != 0) {
        ESP_LOGE(TAG, "Matrix canvas buffer/stride not 4-byte aligned");
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }

    s_canvas = lv_canvas_create(parent);
    if (!s_canvas) {
        ESP_LOGE(TAG, "Failed to create matrix canvas");
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    lv_canvas_set_draw_buf(s_canvas, &s_draw_buf); // Canvas renders straight out of s_pixels
    lv_obj_set_size(s_canvas, MTX_CANVAS_W, MTX_CANVAS_H);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE); // It's a backdrop, not a widget

    mtx_build_palette(); // One-off float work, kept out of the frame path
    mtx_build_falloff();
    mtx_build_glyphs();
    mtx_seed();

    memset(s_pixels, 0, s_buf_size);                // Black first frame, in case we're shown early
    memset(s_cell_prev, 0xFF, sizeof(s_cell_prev)); // Matches no real key, so frame 1 paints all
    s_last_tick = lv_tick_get();

    // Created running: lcd_anim.c pauses it if MATRIX_RAIN isn't the active anim
    s_timer = lv_timer_create(mtx_timer_cb, MTX_FRAME_PERIOD, NULL);
    if (!s_timer) {
        ESP_LOGE(TAG, "Failed to create matrix timer");
        lv_obj_delete(s_canvas); // Unwind so a later retry starts clean
        s_canvas = NULL;         // NULL again, so the guard above won't claim success
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    return true;
}

void lcd_anim_matrix_start(void)
{
    if (!s_canvas) return; // Init failed: stay silent rather than crash
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    s_last_tick = lv_tick_get(); // Re-stamp, or the hidden interval arrives as one dt
    if (s_timer) lv_timer_resume(s_timer);
}

// Stop simulating but leave the canvas visible (frozen on its last frame)
void lcd_anim_matrix_pause(void)
{
    if (s_timer) lv_timer_pause(s_timer);
}

// Pausing matters as much as hiding: a hidden canvas still burns a full frame
void lcd_anim_matrix_stop(void)
{
    if (s_timer) lv_timer_pause(s_timer);
    if (s_canvas) lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
}

#endif // POLYCAST5_EN_MATRIX_RAIN_ANIM
