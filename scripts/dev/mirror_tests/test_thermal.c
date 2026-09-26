/* Host tests for the Screen Mirror thermal channel.
 *
 * 1. The page's integer tap builder against the float one it replaced.
 * 2. Render vectors: real THERMAL messages plus the canvas lcd_ir_exp_render.c draws from
 *    them, for test_thermal.js to reproduce pixel for pixel in the browser decoder.
 * 3. A scripted session through the real publish, encode and tile paths, written as a
 *    stream with the screen the viewer must show at each checkpoint. test_thermal.js
 *    replays it through the real canvas sink.
 *
 *   test_thermal <render.bin> <stream.bin> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "mirror.h"
#include "mirror_priv.h"
#include "mirror_proto.h"
#include "lcd_ir_exp_render.h"

/* The thermal page's geometry, as lcd_ir_exp.c */
#define CW 180
#define CH 135
#define SW 32
#define SH 24

#define W MIRROR_SCR_W
#define H MIRROR_SCR_H

static int failures = 0;

static void ok(const char *what) { printf("  ok  %s\n", what); }
static void bad(const char *what) { printf("  !! %s\n", what); failures++; }

static uint32_t rng = 20260926u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static int rnd_in(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* ---- 1. taps ---- */

/* The float builder lcd_ir_exp.c used before the render math moved out, verbatim */
static void float_taps(irx_tap_t *taps, int dst_n, int src_n, bool flip)
{
    const float scale = (float)src_n / (float)dst_n;

    for (int d = 0; d < dst_n; ++d) {
        const float sx = ((float)d + 0.5f) * scale - 0.5f;
        const int i0 = (int)floorf(sx);
        const float t = sx - (float)i0;

        const float t2 = t * t;
        const float t3 = t2 * t;
        float w[4];
        w[0] = -0.5f * t3 + t2 - 0.5f * t;
        w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
        w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
        w[3] = 0.5f * t3 - 0.5f * t2;

        int sum = 0;
        for (int k = 0; k < 4; ++k) {
            int si = i0 - 1 + k;
            if (si < 0) si = 0;
            if (si > src_n - 1) si = src_n - 1;
            if (flip) si = src_n - 1 - si;
            taps[d].idx[k] = (int16_t)si;
            taps[d].w[k] = (int16_t)lrintf(w[k] * 256.0f);
            sum += taps[d].w[k];
        }

        taps[d].w[1] = (int16_t)(taps[d].w[1] + (256 - sum));
    }
}

static void test_taps(void)
{
    printf("=== integer taps vs the float build they replaced ===\n");

    static irx_tap_t a[256], b[256];
    const int page[][3] = { { CW, SW, 0 }, { CH, SH, 1 }, { CW, SW, 1 }, { CH, SH, 0 } };

    for (unsigned c = 0; c < sizeof(page) / sizeof(page[0]); c++) {
        irx_render_taps(a, page[c][0], page[c][1], page[c][2] != 0);
        float_taps(b, page[c][0], page[c][1], page[c][2] != 0);

        char what[96];
        snprintf(what, sizeof(what), "%d from %d%s: bit-identical to the float build",
                page[c][0], page[c][1], page[c][2] ? ", flipped" : "");

        if (memcmp(a, b, sizeof(irx_tap_t) * (size_t)page[c][0]) != 0) bad(what);
        else ok(what);
    }

    int sums = 0;
    for (int dst = 1; dst <= 240; dst++) {
        for (int src = 1; src <= 64; src += 7) {
            irx_render_taps(a, dst, src, (dst & 1) != 0);
            for (int d = 0; d < dst; d++) {
                const int s = a[d].w[0] + a[d].w[1] + a[d].w[2] + a[d].w[3];
                for (int k = 0; k < 4; k++) if (a[d].idx[k] < 0 || a[d].idx[k] >= src) sums++;
                if (s != 256) sums++;
            }
        }
    }
    if (sums) bad("weights sum to 256 and indices stay in range at every size");
    else ok("weights sum to 256 and indices stay in range at every size");
}

/* ---- shared: the device side of one session step ---- */

static uint8_t msgbuf[MIRROR_MSG_MAX_BYTES];
static uint16_t seq = 0;
static FILE *fs = NULL; /* stream.bin, when a session is being recorded */
static size_t step_th_bytes = 0, step_tile_bytes = 0;
static uint16_t step_tiles = 0;

static void emit_msg(const uint8_t *m, size_t n)
{
    if (fs == NULL) return;
    const uint32_t len = (uint32_t)n;
    fputc('M', fs);
    fwrite(&len, 4, 1, fs);
    fwrite(m, 1, n, fs);
}

/* mirror_task's send branch: thermal first, then the tile frame */
static void device_step(mirror_quality_t q, bool keyframe)
{
    step_th_bytes = step_tile_bytes = 0;
    step_tiles = 0;

    if (keyframe) mirror_force_keyframe();

    const size_t n = mirror_thermal_encode(keyframe, (uint16_t)(seq + 1), msgbuf, sizeof(msgbuf));
    if (n > 0) {
        if (n > MIRROR_MSG_MAX_BYTES) bad("thermal message larger than the send buffer");
        emit_msg(msgbuf, n);
        seq++;
        mirror_thermal_commit();
        step_th_bytes = n;
    }

    step_tiles = mirror_encode_frame(q);
    if (step_tiles > 0) {
        const uint16_t fseq = (n > 0) ? seq : (uint16_t)(seq + 1); /* Shares the THERMAL's */
        const uint16_t msgs = mirror_encode_msg_count();
        for (uint16_t i = 0; i < msgs; i++) {
            const size_t m = mirror_encode_build_msg(i, fseq, keyframe, q, msgbuf,
                    sizeof(msgbuf));
            emit_msg(msgbuf, m);
            step_tile_bytes += m;
        }
        seq = fseq;
    }
}

static void push_screen(const uint16_t *s)
{
    for (int y = 0; y < H; y += 20) {
        const int y2 = (y + 19 < H) ? y + 19 : H - 1;
        mirror_capture(0, (int16_t)y, W - 1, (int16_t)y2, &s[(size_t)y * W]);
    }
}

/* ---- the page, simulated ---- */

static irx_tap_t tap_x[CW], tap_y[CH];
static int16_t mid[SH * CW];
static irx_scaler_t sc;

static int16_t frame[SW * SH];
static uint16_t pal[256];
static uint16_t fb[CW * CH]; /* The canvas as LVGL would show it */
static int32_t irx_lo, irx_hi;
static bool range_seeded = false;

static uint16_t truth[W * H]; /* The whole screen as the panel shows it */

static const mirror_rect_t CANVAS = { 0, 0, CW - 1, CH - 1 };
static const mirror_rect_t LABEL = { 2, 114, 79, 132 }; /* The status label's box */

static void make_palette(int which)
{
    for (int i = 0; i < 256; i++) {
        const int r = (which == 0) ? i : 255 - i, g = (i * 3 + which * 40) & 0xFF, b = (i * 7) & 0xFF;
        pal[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

/* A warm object on a cooler background plus the sensor's ~0.21 C (about 10 LSB) noise */
static void sense(int t)
{
    int32_t mn = INT32_MAX, mx = INT32_MIN;

    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            const int dx = x - 12 - (t % 5), dy = y - 10;
            int v = 1100 + x * 3 + ((dx * dx + dy * dy < 30) ? 600 : 0);
            v += (int)(rnd() % 21) - 10;
            frame[y * SW + x] = (int16_t)v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    }

    /* The page's auto-range */
    int32_t lo = mn, hi = mx;
    if (hi - lo < 150) { const int32_t m = (hi + lo) / 2; lo = m - 75; hi = m + 75; }
    if (!range_seeded) { irx_lo = lo; irx_hi = hi; range_seeded = true; }
    else {
        irx_lo += (lo - irx_lo) >> 2;
        irx_hi += (hi - irx_hi) >> 2;
        if (irx_hi - irx_lo < 150) irx_hi = irx_lo + 150;
    }
}

static void render_and_publish(void)
{
    irx_render_frame(&sc, frame, irx_lo, irx_hi, pal, fb, CW);
    irx_render_crosshair(fb, CW, CW, CH, 0xFFFF, 0x0000);

    const mirror_thermal_frame_t f = {
        .px = frame, .cols = SW, .rows = SH, .lo = irx_lo, .hi = irx_hi, .palette = pal,
        .flip_h = false, .flip_v = true, .crosshair = true, .cross_fg = 0xFFFF, .cross_bg = 0x0000,
    };
    mirror_thermal_frame(&f);
}

static void publish_view(bool on, bool label)
{
    mirror_thermal_view(on, &CANVAS, label ? &LABEL : NULL, label ? 1 : 0);
}

/* Compose what the panel shows: canvas, readout panel, and the label over the canvas */
static void compose(bool label, int panel)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t c;
            if (x < CW) {
                c = fb[y * CW + x];
            } else {
                /* Readout column: text-ish rows whose content moves with panel */
                const int row = y / 12, glyph = ((x - CW) / 5 + row + panel) % 4;
                c = (y >= 20 && (y % 12) < 9 && glyph < 2) ? 0xFFFF : 0x18E3;
            }
            truth[y * W + x] = c;
        }
    }

    if (label) {
        for (int y = LABEL.y1; y <= LABEL.y2; y++)
            for (int x = LABEL.x1; x <= LABEL.x2; x++)
                truth[y * W + x] = (((x / 3) + (y / 4)) % 3 == 0) ? 0xFFFF : 0x0841;
    }
}

static void compose_menu(int phase)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            truth[y * W + x] = ((y / 24 + phase) % 2 && (x / 6) % 3) ? 0xFFFF : 0x0000;
}

static uint16_t qmask(mirror_quality_t q)
{
    return q == MIRROR_Q_G5 ? 0xFFDF : (q >= MIRROR_Q_444 ? 0xF79E : 0xFFFF);
}

static bool tile_hits(int tile, const mirror_rect_t *r)
{
    const int x1 = (tile % MIRROR_GRID_COLS) * MIRROR_TILE_W, y1 = (tile / MIRROR_GRID_COLS) * MIRROR_TILE_H;
    return x1 <= r->x2 && r->x1 <= x1 + MIRROR_TILE_W - 1 && y1 <= r->y2 && r->y1 <= y1 + MIRROR_TILE_H - 1;
}

/* What the viewer must show, worked out independently of mirror_thermal.c: exact truth on
 * the canvas part of a thermal tile, the tile path's version of truth everywhere else */
static void checkpoint(const char *name, bool th_on, bool label, mirror_quality_t q)
{
    static uint16_t e[W * H];
    const uint16_t m = qmask(q);
    const bool half = (q == MIRROR_Q_444_HALF);

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const int tile = (y / MIRROR_TILE_H) * MIRROR_GRID_COLS + x / MIRROR_TILE_W;
            const bool owned = th_on && tile_hits(tile, &CANVAS) && !(label && tile_hits(tile, &LABEL));

            if (owned && x <= CANVAS.x2 && y <= CANVAS.y2) {
                e[y * W + x] = truth[y * W + x];
            } else if (half) {
                const int tx = (x / MIRROR_TILE_W) * MIRROR_TILE_W, ty = (y / MIRROR_TILE_H) * MIRROR_TILE_H;
                const int sx = tx + ((x - tx) & ~1), sy = ty + ((y - ty) & ~1);
                e[y * W + x] = (uint16_t)(truth[sy * W + sx] & m);
            } else {
                e[y * W + x] = (uint16_t)(truth[y * W + x] & m);
            }
        }
    }

    const uint8_t nl = (uint8_t)strlen(name);
    fputc('E', fs);
    fputc(nl, fs);
    fwrite(name, 1, nl, fs);
    fwrite(e, sizeof(uint16_t), W * H, fs);
}

static void viewer_reload(void)
{
    fputc('R', fs);
}

/* ---- 2. render vectors ---- */

static void write_taps(FILE *f, int dst, int src, bool flip)
{
    static irx_tap_t t[256];
    irx_render_taps(t, dst, src, flip);

    const uint16_t d16 = (uint16_t)dst, s16 = (uint16_t)src;
    fputc('T', f);
    fwrite(&d16, 2, 1, f);
    fwrite(&s16, 2, 1, f);
    fputc(flip ? 1 : 0, f);
    for (int d = 0; d < dst; d++) fwrite(t[d].idx, sizeof(int16_t), 4, f);
    for (int d = 0; d < dst; d++) fwrite(t[d].w, sizeof(int16_t), 4, f);
}

static void test_vectors(const char *path)
{
    printf("\n=== render vectors for the browser port ===\n");

    FILE *f = fopen(path, "wb");
    if (!f) { perror("open render.bin"); failures++; return; }

    /* Every table the page builds, plus a spread of others */
    write_taps(f, CW, SW, false);
    write_taps(f, CH, SH, true);
    for (int i = 0; i < 40; i++) write_taps(f, rnd_in(1, 240), rnd_in(1, 64), (rnd() & 1) != 0);

    static irx_tap_t tx[W], ty[H];
    static int16_t md[64 * W];
    static int16_t px[MIRROR_THERMAL_MAX_PX];
    static uint16_t p[256], out[W * H];
    int cases = 0;

    for (int c = 0; c < 60; c++) {
        /* The first case is the page exactly; the rest wander */
        const bool page = (c < 20);
        const int cols = page ? SW : rnd_in(1, 40);
        const int rows = page ? SH : rnd_in(1, MIRROR_THERMAL_MAX_PX / cols < 40 ? MIRROR_THERMAL_MAX_PX / cols : 40);
        const int w = page ? CW : rnd_in(1, W), h = page ? CH : rnd_in(1, H);
        const int x = page ? 0 : rnd_in(0, W - w), y = page ? 0 : rnd_in(0, H - h);
        const bool fh = page ? false : (rnd() & 1), fv = page ? true : (rnd() & 1);
        const bool cross = page ? true : (rnd() & 1);

        /* Plausible sensor values, then full-range junk, then the extremes */
        for (int i = 0; i < cols * rows; i++) {
            px[i] = (c % 3 == 0) ? (int16_t)rnd_in(-2000, 15000)
                  : (c % 3 == 1) ? (int16_t)(rnd() & 0xFFFF)
                  : (int16_t)((rnd() & 1) ? 32767 : -32768);
        }
        for (int i = 0; i < 256; i++) p[i] = (uint16_t)rnd();

        int32_t lo = rnd_in(-40000, 40000), hi;
        switch (c % 5) {
            case 0: hi = lo + 150; break;                 /* the page's minimum span */
            case 1: hi = lo + rnd_in(1, 3000); break;
            case 2: hi = lo; break;                       /* no span */
            case 3: hi = lo - rnd_in(1, 500); break;      /* inverted */
            default: hi = lo + 1; break;                  /* steepest scale */
        }

        const mirror_thermal_frame_t fr = {
            .px = px, .cols = (uint8_t)cols, .rows = (uint8_t)rows, .lo = lo, .hi = hi, .palette = p,
            .flip_h = fh, .flip_v = fv, .crosshair = cross,
            .cross_fg = (uint16_t)rnd(), .cross_bg = (uint16_t)rnd(),
        };
        const mirror_rect_t cv = { (int16_t)x, (int16_t)y, (int16_t)(x + w - 1), (int16_t)(y + h - 1) };

        mirror_thermal_frame(&fr);
        mirror_thermal_view(true, &cv, NULL, 0);

        const size_t n = mirror_thermal_encode(true, (uint16_t)c, msgbuf, sizeof(msgbuf));
        if (n == 0) { bad("thermal encode produced nothing for a live canvas"); continue; }
        mirror_thermal_commit();

        irx_render_taps(tx, w, cols, fh);
        irx_render_taps(ty, h, rows, fv);
        const irx_scaler_t s = { tx, ty, md, cols, rows, w, h };
        irx_render_frame(&s, px, lo, hi, p, out, w);
        if (cross) irx_render_crosshair(out, w, w, h, fr.cross_fg, fr.cross_bg);

        const uint32_t len = (uint32_t)n;
        const uint16_t w16 = (uint16_t)w, h16 = (uint16_t)h;
        fputc('V', f);
        fwrite(&len, 4, 1, f);
        fwrite(msgbuf, 1, n, f);
        fwrite(&w16, 2, 1, f);
        fwrite(&h16, 2, 1, f);
        fwrite(out, sizeof(uint16_t), (size_t)w * h, f);
        cases++;
    }

    mirror_thermal_stop();
    fclose(f);
    printf("      wrote 42 tap tables and %d render cases\n", cases);
}

/* ---- 3. a scripted session ---- */

static void test_session(const char *path)
{
    printf("\n=== scripted session: exclusion, overlays, lifecycle ===\n");

    fs = fopen(path, "wb");
    if (!fs) { perror("open stream.bin"); failures++; return; }

    irx_render_taps(tap_x, CW, SW, false);
    irx_render_taps(tap_y, CH, SH, true);
    sc = (irx_scaler_t){ tap_x, tap_y, mid, SW, SH, CW, CH };
    make_palette(0);

    mirror_thermal_reset();
    mirror_force_keyframe();

    /* A menu, before the page: the channel must stay silent */
    compose_menu(0);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, true);
    if (step_th_bytes != 0) bad("a THERMAL message went out with no thermal page");
    else ok("no THERMAL message while the thermal page is not shown");
    checkpoint("menu", false, false, MIRROR_Q_EXACT);

    /* Page up, black canvas, WARMING UP, no frame yet: tiles only */
    memset(fb, 0, sizeof(fb));
    compose(true, 0);
    push_screen(truth);
    publish_view(false, true);
    device_step(MIRROR_Q_EXACT, false);
    if (step_th_bytes != 0) bad("channel opened before the first frame");
    checkpoint("warming", false, true, MIRROR_Q_EXACT);

    /* First frame, STABILIZING over the corner */
    sense(0);
    render_and_publish();
    publish_view(true, true);
    compose(true, 0);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    if (step_th_bytes == 0) bad("first frame did not open the channel");
    checkpoint("first frame, label", true, true, MIRROR_Q_EXACT);

    /* Noisy frames with the label: only the label's tiles may churn */
    size_t churn = 0;
    for (int t = 1; t <= 8; t++) {
        sense(t);
        render_and_publish();
        compose(true, 0);
        push_screen(truth);
        device_step(MIRROR_Q_EXACT, false);
        churn += step_tiles;
        char name[32];
        snprintf(name, sizeof(name), "label frame %d", t);
        checkpoint(name, true, true, MIRROR_Q_EXACT);
    }
    {
        int label_tiles = 0;
        for (int tile = 0; tile < MIRROR_TILE_CNT; tile++) if (tile_hits(tile, &LABEL)) label_tiles++;
        printf("      label up: %.1f tiles a frame (the label covers %d)\n", churn / 8.0, label_tiles);
        if (churn > (size_t)label_tiles * 8) bad("tiles outside the label churned");
        else ok("only the label's tiles churn while it is up");
    }

    /* Latest wins: three frames published between sends, one goes out */
    for (int t = 9; t <= 11; t++) { sense(t); render_and_publish(); }
    compose(true, 0);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("latest of three", true, true, MIRROR_Q_EXACT);

    /* Label gone: steady state */
    publish_view(true, false);
    compose(false, 0);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("label gone", true, false, MIRROR_Q_EXACT);

    size_t th = 0, tiles = 0;
    for (int t = 12; t <= 19; t++) {
        sense(t);
        render_and_publish();
        compose(false, 0);
        push_screen(truth);
        device_step(MIRROR_Q_EXACT, false);
        th += step_th_bytes;
        tiles += step_tile_bytes;
        char name[32];
        snprintf(name, sizeof(name), "steady frame %d", t);
        checkpoint(name, true, false, MIRROR_Q_EXACT);
    }
    printf("      steady: THERMAL %zu B + tiles %zu B over 8 frames -> %.1f KB/s at 8 fps\n",
            th, tiles, (double)(th + tiles) / 1024.0);
    if (tiles != 0) bad("canvas noise leaked into the tile stream");
    else ok("canvas noise never reaches the tile stream");

    /* Readout changes: the panel side of column 11 must still go out */
    sense(20);
    render_and_publish();
    compose(false, 1);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    if (step_tiles == 0) bad("a panel change went nowhere");
    checkpoint("panel changes", true, false, MIRROR_Q_EXACT);

    /* Palette change */
    make_palette(1);
    sense(21);
    render_and_publish();
    compose(false, 1);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    if (step_th_bytes < (size_t)MIRROR_THERMAL_HDR_BYTES + 512) bad("palette change sent no palette");
    checkpoint("palette changed", true, false, MIRROR_Q_EXACT);

    /* Frozen with HOLD up: no new frame, the hole alone moves */
    publish_view(true, true);
    compose(true, 1);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("hold", true, true, MIRROR_Q_EXACT);
    publish_view(true, false);
    compose(false, 1);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("hold released", true, false, MIRROR_Q_EXACT);

    /* The viewer reloads its tab: a keyframe must rebuild everything, palette included */
    viewer_reload();
    device_step(MIRROR_Q_EXACT, true);
    checkpoint("after reload", true, false, MIRROR_Q_EXACT);

    /* Quality drops: tiles quantize and halve, the thermal part stays exact */
    for (int q = MIRROR_Q_G5; q <= MIRROR_Q_444_HALF; q++) {
        sense(21 + q);
        render_and_publish();
        publish_view(true, q == MIRROR_Q_444);
        compose(q == MIRROR_Q_444, 2 + q);
        push_screen(truth);
        device_step((mirror_quality_t)q, true);
        char name[32];
        snprintf(name, sizeof(name), "quality %d", q);
        checkpoint(name, true, q == MIRROR_Q_444, (mirror_quality_t)q);
    }
    device_step(MIRROR_Q_EXACT, true);
    checkpoint("quality back", true, false, MIRROR_Q_EXACT);

    /* Detail view hides the canvas */
    publish_view(false, false);
    compose_menu(1);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("detail view", false, false, MIRROR_Q_EXACT);

    /* Back to the image: the old frame is still on the canvas */
    publish_view(true, false);
    compose(false, 2);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    if (step_th_bytes < (size_t)MIRROR_THERMAL_HDR_BYTES + 512) bad("reopening did not resend the palette");
    checkpoint("back from detail", true, false, MIRROR_Q_EXACT);

    /* Redaction: the channel closes and nothing but placeholder remains */
    mirror_set_redacted(true);
    device_step(MIRROR_Q_EXACT, false);
    for (int i = 0; i < W * H; i++) truth[i] = MIRROR_REDACT_COLOR;
    checkpoint("redacted", false, false, MIRROR_Q_EXACT);
    {
        sense(30);
        render_and_publish(); /* even a new frame must not reopen it */
        device_step(MIRROR_Q_EXACT, false);
        if (step_th_bytes != 0) bad("a THERMAL message went out while redacted");
        else ok("nothing thermal goes out while redacted");
        checkpoint("redacted, new frame", false, false, MIRROR_Q_EXACT);
    }
    mirror_set_redacted(false);
    compose(false, 2);
    push_screen(truth); /* the LCD invalidates on lift */
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("redaction lifted", true, false, MIRROR_Q_EXACT);

    /* Leaving the page. irx_cleanup stops the channel and then blocks up to 3.5 s parking
       the sensors with no flush, so only the stop itself can put the canvas back */
    mirror_thermal_stop();
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("stopped, nothing flushed", false, false, MIRROR_Q_EXACT);

    compose_menu(2);
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    checkpoint("page left", false, false, MIRROR_Q_EXACT);

    /* Nothing left behind: an unchanged screen costs nothing and sends nothing thermal */
    push_screen(truth);
    device_step(MIRROR_Q_EXACT, false);
    if (step_th_bytes || step_tiles) bad("the page left something behind");
    else ok("after the page, an unchanged screen sends nothing");
    device_step(MIRROR_Q_EXACT, true);
    if (step_th_bytes) bad("a keyframe off the page sent a THERMAL message");
    checkpoint("keyframe after page", false, false, MIRROR_Q_EXACT);

    fclose(fs);
    fs = NULL;
}

/* ---- what the channel saves ---- */

static void test_savings(void)
{
    printf("\n=== noisy canvas: tile path vs thermal channel ===\n");

    irx_render_taps(tap_x, CW, SW, false);
    irx_render_taps(tap_y, CH, SH, true);
    sc = (irx_scaler_t){ tap_x, tap_y, mid, SW, SH, CW, CH };
    make_palette(0);

    for (int use = 0; use < 2; use++) {
        mirror_thermal_stop();
        mirror_thermal_reset();
        range_seeded = false;

        sense(0);
        render_and_publish();
        publish_view(use == 1, false);
        compose(false, 0);
        push_screen(truth);
        device_step(MIRROR_Q_EXACT, true);

        size_t bytes = 0, tiles = 0;
        for (int t = 1; t <= 16; t++) {
            sense(t);
            render_and_publish();
            compose(false, 0);
            push_screen(truth);
            device_step(MIRROR_Q_EXACT, false);
            bytes += step_th_bytes + step_tile_bytes;
            tiles += step_tiles;
        }

        printf("      %-15s %5.1f tiles a frame, %6.1f KB/s at 8 fps\n",
                use ? "thermal channel" : "tile path", tiles / 16.0, bytes / 2.0 / 1024.0);

        static size_t tile_path = 0;
        if (!use) tile_path = bytes;
        else if (bytes * 4 > tile_path) bad("the thermal channel is not at least 4x cheaper");
        else ok("the thermal channel is at least 4x cheaper than the tile path");
    }

    mirror_thermal_stop();
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_thermal <render.bin> <stream.bin>\n"); return 2; }

    mirror_state = MIRROR_LIVE;

    test_taps();
    test_vectors(argv[1]);
    test_session(argv[2]);
    test_savings();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
