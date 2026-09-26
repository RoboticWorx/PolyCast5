#include <string.h>
#include <stdint.h>

#include "esp_attr.h" // EXT_RAM_BSS_ATTR, behind POLYCAST5_USE_PSRAM_BSS

#include "polycast5_macros.h"

#ifdef POLYCAST5_EN_SCREEN_MIRROR

#include "mirror.h"
#include "mirror_priv.h"
#include "mirror_proto.h"

#define MIRROR_MAX_PAL 16 // PAL4 indices are nibbles

// Worst case is every tile RAW: 135 * (5 + 480) = 65,475 B
#define MIRROR_ARENA_BYTES (MIRROR_TILE_CNT * (MIRROR_TILE_HDR_BYTES + MIRROR_TILE_PX * 2))

POLYCAST5_USE_PSRAM_BSS static uint8_t s_arena[MIRROR_ARENA_BYTES];

static uint32_t s_rec_off[MIRROR_TILE_CNT + 1]; // Byte offset of each record, plus the end
static uint16_t s_rec_tile[MIRROR_TILE_CNT]; // Tile index per record, to restore a failed send
static uint16_t s_rec_count = 0;

// One redaction snapshot per frame: mirror_encode_frame reads it, mirror_encode_build_msg
// stamps the header from it, so every message of a frame agrees on the flag
static bool s_redacted_snap = false;

// Tiles whose canvas part the viewer paints from the thermal channel
static bool s_th_on = false;
static uint32_t s_th_tiles[MIRROR_TILE_WORDS];
static mirror_rect_t s_th_canvas;

// RGB565 is RRRRRGGGGGGBBBBB. These drop low bits without disturbing the field layout, so
// the browser needs no dequantization step
static uint16_t quantize_mask(mirror_quality_t q)
{
    switch (q) {
        case MIRROR_Q_G5:
            return 0xFFDF; // Green's LSB only
        case MIRROR_Q_444:
        case MIRROR_Q_444_HALF:
            return 0xF79E; // RGB444 packed in 565
        case MIRROR_Q_EXACT:
        default:
            return 0xFFFF;
    }
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

// Pixel i in the chosen scan order
static inline uint16_t scan_px(const uint16_t *px, int w, int h, bool vertical, int i)
{
    return vertical ? px[(i % h) * w + (i / h)] : px[i];
}

// Runs of one colour. A run caps at 255 so the count fits a byte
static uint32_t count_runs(const uint16_t *px, int w, int h, bool vertical)
{
    const int total = w * h;

    uint32_t runs = 1;
    uint32_t len = 1;
    uint16_t prev = px[0];

    for (int i = 1; i < total; i++) {
        const uint16_t c = scan_px(px, w, h, vertical, i);

        if (c == prev && len < 255) {
            len++;
        } else {
            runs++;
            len = 1;
            prev = c;
        }
    }

    return runs;
}

static size_t emit_rle(const uint16_t *px, int w, int h, bool vertical, uint8_t *out, size_t cap)
{
    const int total = w * h;

    size_t n = 0;
    uint16_t prev = px[0];
    uint32_t len = 1;

    for (int i = 1; i <= total; i++) {
        if (i < total) {
            const uint16_t c = scan_px(px, w, h, vertical, i);

            if (c == prev && len < 255) {
                len++;
                continue;
            }
        }

        if (n + 3 > cap) {
            return 0;
        }

        out[n++] = (uint8_t)len;
        put_u16(&out[n], prev);
        n += 2;

        if (i < total) {
            prev = scan_px(px, w, h, vertical, i);
            len = 1;
        }
    }

    return n;
}

// Distinct colours, abandoned as soon as PAL4 is off the table
static int build_palette(const uint16_t *px, int total, uint16_t *pal)
{
    int n = 0;

    for (int i = 0; i < total; i++) {
        int j = 0;

        while (j < n && pal[j] != px[i]) {
            j++;
        }

        if (j == n) {
            if (n == MIRROR_MAX_PAL) {
                return -1; // Too many colours
            }

            pal[n++] = px[i];
        }
    }

    return n;
}

// Write the tile whichever way is smallest
static size_t encode_pixels(const uint16_t *px, int w, int h, uint8_t *out, size_t cap,
        uint8_t *out_enc)
{
    const int total = w * h;
    const size_t raw_bytes = (size_t)total * 2;

    uint16_t pal[MIRROR_MAX_PAL];
    const int pal_n = build_palette(px, total, pal);

    if (pal_n == 1) {
        if (cap < 2) {
            return 0;
        }

        put_u16(out, px[0]);
        *out_enc = MIRROR_ENC_SOLID;
        return 2;
    }

    const size_t rle_h_bytes = count_runs(px, w, h, false) * 3;
    const size_t rle_v_bytes = count_runs(px, w, h, true) * 3;
    const size_t pal_bytes = (pal_n > 0)
            ? (size_t)(1 + pal_n * 2 + (total + 1) / 2)
            : raw_bytes + 1; // Never chosen

    size_t best = raw_bytes;
    uint8_t best_enc = MIRROR_ENC_RAW;

    if (rle_h_bytes < best) {
        best = rle_h_bytes;
        best_enc = MIRROR_ENC_RLE_H;
    }
    if (rle_v_bytes < best) {
        best = rle_v_bytes;
        best_enc = MIRROR_ENC_RLE_V;
    }
    if (pal_bytes < best) {
        best = pal_bytes;
        best_enc = MIRROR_ENC_PAL4;
    }

    if (best > cap) {
        return 0;
    }

    if (best_enc == MIRROR_ENC_RLE_H || best_enc == MIRROR_ENC_RLE_V) {
        const size_t n = emit_rle(px, w, h, best_enc == MIRROR_ENC_RLE_V, out, cap);

        if (n != 0) {
            *out_enc = best_enc;
            return n;
        }

        best_enc = MIRROR_ENC_RAW; // Fall back
    }

    if (best_enc == MIRROR_ENC_PAL4) {
        size_t n = 0;

        out[n++] = (uint8_t)pal_n;
        for (int i = 0; i < pal_n; i++) {
            put_u16(&out[n], pal[i]);
            n += 2;
        }

        memset(&out[n], 0, (size_t)(total + 1) / 2);
        for (int i = 0; i < total; i++) {
            int idx = 0;

            while (pal[idx] != px[i]) {
                idx++;
            }

            // The high nibble holds the even pixel
            out[n + (size_t)i / 2] |= (uint8_t)(idx << ((i & 1) ? 0 : 4));
        }
        n += (size_t)(total + 1) / 2;

        *out_enc = MIRROR_ENC_PAL4;
        return n;
    }

    if (raw_bytes > cap) {
        return 0;
    }

    for (int i = 0; i < total; i++) {
        put_u16(&out[(size_t)i * 2], px[i]);
    }

    *out_enc = MIRROR_ENC_RAW;
    return raw_bytes;
}

uint16_t mirror_encode_frame(mirror_quality_t q)
{
    const uint16_t mask = quantize_mask(q);
    const bool half = (q == MIRROR_Q_444_HALF);

    // Redaction is applied at capture time, so the shadow already holds the placeholder.
    // Snapshot the flag once here and stamp every message of this frame from it
    s_redacted_snap = mirror_is_redacted();

    const uint16_t *shadow = mirror_shadow();
    uint16_t *ref = mirror_ref();

    uint16_t tile_px[MIRROR_TILE_PX];
    uint16_t small_px[(MIRROR_TILE_W / 2) * ((MIRROR_TILE_H + 1) / 2)];

    s_rec_count = 0;
    s_rec_off[0] = 0;

    for (uint16_t t = 0; t < MIRROR_TILE_CNT; t++) {
        if (!mirror_dirty_claim(t)) {
            continue;
        }

        const uint8_t gen = mirror_tile_gen(t);

        const int col = t % MIRROR_GRID_COLS;
        const int row = t / MIRROR_GRID_COLS;
        const uint32_t origin = (uint32_t)row * MIRROR_TILE_H * MIRROR_SCR_W + col * MIRROR_TILE_W;

        // Gather and quantize, then diff against what the viewer already holds. A tile
        // whose reference is not valid is always re-encoded: it has either never been
        // delivered, or a keyframe/quality change/failed send retired it
        bool changed = !mirror_ref_is_valid(t);

        // The viewer paints the thermal frame over this tile's canvas part, so what LVGL
        // flushed there is never seen: holding it constant keeps sensor noise out of the diff
        int th_x0 = 0, th_x1 = -1, th_y0 = 0, th_y1 = -1;

        if (s_th_on && (s_th_tiles[t >> 5] & (1u << (t & 31))) != 0) {
            th_x0 = s_th_canvas.x1 - col * MIRROR_TILE_W;
            th_x1 = s_th_canvas.x2 - col * MIRROR_TILE_W;
            th_y0 = s_th_canvas.y1 - row * MIRROR_TILE_H;
            th_y1 = s_th_canvas.y2 - row * MIRROR_TILE_H;

            if (th_x0 < 0) th_x0 = 0;
            if (th_x1 > MIRROR_TILE_W - 1) th_x1 = MIRROR_TILE_W - 1;
            if (th_y0 < 0) th_y0 = 0;
            if (th_y1 > MIRROR_TILE_H - 1) th_y1 = MIRROR_TILE_H - 1;
        }

        for (int y = 0; y < MIRROR_TILE_H; y++) {
            const uint16_t *src = &shadow[origin + (uint32_t)y * MIRROR_SCR_W];
            uint16_t *dst = &tile_px[y * MIRROR_TILE_W];

            for (int x = 0; x < MIRROR_TILE_W; x++) {
                dst[x] = (uint16_t)(src[x] & mask);
            }

            if (y >= th_y0 && y <= th_y1) {
                for (int x = th_x0; x <= th_x1; x++) {
                    dst[x] = (uint16_t)(MIRROR_THERMAL_FILL & mask);
                }
            }

            if (!changed && memcmp(&ref[origin + (uint32_t)y * MIRROR_SCR_W], dst,
                    MIRROR_TILE_W * sizeof(uint16_t)) != 0) {
                changed = true;
            }
        }

        if (!changed) {
            continue; // Repainted with identical pixels, so nothing is owed
        }

        // At half scale the payload is subsampled, but the reference stays full size so a
        // later level change still diffs correctly
        const uint16_t *src_px = tile_px;
        int enc_w = MIRROR_TILE_W;
        int enc_h = MIRROR_TILE_H;

        if (half) {
            enc_w = MIRROR_TILE_W / 2;
            enc_h = (MIRROR_TILE_H + 1) / 2;

            for (int y = 0; y < enc_h; y++) {
                for (int x = 0; x < enc_w; x++) {
                    small_px[y * enc_w + x] = tile_px[(y * 2) * MIRROR_TILE_W + (x * 2)];
                }
            }

            src_px = small_px;
        }

        uint8_t *rec = &s_arena[s_rec_off[s_rec_count]];
        const size_t room = MIRROR_ARENA_BYTES - s_rec_off[s_rec_count];

        if (room < MIRROR_TILE_HDR_BYTES + 2) {
            mirror_dirty_restore(t); // Arena full, so this tile rides the next frame
            break;
        }

        uint8_t enc = MIRROR_ENC_RAW;
        const size_t payload = encode_pixels(src_px, enc_w, enc_h,
                rec + MIRROR_TILE_HDR_BYTES, room - MIRROR_TILE_HDR_BYTES, &enc);

        if (payload == 0) {
            mirror_dirty_restore(t);
            break;
        }

        // lcd_task runs at a higher priority than this task, so a tile can be repainted
        // underneath the gather. Drop the torn bytes and leave it dirty
        if (mirror_tile_gen(t) != gen) {
            mirror_dirty_restore(t);
            continue;
        }

        for (int y = 0; y < MIRROR_TILE_H; y++) {
            memcpy(&ref[origin + (uint32_t)y * MIRROR_SCR_W], &tile_px[y * MIRROR_TILE_W],
                    MIRROR_TILE_W * sizeof(uint16_t));
        }

        mirror_ref_mark_valid(t);

        put_u16(rec, t);
        rec[2] = enc;
        put_u16(&rec[3], (uint16_t)payload);

        s_rec_tile[s_rec_count] = t;
        s_rec_off[s_rec_count + 1] = s_rec_off[s_rec_count] + MIRROR_TILE_HDR_BYTES + payload;
        s_rec_count++;
    }

    return s_rec_count;
}

uint16_t mirror_encode_msg_count(void)
{
    return (uint16_t)((s_rec_count + MIRROR_TILES_PER_MSG - 1) / MIRROR_TILES_PER_MSG);
}

size_t mirror_encode_build_msg(uint16_t index, uint16_t seq, bool keyframe,
        mirror_quality_t q, uint8_t *out, size_t cap)
{
    const uint16_t msgs = mirror_encode_msg_count();

    if (out == NULL || index >= msgs) {
        return 0;
    }

    const uint16_t first = (uint16_t)(index * MIRROR_TILES_PER_MSG);
    uint16_t count = (uint16_t)(s_rec_count - first);

    if (count > MIRROR_TILES_PER_MSG) {
        count = MIRROR_TILES_PER_MSG;
    }

    const size_t body = s_rec_off[first + count] - s_rec_off[first];

    if (cap < MIRROR_FRAME_HDR_BYTES + body) {
        return 0;
    }

    uint8_t flags = (uint8_t)((uint8_t)q << MIRROR_FLAG_LEVEL_SHIFT);

    if (keyframe) {
        flags |= MIRROR_FLAG_KEYFRAME;
    }
    if (q == MIRROR_Q_444_HALF) {
        flags |= MIRROR_FLAG_HALF_SCALE;
    }
    if (index == msgs - 1) {
        flags |= MIRROR_FLAG_LAST_MSG;
    }
    if (s_redacted_snap) {
        flags |= MIRROR_FLAG_REDACTED;
    }

    out[0] = MIRROR_MSG_FRAME;
    out[1] = flags;
    put_u16(&out[2], seq);
    out[4] = (uint8_t)((q == MIRROR_Q_444_HALF) ? (MIRROR_TILE_W / 2) : MIRROR_TILE_W);
    out[5] = (uint8_t)((q == MIRROR_Q_444_HALF) ? ((MIRROR_TILE_H + 1) / 2) : MIRROR_TILE_H);
    out[6] = MIRROR_GRID_COLS;
    out[7] = MIRROR_GRID_ROWS;
    put_u16(&out[8], count);

    memcpy(&out[MIRROR_FRAME_HDR_BYTES], &s_arena[s_rec_off[first]], body);

    return MIRROR_FRAME_HDR_BYTES + body;
}

void mirror_encode_set_thermal(const uint32_t *tiles, const mirror_rect_t *canvas)
{
    if (tiles == NULL || canvas == NULL) {
        s_th_on = false;
        memset(s_th_tiles, 0, sizeof(s_th_tiles));
        return;
    }

    memcpy(s_th_tiles, tiles, sizeof(s_th_tiles));
    s_th_canvas = *canvas;
    s_th_on = true;
}

void mirror_encode_restore_msg(uint16_t index)
{
    const uint16_t first = (uint16_t)(index * MIRROR_TILES_PER_MSG);

    if (first >= s_rec_count) {
        return;
    }

    uint16_t count = (uint16_t)(s_rec_count - first);

    if (count > MIRROR_TILES_PER_MSG) {
        count = MIRROR_TILES_PER_MSG;
    }

    for (uint16_t i = 0; i < count; i++) {
        const uint16_t tile = s_rec_tile[first + i];

        // The reference was advanced when this tile was encoded. If it never reached the
        // wire, retiring the reference is what makes the next frame re-send it instead of
        // comparing equal and skipping it forever
        mirror_ref_invalidate(tile);
        mirror_dirty_restore(tile);
    }
}

#endif // POLYCAST5_EN_SCREEN_MIRROR
