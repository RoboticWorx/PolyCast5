#include <stdint.h>
#include <stdbool.h>

#include "lcd_ir_exp_render.h"

/* =============== Interpolation tables =============== */

// a / b rounded to nearest, ties to even as lrintf() does. b > 0.
static int64_t irx_div_round(int64_t a, int64_t b)
{
    int64_t q = a / b;
    int64_t r = a % b;
    if (r < 0) {
        r += b;
        q--;
    }
    if (2 * r > b || (2 * r == b && (q & 1))) {
        q++;
    }
    return q;
}

// Precompute, for every output coordinate, the four source samples it reads and their Catmull-Rom weights in Q8.
// Built once at page entry so the per-frame inner loop is pure integer multiply-accumulate - what makes 8 fps
// affordable without an FPU.
//
// Exact rational arithmetic rather than float: for the page's two tables it reproduces the float build it replaced
// bit for bit (the host tests check), and the browser can recompute it exactly.
//
// Mirroring an axis only reverses the source indices; the weight order stays, as the Catmull-Rom basis is
// symmetric and idx[1]/idx[2] still bracket the output sample. The weight-sum correction below always lands on
// w[1], a different physical tap once reversed, so a mirrored axis is accurate to 1 LSB (0.02 C) rather than
// bit-identical - measured, and an order of magnitude under the sensor's own 0.21 C noise.
void irx_render_taps(irx_tap_t *taps, int dst_n, int src_n, bool flip)
{
    // Output centre in source space is num / den = (d + 0.5) * src_n / dst_n - 0.5
    const int64_t den = 2 * (int64_t)dst_n;
    const int64_t den3 = den * den * den;

    for (int d = 0; d < dst_n; ++d) {
        const int64_t num = (2 * (int64_t)d + 1) * src_n - dst_n;

        // Split into an index and a fraction t = r / den, flooring rather than truncating
        int64_t i0 = num / den;
        int64_t r = num % den;
        if (r < 0) {
            r += den;
            i0--;
        }

        // The Catmull-Rom basis scaled by 2 * den^3, so each weight is an exact integer until the final divide
        const int64_t r2 = r * r;
        const int64_t r3 = r2 * r;
        const int64_t n[4] = {
            -r3 + 2 * r2 * den - r * den * den,
            3 * r3 - 5 * r2 * den + 2 * den3,
            -3 * r3 + 4 * r2 * den + r * den * den,
            r3 - r2 * den,
        };

        int sum = 0;
        for (int k = 0; k < 4; ++k) {
            int si = (int)i0 - 1 + k;
            if (si < 0) si = 0; // Clamp at the edges rather than wrap
            if (si > src_n - 1) si = src_n - 1;
            if (flip) si = src_n - 1 - si; // Mirror this axis
            taps[d].idx[k] = (int16_t)si;
            taps[d].w[k] = (int16_t)irx_div_round(128 * n[k], den3); // 256 * n / (2 * den^3)
            sum += taps[d].w[k];
        }

        // Round-off can leave the weights summing to 255 or 257, which would tint flat
        // regions. Push the error into the dominant tap so the sum is exactly 256.
        taps[d].w[1] = (int16_t)(taps[d].w[1] + (256 - sum));
    }
}

/* =============== Render =============== */

// Separable Catmull-Rom upscale straight into the RGB565 framebuffer. Pass 1 resamples each source row to the full
// output width; pass 2 resamples down the columns and maps through the palette in the same loop, so no full-size
// intermediate is ever materialised.
//
// Both passes clamp the result to the two samples it sits between: Catmull-Rom overshoots, which shows as a bright
// halo beside a hot object, and bracketing removes it without softening genuine edges.
void irx_render_frame(const irx_scaler_t *s, const int16_t *src, int32_t lo, int32_t hi,
        const uint16_t *palette, uint16_t *out, int stride)
{
    // Pass 1: horizontal, src_w -> dst_w
    for (int r = 0; r < s->src_h; ++r) {
        const int16_t *srow = src + r * s->src_w;
        int16_t *drow = s->mid + r * s->dst_w;

        for (int x = 0; x < s->dst_w; ++x) {
            const irx_tap_t *tp = &s->tap_x[x];
            const int32_t s1 = srow[tp->idx[1]];
            const int32_t s2 = srow[tp->idx[2]];

            int32_t v = ((int32_t)srow[tp->idx[0]] * tp->w[0] + s1 * tp->w[1] +
                         s2 * tp->w[2] + (int32_t)srow[tp->idx[3]] * tp->w[3]) >> 8;

            const int32_t lo2 = (s1 < s2) ? s1 : s2;
            const int32_t hi2 = (s1 < s2) ? s2 : s1;
            if (v < lo2) v = lo2;
            else if (v > hi2) v = hi2;

            drow[x] = (int16_t)v;
        }
    }

    // Pass 2: vertical, src_h -> dst_h, plus normalise and palette
    const int32_t span = hi - lo;
    const int32_t recip = (span > 0) ? ((255 << 16) / span) : 0;

    for (int y = 0; y < s->dst_h; ++y) {
        const irx_tap_t *tp = &s->tap_y[y];
        const int16_t *r0 = s->mid + tp->idx[0] * s->dst_w;
        const int16_t *r1 = s->mid + tp->idx[1] * s->dst_w;
        const int16_t *r2 = s->mid + tp->idx[2] * s->dst_w;
        const int16_t *r3 = s->mid + tp->idx[3] * s->dst_w;
        uint16_t *row = out + y * stride;

        for (int x = 0; x < s->dst_w; ++x) {
            const int32_t s1 = r1[x];
            const int32_t s2 = r2[x];

            int32_t v = ((int32_t)r0[x] * tp->w[0] + s1 * tp->w[1] +
                         s2 * tp->w[2] + (int32_t)r3[x] * tp->w[3]) >> 8;

            const int32_t lo2 = (s1 < s2) ? s1 : s2;
            const int32_t hi2 = (s1 < s2) ? s2 : s1;
            if (v < lo2) v = lo2;
            else if (v > hi2) v = hi2;

            // Clamp into the displayed span BEFORE scaling. recip can reach (255 << 16) / IRX_MIN_SPAN, so an
            // out-of-range sample would overflow the multiply - signed overflow, not a wrong colour. Bounding
            // the difference first caps the product at 255 << 16 by construction.
            int32_t d = v - lo;
            if (d < 0) d = 0;
            else if (d > span) d = span;

            int32_t n = (d * recip) >> 16;
            if (n > 255) n = 255;

            row[x] = palette[n];
        }
    }
}

// Centre reticle. A dark pixel either side of each white core arm keeps it legible over black, white and every
// palette colour in between.
void irx_render_crosshair(uint16_t *out, int stride, int w, int h, uint16_t fg, uint16_t bg)
{
    const int cx = w / 2;
    const int cy = h / 2;

    for (int d = 2; d <= 7; ++d) { // Leave a 2 px gap so the centre stays visible
        for (int s = -1; s <= 1; ++s) {
            const uint16_t col = (s == 0) ? fg : bg;

            const int yy = cy + s;
            if (yy >= 0 && yy < h) {
                if (cx - d >= 0) out[yy * stride + (cx - d)] = col;
                if (cx + d < w) out[yy * stride + (cx + d)] = col;
            }

            const int xx = cx + s;
            if (xx >= 0 && xx < w) {
                if (cy - d >= 0) out[(cy - d) * stride + xx] = col;
                if (cy + d < h) out[(cy + d) * stride + xx] = col;
            }
        }
    }
}
