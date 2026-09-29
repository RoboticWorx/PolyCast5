#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#include "polycast5_macros.h"

#include "mirror.h"
#include "mirror_priv.h"

volatile mirror_state_t mirror_state = MIRROR_OFF;

// The live screen, written only by the flush hook. 64,800 B
POLYCAST5_USE_PSRAM_BSS static uint16_t s_shadow[MIRROR_SCR_W * MIRROR_SCR_H];

// What the viewer holds, quantized at the level it was sent with. 64,800 B
POLYCAST5_USE_PSRAM_BSS static uint16_t s_ref[MIRROR_SCR_W * MIRROR_SCR_H];

// One bit per tile, set by the flush hook, cleared by the encoder
static uint32_t s_dirty[(MIRROR_TILE_CNT + 31) / 32];

// One bit per tile: does s_ref hold what the viewer actually has? Zero-initialized, so
// nothing is assumed delivered until it has been encoded AND sent
static uint32_t s_ref_valid[(MIRROR_TILE_CNT + 31) / 32];

// Bumped by every capture touching the tile, so the encoder can spot a torn read.
// Volatile because the encoder reads it either side of the copy expecting lcd_task to
// have changed it underneath
static volatile uint8_t s_gen[MIRROR_TILE_CNT];

static portMUX_TYPE s_dirty_mux = portMUX_INITIALIZER_UNLOCKED;

const uint16_t *mirror_shadow(void)
{
    return s_shadow;
}

uint16_t *mirror_ref(void)
{
    return s_ref;
}

void mirror_capture(int16_t x1, int16_t y1, int16_t x2, int16_t y2, const uint16_t *px)
{
    // Clip defensively: a bad area would corrupt the heap either side of the shadow
    if (px == NULL || x1 < 0 || y1 < 0 || x2 >= MIRROR_SCR_W || y2 >= MIRROR_SCR_H ||
            x2 < x1 || y2 < y1) {
        return;
    }

    const int16_t w = x2 - x1 + 1;
    const int16_t h = y2 - y1 + 1;

    if (w == MIRROR_SCR_W) {
        // LVGL's partial render is full-width far more often than not, and then the
        // whole band is one contiguous run in both source and destination
        memcpy(&s_shadow[(uint32_t)y1 * MIRROR_SCR_W], px,
                (size_t)w * h * sizeof(uint16_t));
    } else {
        const uint16_t *src = px;
        uint16_t *dst = &s_shadow[(uint32_t)y1 * MIRROR_SCR_W + x1];

        for (int16_t y = 0; y < h; y++) {
            memcpy(dst, src, (size_t)w * sizeof(uint16_t));

            // Rows are exactly w pixels apart, never padded: LVGL's stride is
            // LV_ROUND_UP(w * bpp / 8, LV_DRAW_BUF_STRIDE_ALIGN) and lv_conf.h sets that
            // align to 1. The existing SPI path in st7789_flush_cb assumes the same
            src += w;
            dst += MIRROR_SCR_W;
        }
    }

    // Mark every tile the area overlaps
    const int c0 = x1 / MIRROR_TILE_W, c1 = x2 / MIRROR_TILE_W;
    const int r0 = y1 / MIRROR_TILE_H, r1 = y2 / MIRROR_TILE_H;

    portENTER_CRITICAL(&s_dirty_mux);
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            const int t = r * MIRROR_GRID_COLS + c;

            s_dirty[t >> 5] |= (1u << (t & 31));
            s_gen[t]++;
        }
    }
    portEXIT_CRITICAL(&s_dirty_mux);
}

void mirror_force_keyframe(void)
{
    // Dropping the reference as well as dirtying is what makes this a real full repaint.
    // Marking tiles dirty alone is not enough: they would compare equal to the reference
    // and be skipped, so a newly attached viewer would sit looking at a blank canvas
    portENTER_CRITICAL(&s_dirty_mux);
    for (size_t i = 0; i < sizeof(s_dirty) / sizeof(s_dirty[0]); i++) {
        s_dirty[i] = 0xFFFFFFFFu;
        s_ref_valid[i] = 0;
    }

    // Mask the bits past the last tile. Nothing ever clears them, so leaving them set
    // would make mirror_dirty_any() true forever and the encoder would run every tick
    s_dirty[MIRROR_TILE_CNT >> 5] &= (1u << (MIRROR_TILE_CNT & 31)) - 1u;
    portEXIT_CRITICAL(&s_dirty_mux);
}

bool mirror_ref_is_valid(uint16_t tile)
{
    if (tile >= MIRROR_TILE_CNT) {
        return false;
    }

    bool valid;

    portENTER_CRITICAL(&s_dirty_mux);
    valid = (s_ref_valid[tile >> 5] & (1u << (tile & 31))) != 0;
    portEXIT_CRITICAL(&s_dirty_mux);

    return valid;
}

void mirror_ref_mark_valid(uint16_t tile)
{
    if (tile >= MIRROR_TILE_CNT) {
        return;
    }

    portENTER_CRITICAL(&s_dirty_mux);
    s_ref_valid[tile >> 5] |= (1u << (tile & 31));
    portEXIT_CRITICAL(&s_dirty_mux);
}

void mirror_ref_invalidate(uint16_t tile)
{
    if (tile >= MIRROR_TILE_CNT) {
        return;
    }

    portENTER_CRITICAL(&s_dirty_mux);
    s_ref_valid[tile >> 5] &= ~(1u << (tile & 31));
    portEXIT_CRITICAL(&s_dirty_mux);
}

bool mirror_dirty_claim(uint16_t tile)
{
    if (tile >= MIRROR_TILE_CNT) {
        return false;
    }

    bool was_dirty;

    portENTER_CRITICAL(&s_dirty_mux);
    was_dirty = (s_dirty[tile >> 5] & (1u << (tile & 31))) != 0;
    s_dirty[tile >> 5] &= ~(1u << (tile & 31));
    portEXIT_CRITICAL(&s_dirty_mux);

    return was_dirty;
}

void mirror_dirty_restore(uint16_t tile)
{
    if (tile >= MIRROR_TILE_CNT) {
        return;
    }

    portENTER_CRITICAL(&s_dirty_mux);
    s_dirty[tile >> 5] |= (1u << (tile & 31));
    portEXIT_CRITICAL(&s_dirty_mux);
}

bool mirror_dirty_any(void)
{
    for (size_t i = 0; i < sizeof(s_dirty) / sizeof(s_dirty[0]); i++) {
        if (s_dirty[i] != 0) {
            return true;
        }
    }

    return false;
}

uint8_t mirror_tile_gen(uint16_t tile)
{
    return (tile < MIRROR_TILE_CNT) ? s_gen[tile] : 0;
}
