/* Host round-trip test for the Screen Mirror tile encoder.
 * Decodes with an independent implementation of what the browser will do, so a bug in
 * the encoder cannot cancel out against a matching bug in the decoder. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "mirror.h"
#include "mirror_priv.h"
#include "mirror_proto.h"

/* ---- the "browser": decode a FRAME message into a 240x135 RGB565 canvas ---- */

static uint16_t canvas[MIRROR_SCR_W * MIRROR_SCR_H];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int decode_msg(const uint8_t *m, size_t len)
{
    if (len < MIRROR_FRAME_HDR_BYTES || m[0] != MIRROR_MSG_FRAME) return -1;

    const uint8_t flags = m[1];
    const int tw = m[4], th = m[5], cols = m[6];
    const int half = (flags & MIRROR_FLAG_HALF_SCALE) ? 1 : 0;
    const int count = rd16(&m[8]);

    size_t off = MIRROR_FRAME_HDR_BYTES;

    for (int k = 0; k < count; k++) {
        if (off + MIRROR_TILE_HDR_BYTES > len) return -2;

        const int tile = rd16(&m[off]);
        const uint8_t enc = m[off + 2];
        const size_t plen = rd16(&m[off + 3]);
        const uint8_t *p = &m[off + MIRROR_TILE_HDR_BYTES];

        if (off + MIRROR_TILE_HDR_BYTES + plen > len) return -3;

        const int total = tw * th;
        uint16_t px[MIRROR_TILE_PX];

        switch (enc) {
            case MIRROR_ENC_SOLID: {
                if (plen != 2) return -4;
                const uint16_t c = rd16(p);
                for (int i = 0; i < total; i++) px[i] = c;
                break;
            }
            case MIRROR_ENC_RAW: {
                if (plen != (size_t)total * 2) return -5;
                for (int i = 0; i < total; i++) px[i] = rd16(&p[i * 2]);
                break;
            }
            case MIRROR_ENC_RLE_H:
            case MIRROR_ENC_RLE_V: {
                int i = 0;
                size_t q = 0;
                while (q + 3 <= plen) {
                    int n = p[q];
                    const uint16_t c = rd16(&p[q + 1]);
                    q += 3;
                    while (n-- > 0) {
                        if (i >= total) return -6;
                        if (enc == MIRROR_ENC_RLE_V) px[(i % th) * tw + (i / th)] = c;
                        else px[i] = c;
                        i++;
                    }
                }
                if (i != total) return -7;
                break;
            }
            case MIRROR_ENC_PAL4: {
                const int n = p[0];
                if (n < 1 || n > 16) return -8;
                if (plen != (size_t)(1 + n * 2 + (total + 1) / 2)) return -9;
                const uint8_t *idx = &p[1 + n * 2];
                for (int i = 0; i < total; i++) {
                    const int v = (i & 1) ? (idx[i / 2] & 0x0F) : (idx[i / 2] >> 4);
                    if (v >= n) return -10;
                    px[i] = rd16(&p[1 + v * 2]);
                }
                break;
            }
            default: return -11;
        }

        /* Paint. At half scale each sample covers a 2x2 block of the real screen. */
        const int col = tile % cols, row = tile / cols;
        const int ox = col * MIRROR_TILE_W, oy = row * MIRROR_TILE_H;

        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                const uint16_t c = px[y * tw + x];
                const int sx = half ? x * 2 : x, sy = half ? y * 2 : y;
                for (int dy = 0; dy < (half ? 2 : 1); dy++) {
                    for (int dx = 0; dx < (half ? 2 : 1); dx++) {
                        const int gx = ox + sx + dx, gy = oy + sy + dy;
                        if (gx < ox + MIRROR_TILE_W && gy < oy + MIRROR_TILE_H &&
                            gx < MIRROR_SCR_W && gy < MIRROR_SCR_H)
                            canvas[gy * MIRROR_SCR_W + gx] = c;
                    }
                }
            }
        }

        off += MIRROR_TILE_HDR_BYTES + plen;
    }

    return (flags & MIRROR_FLAG_LAST_MSG) ? 1 : 0;
}

/* ---- synthetic screens ---- */

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void scr_solid(uint16_t *s, uint16_t c)
{
    for (int i = 0; i < MIRROR_SCR_W * MIRROR_SCR_H; i++) s[i] = c;
}

/* Flat background with blocky glyph-ish runs: stands in for a menu */
static void scr_menu(uint16_t *s, int shift)
{
    scr_solid(s, 0x0000);
    for (int row = 0; row < 5; row++) {
        const int y0 = 10 + row * 24 + shift;
        for (int y = y0; y < y0 + 14 && y < MIRROR_SCR_H; y++) {
            if (y < 0) continue;
            for (int x = 12; x < 210; x++) {
                const int on = ((x / 5 + row * 3) % 4) < 2;
                s[y * MIRROR_SCR_W + x] = on ? 0xFFFF : 0x0000;
            }
        }
    }
}

/* Smooth two-axis gradient: stands in for the upscaled thermal image */
static void scr_thermal(uint16_t *s, int phase)
{
    for (int y = 0; y < MIRROR_SCR_H; y++)
        for (int x = 0; x < MIRROR_SCR_W; x++) {
            const int v = (x * 255 / MIRROR_SCR_W + y * 255 / MIRROR_SCR_H + phase) / 2;
            const int r = v, g = 255 - v, b = (v * 3) % 256;
            s[y * MIRROR_SCR_W + x] =
                (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
}

/* Vertically constant columns: stands in for DOOM's raycast walls */
static void scr_doom(uint16_t *s, int phase)
{
    for (int x = 0; x < MIRROR_SCR_W; x++) {
        const int h = 30 + ((x * 7 + phase * 3) % 60);
        const uint16_t wall = (uint16_t)(((x * 13 + phase) % 0xFFFF) & 0xF79E);
        for (int y = 0; y < MIRROR_SCR_H; y++) {
            const int mid = MIRROR_SCR_H / 2;
            s[y * MIRROR_SCR_W + x] =
                (y > mid - h && y < mid + h) ? wall : (y <= mid ? 0x2104 : 0x4208);
        }
    }
}

static void scr_noise(uint16_t *s)
{
    for (int i = 0; i < MIRROR_SCR_W * MIRROR_SCR_H; i++) s[i] = (uint16_t)rnd();
}

/* ---- driver ---- */

static uint16_t truth[MIRROR_SCR_W * MIRROR_SCR_H];
static uint8_t msgbuf[MIRROR_MSG_MAX_BYTES];

static int failures = 0;

/* Push a screen through the capture hook exactly as the flush callback would:
 * 20-line full-width bands, which is what LVGL actually delivers. */
static void push_screen(const uint16_t *s)
{
    for (int y = 0; y < MIRROR_SCR_H; y += 20) {
        const int y2 = (y + 19 < MIRROR_SCR_H) ? y + 19 : MIRROR_SCR_H - 1;
        mirror_capture(0, (int16_t)y, MIRROR_SCR_W - 1, (int16_t)y2,
                       &s[(size_t)y * MIRROR_SCR_W]);
    }
}

static size_t send_frame(mirror_quality_t q, bool keyframe)
{
    const uint16_t tiles = mirror_encode_frame(q);
    if (tiles == 0) return 0;

    const uint16_t msgs = mirror_encode_msg_count();
    size_t bytes = 0;
    int saw_last = 0;

    for (uint16_t i = 0; i < msgs; i++) {
        const size_t n = mirror_encode_build_msg(i, 1, keyframe, q, msgbuf, sizeof(msgbuf));
        if (n == 0) { printf("  !! build_msg %u returned 0\n", i); failures++; return bytes; }
        if (n > MIRROR_MSG_MAX_BYTES) { printf("  !! msg too big %zu\n", n); failures++; }
        bytes += n;
        const int r = decode_msg(msgbuf, n);
        if (r < 0) { printf("  !! decode error %d on msg %u\n", r, i); failures++; return bytes; }
        saw_last += r;
    }

    if (saw_last != 1) { printf("  !! last-msg flag count = %d\n", saw_last); failures++; }
    return bytes;
}

static uint16_t qmask(mirror_quality_t q)
{
    return q == MIRROR_Q_G5 ? 0xFFDF : (q >= MIRROR_Q_444 ? 0xF79E : 0xFFFF);
}

/* At full scale the canvas must equal the quantized truth exactly. */
static void verify_exact(const uint16_t *s, mirror_quality_t q, const char *what)
{
    const uint16_t m = qmask(q);
    int bad = 0, first = -1;

    for (int i = 0; i < MIRROR_SCR_W * MIRROR_SCR_H; i++)
        if (canvas[i] != (uint16_t)(s[i] & m)) { if (first < 0) first = i; bad++; }

    if (bad) {
        printf("  !! %s: %d/%d px differ (first at %d,%d: got %04X want %04X)\n", what, bad,
               MIRROR_SCR_W * MIRROR_SCR_H, first % MIRROR_SCR_W, first / MIRROR_SCR_W,
               canvas[first], (uint16_t)(s[first] & m));
        failures++;
    } else {
        printf("  ok  %s: pixel-exact\n", what);
    }
}

static void reset_all(void)
{
    memset(canvas, 0, sizeof(canvas));
    /* Drive the encoder's reference back to zero the way a fresh session does. */
    uint16_t zero[MIRROR_SCR_W * MIRROR_SCR_H];
    memset(zero, 0, sizeof(zero));
    push_screen(zero);
    mirror_force_keyframe();
    send_frame(MIRROR_Q_EXACT, true);
    memset(canvas, 0, sizeof(canvas));
}

int main(void)
{
    mirror_state = MIRROR_LIVE;

    printf("=== round-trip, all quality levels ===\n");
    const char *qn[] = { "EXACT", "G5", "444", "444_HALF" };

    for (int q = 0; q < MIRROR_Q_COUNT; q++) {
        reset_all();
        mirror_force_keyframe();

        scr_menu(truth, 0);
        push_screen(truth);
        const size_t b = send_frame((mirror_quality_t)q, true);

        printf(" %-9s keyframe %6zu B", qn[q], b);
        if (q == MIRROR_Q_444_HALF) {
            printf("  (half scale, exactness not asserted)\n");
        } else {
            printf("\n");
            verify_exact(truth, (mirror_quality_t)q, qn[q]);
        }
    }

    printf("\n=== content types, keyframe cost at EXACT (raw = 64800 B) ===\n");
    struct { const char *name; void (*gen)(uint16_t *, int); } kinds[] = {
        { "menu",    scr_menu    },
        { "thermal", scr_thermal },
        { "doom",    scr_doom    },
    };

    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        reset_all();
        mirror_force_keyframe();
        kinds[k].gen(truth, 0);
        push_screen(truth);
        const size_t b = send_frame(MIRROR_Q_EXACT, true);
        verify_exact(truth, MIRROR_Q_EXACT, kinds[k].name);
        printf("      %-8s %6zu B  (%.1fx vs raw)\n", kinds[k].name, b, 64800.0 / (double)b);
    }

    {
        reset_all();
        mirror_force_keyframe();
        scr_noise(truth);
        push_screen(truth);
        const size_t b = send_frame(MIRROR_Q_EXACT, true);
        verify_exact(truth, MIRROR_Q_EXACT, "noise");
        printf("      %-8s %6zu B  (%.1fx vs raw, must not exceed ~1.0x)\n", "noise", b,
               64800.0 / (double)b);
    }

    printf("\n=== incremental: what a steady scene actually costs per frame ===\n");

    /* Thermal with the IIR drift the real sensor has: every pixel moves a little. */
    for (int level = 0; level < MIRROR_Q_COUNT; level++) {
        reset_all();
        mirror_force_keyframe();
        scr_thermal(truth, 0);
        push_screen(truth);
        send_frame((mirror_quality_t)level, true);

        size_t total = 0;
        for (int f = 1; f <= 8; f++) {
            scr_thermal(truth, f); /* drift */
            push_screen(truth);
            total += send_frame((mirror_quality_t)level, false);
        }
        printf(" thermal drift %-9s %6zu B over 8 frames -> %6.1f KB/s at 8 fps\n",
               qn[level], total, total / 8.0 * 8.0 / 1024.0);
    }

    for (int level = 0; level < MIRROR_Q_COUNT; level++) {
        reset_all();
        mirror_force_keyframe();
        scr_doom(truth, 0);
        push_screen(truth);
        send_frame((mirror_quality_t)level, true);

        size_t total = 0;
        for (int f = 1; f <= 8; f++) {
            scr_doom(truth, f);
            push_screen(truth);
            total += send_frame((mirror_quality_t)level, false);
        }
        printf(" doom    motion %-9s %6zu B over 8 frames -> %6.1f KB/s at 8 fps\n",
               qn[level], total, total / 8.0 * 8.0 / 1024.0);
    }

    printf("\n=== idle: an unchanged screen must cost nothing ===\n");
    reset_all();
    mirror_force_keyframe();
    scr_menu(truth, 0);
    push_screen(truth);
    send_frame(MIRROR_Q_EXACT, true);

    push_screen(truth); /* LVGL repaints, but with identical pixels */
    const size_t idle = send_frame(MIRROR_Q_EXACT, false);
    if (idle != 0) { printf("  !! idle frame cost %zu B, expected 0\n", idle); failures++; }
    else printf("  ok  repaint with identical pixels emits nothing\n");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
