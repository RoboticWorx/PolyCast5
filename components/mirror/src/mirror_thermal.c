#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_attr.h" // EXT_RAM_BSS_ATTR, behind POLYCAST5_USE_PSRAM_BSS

#include "polycast5_macros.h"

#ifdef POLYCAST5_EN_SCREEN_MIRROR

#include "mirror.h"
#include "mirror_priv.h"
#include "mirror_proto.h"

_Static_assert(MIRROR_THERMAL_HDR_BYTES + 256 * 2 + MIRROR_THERMAL_MAX_PX * 2 <= MIRROR_MSG_MAX_BYTES,
        "A thermal message must fit the frame message buffer");
_Static_assert(MIRROR_SCR_W <= 255 && MIRROR_SCR_H <= 255, "Canvas geometry travels as bytes");

// Everything the viewer needs to redraw the canvas
typedef struct {
    uint32_t gen;       // Bumped by every change, so an unchanged slot is never re-copied
    uint32_t frame_gen; // Bumped by every published frame
    uint32_t pal_gen;   // Bumped when the palette table changes
    bool view_on;
    bool have_frame;
    bool flip_h;
    bool flip_v;
    bool crosshair;
    uint8_t cols;
    uint8_t rows;
    uint8_t n_holes;
    int32_t lo;
    int32_t hi;
    uint16_t cross_fg;
    uint16_t cross_bg;
    mirror_rect_t canvas;
    mirror_rect_t holes[MIRROR_THERMAL_MAX_HOLES];
    uint16_t palette[256];
    int16_t px[MIRROR_THERMAL_MAX_PX];
} thermal_state_t;

// Written only by lcd_task, read by mirror_task. s_pub_seq is odd while a write is under
// way. lcd_task outranks the reader, so a torn read means it published mid-copy
POLYCAST5_USE_PSRAM_BSS static thermal_state_t s_pub;
static volatile uint32_t s_pub_seq = 0;

// mirror_task's copy, and what the viewer was last told
POLYCAST5_USE_PSRAM_BSS static thermal_state_t s_snap;
static bool s_snap_valid = false;
static bool s_told_on = false;
static uint32_t s_told_tiles[MIRROR_TILE_WORDS];
static uint32_t s_told_frame = 0;
static uint32_t s_told_pal = 0;
static bool s_owe_full = true;

// The message last built, adopted by mirror_thermal_commit once it is on the wire
static bool s_next_ready = false;
static bool s_next_on = false;
static bool s_next_pal = false;
static uint32_t s_next_tiles[MIRROR_TILE_WORDS];
static uint32_t s_next_frame = 0;
static uint32_t s_next_pal_gen = 0;
static mirror_rect_t s_next_canvas;

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

static bool overlaps(const mirror_rect_t *a, const mirror_rect_t *b)
{
    return (a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2);
}

/* =============== Writer: lcd_task =============== */

static void pub_begin(void)
{
    s_pub_seq++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void pub_end(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    s_pub_seq++;
}

void mirror_thermal_frame(const mirror_thermal_frame_t *f)
{
    if (f == NULL || f->px == NULL || f->palette == NULL || f->cols == 0 || f->rows == 0 ||
            (uint32_t)f->cols * f->rows > MIRROR_THERMAL_MAX_PX) {
        return;
    }

    // Compared outside the write: this task is the only writer
    const bool pal_changed = !s_pub.have_frame ||
            memcmp(s_pub.palette, f->palette, sizeof(s_pub.palette)) != 0;

    pub_begin();

    memcpy(s_pub.px, f->px, (size_t)f->cols * f->rows * sizeof(int16_t));
    if (pal_changed) {
        memcpy(s_pub.palette, f->palette, sizeof(s_pub.palette));
        s_pub.pal_gen++;
    }

    s_pub.cols = f->cols;
    s_pub.rows = f->rows;
    s_pub.lo = f->lo;
    s_pub.hi = f->hi;
    s_pub.flip_h = f->flip_h;
    s_pub.flip_v = f->flip_v;
    s_pub.crosshair = f->crosshair;
    s_pub.cross_fg = f->cross_fg;
    s_pub.cross_bg = f->cross_bg;
    s_pub.have_frame = true;
    s_pub.frame_gen++;
    s_pub.gen++;

    pub_end();
}

void mirror_thermal_view(bool on, const mirror_rect_t *canvas, const mirror_rect_t *holes,
        size_t n_holes)
{
    if (canvas == NULL || n_holes > MIRROR_THERMAL_MAX_HOLES || (n_holes > 0 && holes == NULL)) {
        on = false;
        n_holes = 0;
    }

    // Unchanged, so mirror_task has nothing to re-copy
    if (on == s_pub.view_on && n_holes == s_pub.n_holes &&
            (canvas == NULL || memcmp(canvas, &s_pub.canvas, sizeof(*canvas)) == 0) &&
            (n_holes == 0 || memcmp(holes, s_pub.holes, n_holes * sizeof(*holes)) == 0)) {
        return;
    }

    pub_begin();

    s_pub.view_on = on;
    if (canvas != NULL) {
        s_pub.canvas = *canvas;
    }
    s_pub.n_holes = (uint8_t)n_holes;
    if (n_holes > 0) {
        memcpy(s_pub.holes, holes, n_holes * sizeof(*holes));
    }
    s_pub.gen++;

    pub_end();
}

void mirror_thermal_stop(void)
{
    if (!s_pub.view_on && !s_pub.have_frame) {
        return;
    }

    pub_begin();

    s_pub.view_on = false;
    s_pub.have_frame = false;
    s_pub.n_holes = 0;
    s_pub.gen++;

    pub_end();
}

/* =============== Reader: mirror_task =============== */

// Refresh s_snap if the slot moved. false when every copy was torn: s_snap is then unusable
// until a later copy lands whole
static bool snap_refresh(void)
{
    if (s_snap_valid && s_pub.gen == s_snap.gen) {
        return true;
    }

    for (int tries = 0; tries < 4; tries++) {
        const uint32_t seq = s_pub_seq;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if ((seq & 1u) != 0) {
            continue;
        }

        memcpy(&s_snap, &s_pub, sizeof(s_snap));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        if (s_pub_seq == seq) {
            s_snap_valid = true;
            return true;
        }
    }

    s_snap_valid = false;
    return false;
}

// Tiles the viewer paints from the frame: every tile the canvas touches, minus any an
// overlay touches
static void thermal_tiles(const thermal_state_t *t, uint32_t *tiles)
{
    memset(tiles, 0, MIRROR_TILE_WORDS * sizeof(uint32_t));

    for (uint16_t tile = 0; tile < MIRROR_TILE_CNT; tile++) {
        const int16_t x1 = (int16_t)((tile % MIRROR_GRID_COLS) * MIRROR_TILE_W);
        const int16_t y1 = (int16_t)((tile / MIRROR_GRID_COLS) * MIRROR_TILE_H);
        const mirror_rect_t r = { x1, y1, (int16_t)(x1 + MIRROR_TILE_W - 1),
                (int16_t)(y1 + MIRROR_TILE_H - 1) };

        if (!overlaps(&r, &t->canvas)) {
            continue;
        }

        bool covered = false;
        for (uint8_t h = 0; h < t->n_holes && !covered; h++) {
            covered = overlaps(&r, &t->holes[h]);
        }

        if (!covered) {
            tiles[tile >> 5] |= (1u << (tile & 31));
        }
    }
}

size_t mirror_thermal_encode(bool keyframe, uint16_t seq, uint8_t *out, size_t cap)
{
    if (keyframe) {
        s_owe_full = true; // Held until a message lands
    }

    if (out == NULL || !snap_refresh()) {
        return 0;
    }

    const thermal_state_t *t = &s_snap;

    // The canvas has to be on screen, inside it, and not behind a redaction
    const bool on = t->view_on && t->have_frame && !mirror_is_redacted() &&
            t->cols > 0 && t->rows > 0 && t->canvas.x1 >= 0 && t->canvas.y1 >= 0 &&
            t->canvas.x2 < MIRROR_SCR_W && t->canvas.y2 < MIRROR_SCR_H &&
            t->canvas.x1 <= t->canvas.x2 && t->canvas.y1 <= t->canvas.y2;

    uint32_t tiles[MIRROR_TILE_WORDS] = { 0 };
    if (on) {
        thermal_tiles(t, tiles);
    }

    const bool owed = (on != s_told_on) ||
            (on && (s_owe_full || t->frame_gen != s_told_frame ||
                    memcmp(tiles, s_told_tiles, sizeof(tiles)) != 0));

    if (!owed) {
        if (!on) {
            s_owe_full = false; // A viewer that was never told ON has nothing to resync
        }
        return 0;
    }

    // Every OFF to ON carries the palette: a new viewer has none
    const bool pal = on && (s_owe_full || !s_told_on || t->pal_gen != s_told_pal);
    const size_t px_bytes = (size_t)t->cols * t->rows * sizeof(int16_t);
    const size_t need = on ? (MIRROR_THERMAL_HDR_BYTES + (pal ? sizeof(t->palette) : 0) + px_bytes)
            : MIRROR_THERMAL_OFF_BYTES;

    if (cap < need) {
        return 0;
    }

    uint8_t flags = 0;
    if (on) {
        flags |= MIRROR_THERMAL_ON;
        if (pal) flags |= MIRROR_THERMAL_PALETTE;
        if (t->flip_h) flags |= MIRROR_THERMAL_FLIP_H;
        if (t->flip_v) flags |= MIRROR_THERMAL_FLIP_V;
        if (t->crosshair) flags |= MIRROR_THERMAL_CROSSHAIR;
    }

    out[0] = MIRROR_MSG_THERMAL;
    out[1] = flags;
    put_u16(&out[2], seq);

    if (on) {
        out[4] = (uint8_t)t->canvas.x1;
        out[5] = (uint8_t)t->canvas.y1;
        out[6] = (uint8_t)(t->canvas.x2 - t->canvas.x1 + 1);
        out[7] = (uint8_t)(t->canvas.y2 - t->canvas.y1 + 1);
        out[8] = t->cols;
        out[9] = t->rows;
        put_u32(&out[10], (uint32_t)t->lo);
        put_u32(&out[14], (uint32_t)t->hi);
        put_u16(&out[18], t->cross_fg);
        put_u16(&out[20], t->cross_bg);

        for (int i = 0; i < MIRROR_THERMAL_TILE_BYTES; i++) {
            out[22 + i] = (uint8_t)(tiles[i >> 2] >> ((i & 3) * 8));
        }

        size_t n = MIRROR_THERMAL_HDR_BYTES;
        if (pal) {
            for (int i = 0; i < 256; i++) {
                put_u16(&out[n], t->palette[i]);
                n += 2;
            }
        }
        for (uint32_t i = 0; i < (uint32_t)t->cols * t->rows; i++) {
            put_u16(&out[n], (uint16_t)t->px[i]);
            n += 2;
        }
    }

    s_next_ready = true;
    s_next_on = on;
    s_next_pal = pal;
    memcpy(s_next_tiles, tiles, sizeof(s_next_tiles));
    s_next_frame = t->frame_gen;
    s_next_pal_gen = t->pal_gen;
    s_next_canvas = t->canvas;

    return need;
}

void mirror_thermal_commit(void)
{
    if (!s_next_ready) {
        return;
    }
    s_next_ready = false;

    // A tile changing hands is painted differently from now on, so resend it from the shadow
    for (uint16_t tile = 0; tile < MIRROR_TILE_CNT; tile++) {
        const uint32_t bit = 1u << (tile & 31);

        if (((s_next_tiles[tile >> 5] ^ s_told_tiles[tile >> 5]) & bit) != 0) {
            mirror_ref_invalidate(tile);
            mirror_dirty_restore(tile);
        }
    }

    memcpy(s_told_tiles, s_next_tiles, sizeof(s_told_tiles));
    s_told_on = s_next_on;
    s_told_frame = s_next_frame;
    if (s_next_pal) {
        s_told_pal = s_next_pal_gen;
    }
    s_owe_full = false;

    mirror_encode_set_thermal(s_told_on ? s_told_tiles : NULL, &s_next_canvas);
}

void mirror_thermal_reset(void)
{
    s_snap_valid = false;
    s_told_on = false;
    memset(s_told_tiles, 0, sizeof(s_told_tiles));
    s_told_frame = 0;
    s_told_pal = 0;
    s_owe_full = true;
    s_next_ready = false;

    mirror_encode_set_thermal(NULL, NULL);
}

#endif // POLYCAST5_EN_SCREEN_MIRROR
