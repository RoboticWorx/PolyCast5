#include "lcd_voice_orb.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"

#ifdef POLYCAST5_SIM
#include "sim_compat.h"
#else
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR (POLYCAST5_USE_PSRAM_BSS)
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "ai_voice.h"       // ai_voice_take_level_peak()
#include "lcd_utils.h"      // user_primary_color, user_secondary_color
#include "polycast5_macros.h"
#endif

static const char *TAG = "LCD_ORB";

/* ===========================================================================
 * Geometry and tuning
 *
 * The canvas is the whole cost, and the cost is the flush rather than the drawing. 96 x 96 is
 * 7.4 ms of bit time at the panel's 20 MHz SPI, but st7789_flush_cb walks the area in
 * FLUSH_CHUNK = 2 row slices and issues six blocking spi_device_transmit calls per slice, so the
 * real figure is nearer 9-13 ms once driver overhead is counted. Everything else - the outline,
 * the span fill, the palette - is well under a millisecond together. 96 also happens to match the
 * 91 x 91 box the old image orb invalidated, so the on-screen footprint is unchanged.
 *
 * If this ever starves ai_task (same core, one blocking i2s_channel_read every 5.33 ms behind
 * 16.7 ms of DMA), raise ORB_FRAME_PERIOD first; after that the lever is FLUSH_CHUNK in
 * lcd_utils.c, not the canvas size.
 * ========================================================================= */

#define ORB_W 96
#define ORB_H 96

// Pixel centres: pixel i covers [i, i+1), so its centre is i + 0.5 and the canvas centre is W/2
#define ORB_CX_Q8 ((ORB_W << 7) - 128)
#define ORB_CY_Q8 ((ORB_H << 7) - 128)

#define ORB_FRAME_PERIOD 33 // Matches LV_DEF_REFR_PERIOD. Effective rate is nearer 20-25 fps

#define ORB_R_MIN 10 // Blob radius at silence
#define ORB_R_MAX 36 // Blob radius at full level. With ORB_DEFORM_GAIN 100 a lobe reaches
                     // 41.5 px, so the ring still clears it at any gain

#define ORB_RING_R 44 // Guide ring: fixed, shows the headroom the blob has left
#define ORB_RING_HALF_Q8 154 // Ring half-thickness, Q8 (0.6 px -> a hairline after AA)
#define ORB_RIPPLE_MAX_R 46 // Ripples fade out before they reach the canvas edge (48)

#define ORB_ANG 256 // Outline samples. At R_MAX the spacing is 0.88 px, so rows never gap
#define ORB_RAMP_N 64 // Radial palette steps; RGB565 cannot resolve more than this anyway

// Outline deformation, Q15. OFF by default: the orb is a true circle that changes size, and the
// motion comes from the radius, the ripples and the glow rather than from the shape.
//
// Raise ORB_DEFORM_GAIN to bring the liquid wobble back (35 is a gentle undulation, 100 is strongly organic)
#ifndef ORB_H1_ORDER
#define ORB_H1_ORDER 3
#endif
#ifndef ORB_H2_ORDER
#define ORB_H2_ORDER 5
#endif
#ifndef ORB_H1_BASE_Q15
#define ORB_H1_BASE_Q15 929 // 0.028 on the lower harmonic
#endif
#ifndef ORB_H2_BASE_Q15
#define ORB_H2_BASE_Q15 825 // 0.025 on the upper. Near-equal on purpose: let either dominate and
                            // the blob reads as one rotating lobe count rather than an organic
                            // shape. Shallow too - three times this stopped reading as a circle
#endif
#define ORB_H1_STEP 1 // Phase advance per frame, in 1/256 turn units...
#define ORB_H2_STEP -2 // ...deliberately not a ratio of each other, so the shape never repeats

// Envelope, evaluated once per frame. Verified against a 33 ms frame: attack reaches 90% in two
// frames (66 ms), release has tau ~48 ms and is back at rest in ~165 ms
#define ORB_ATTACK_NUM 6 // env += ((tgt - env) * 6) >> 3
#ifndef ORB_RELEASE_SH
#define ORB_RELEASE_SH 1 // env -= env >> 1
#endif
#define ORB_NOISE_FLOOR_MAX 3000 // Ceiling, so a sustained loud tone cannot deafen the orb

// Auto-range reference. The mic's absolute level is not something this can assume: the STT path
// applies up to 20x make-up gain to reach its own target peak (AI_VOICE_NORMALIZE_MAX_GAIN), so
// ordinary speech arrives a long way below full scale and how far below depends on how close the
// user is. Normalising against the loudest thing heard recently is what makes the orb use its
// whole radius either way - against a fixed full-scale map it barely twitched
#define ORB_REF_MIN 300   // Quietest reference allowed, so room tone cannot drive full deflection
#define ORB_REF_DECAY_SH 7 // Reference sags back toward ORB_REF_MIN over ~3 s

#define ORB_BREATH_STEP 2 // Idle breathing phase advance, 1/256 turn per frame (~4.2 s cycle)
#define ORB_BREATH_AMP_Q8 384 // +/- 1.5 px, and only while quiet

#define ORB_RIPPLE_MAX 3 // Concurrent ripples
#define ORB_RIPPLE_TRIGGER 40 // Level jump in one frame that counts as an onset (0..255)
#define ORB_RIPPLE_SPEED_Q8 282 // ~1.1 px per frame outward

// Radial ramp shape, as a fraction of the blob radius. The body has to stay solid most of the
// way out or the outline deformation is invisible under the falloff - a pure soft glow reads as
// a static circle no matter how the edge is actually moving
#ifndef ORB_STOP_BODY
#define ORB_STOP_BODY 0.60f // Core fades into full accent by here
#endif
#ifndef ORB_STOP_RIM
#define ORB_STOP_RIM 0.90f // Accent holds out to here, then falls away
#endif
#ifndef ORB_EDGE_BLEND
#define ORB_EDGE_BLEND 0.70f // How far the last stop is pulled to the page colour
#endif
#ifndef ORB_DEFORM_GAIN
#define ORB_DEFORM_GAIN 0 // Scales both harmonics, in 1/100ths. 0 = a perfect circle
#endif

// Largest radius the sqrt table must answer for: ring and ripple spans reach ORB_RIPPLE_MAX_R,
// the blob only ~R_MAX * 1.1. Indexed by integer r^2, so the table is r^2 entries, not r
#define ORB_SQRT_MAX_R 48
#define ORB_SQRT_N (ORB_SQRT_MAX_R * ORB_SQRT_MAX_R + 1)

/* ===========================================================================
 * State
 * ========================================================================= */

static lv_obj_t     *s_canvas;    // Our LVGL canvas; NULL = uninitialized
static lv_draw_buf_t s_draw_buf;  // LVGL descriptor wrapping s_pixels as RGB565
static uint8_t      *s_pixels;    // PSRAM framebuffer, ORB_W * ORB_H * 2 bytes
static size_t        s_buf_size;  // Byte size of s_pixels
static int           s_stride_px; // Row stride in PIXELS (the draw-buf stride is in bytes)
static lv_timer_t   *s_timer;     // Drives one envelope step + one repaint per frame

// Built once at init so the frame path is table lookups and shifts
POLYCAST5_USE_PSRAM_BSS static uint16_t s_sqrt[ORB_SQRT_N];  // r^2 -> r, Q4
POLYCAST5_USE_PSRAM_BSS static uint8_t  s_lvl_lut[256];      // (peak >> 7) -> level 0..255

// One sine period serves the outline, the harmonics and the breathing; cosine is the same table
// read a quarter turn along. Read 512 times a frame but strictly in order over 512 bytes, so it
// is resident after the first pass and costs nothing extra out in PSRAM
POLYCAST5_USE_PSRAM_BSS static int16_t s_sin_tab[ORB_ANG]; // Q15

#if ORB_DEFORM_GAIN > 0
// Angle of every canvas pixel about the centre, as an outline bin, so the shader can ask "what is
// R(theta) here" with a byte load instead of an atan2. Only needed when the outline deforms: a
// circle has one radius at every angle, so the flat scalar below serves instead and this 9 KB
// field, its build, and a table read per pixel all disappear
POLYCAST5_USE_PSRAM_BSS static uint8_t s_ang[ORB_W * ORB_H];
POLYCAST5_USE_PSRAM_BSS static uint8_t s_atan_lut[65]; // (min/max)*64 -> angle within an octant
static int32_t s_out_inv[ORB_ANG];       // This frame's 1/R(theta) per bin, scaled for the ramp
#else
static int32_t s_out_inv_flat;           // 1/R, the same at every angle
#endif

// Internal SRAM: read once per pixel and small enough that keeping it close is free
static uint16_t s_ramp[ORB_RAMP_N];      // Radial palette, RGB565, index 0 = core
static uint16_t s_bg;                    // user_primary_color as RGB565, the canvas ground
static uint16_t s_ring_col;              // Guide ring colour
// Where the outline crosses each row centre, Q8, kept sorted. At the shipped orders and depth the
// shape stays star-shaped and every row has exactly two, but raising ORB_DEFORM_GAIN or moving to
// a higher harmonic pair does produce genuine dents (4 crossings, measured), and filling min to
// max would quietly paint over them. Even-odd pairs render a dent as the background it is
#define ORB_XS_MAX 8
POLYCAST5_USE_PSRAM_BSS static int32_t s_row_xs[ORB_H][ORB_XS_MAX];
POLYCAST5_USE_PSRAM_BSS static uint8_t s_row_n[ORB_H];

// Outline vertices, Q8. Static rather than local: 2 KB of arrays inside a function called from
// lcd_task, whose whole stack is 8 KB, is not worth the risk for no gain
POLYCAST5_USE_PSRAM_BSS static int32_t s_px[ORB_ANG + 1];
POLYCAST5_USE_PSRAM_BSS static int32_t s_py[ORB_ANG + 1];

// Envelope and motion
static int32_t  s_env;          // Smoothed level, 0..255
static int32_t  s_env_prev;     // Previous frame's env, for onset detection
static uint16_t s_noise_floor;  // Slowly adapted peak floor, in raw peak units
static int32_t  s_level_ref;    // Loudest recent level; the orb's full-radius reference
static uint8_t  s_h1_phase;     // Outline harmonic phases, 1/256 turn
static uint8_t  s_h2_phase;
static uint8_t  s_breath_phase;

typedef struct {
    int32_t r_q8;  // Current radius
    int32_t r0_q8; // Where it spawned, which is wherever the blob edge was at the onset
    int32_t life;  // 255 down to 0; also the fade
} orb_ripple_t;

static orb_ripple_t s_ripple[ORB_RIPPLE_MAX];

/* ===========================================================================
 * Fixed-point helpers
 * ========================================================================= */

// Blend b over a. t is 0..255 (0 = all a). Only ever runs on the 1-2 partial pixels at a span
// edge, so the per-channel unpack costs nothing worth optimising away
static inline uint16_t orb_mix565(uint16_t a, uint16_t b, int32_t t)
{
    int32_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int32_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;

    ar += ((br - ar) * t) >> 8;
    ag += ((bg - ag) * t) >> 8;
    ab += ((bb - ab) * t) >> 8;

    return (uint16_t)((ar << 11) | (ag << 5) | ab);
}

static inline int32_t orb_clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// sqrt(x) for an integer square, Q4 out. Clamped rather than asserted: a wobble overshoot
// indexing one past the table is a rendering artefact, a read off the end is a crash
static inline int32_t orb_sqrt_q4(int32_t r2)
{
    if (r2 < 0) return 0;
    if (r2 >= ORB_SQRT_N) r2 = ORB_SQRT_N - 1;

    return s_sqrt[r2];
}

/* ===========================================================================
 * One-off table builders
 *
 * Float is fine in here and nowhere else: this runs once when the page opens, while the frame
 * path below has to stay integer (the C5 has no FPU, so every float op is a libgcc call).
 * ========================================================================= */

static void orb_build_sin(void)
{
    for (int i = 0; i < ORB_ANG; i++) {
        // Spelled out rather than M_PI: that is a POSIX extension, not guaranteed by <math.h>
        const float turn = 6.283185307f / (float)ORB_ANG;
        s_sin_tab[i] = (int16_t)lroundf(sinf((float)i * turn) * 32767.0f);
    }
}

#if ORB_DEFORM_GAIN > 0
// atan2 as a 0..255 outline bin, integer only. Fold into an octant, then look the ratio up:
// 65 table entries is 0.35 degrees of resolution, far finer than the 1.4 degrees a bin spans
static uint8_t orb_angle_bin(int32_t dx, int32_t dy)
{
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    if (adx == 0 && ady == 0) return 0;

    uint32_t a; // Angle within the first octant, 1/256 turn
    if (adx >= ady) {
        a = s_atan_lut[(uint32_t)((ady * 64) / adx)];
        if (dx >= 0) a = (dy >= 0) ? a : (256u - a);          // Octants 0 / 7
        else         a = (dy >= 0) ? (128u - a) : (128u + a); // Octants 3 / 4
    } else {
        a = s_atan_lut[(uint32_t)((adx * 64) / ady)];
        if (dy >= 0) a = (dx >= 0) ? (64u - a) : (64u + a);   // Octants 1 / 2
        else         a = (dx >= 0) ? (192u + a) : (192u - a); // Octants 6 / 5
    }

    return (uint8_t)(a & (ORB_ANG - 1));
}

static void orb_build_angle_field(void)
{
    for (int i = 0; i <= 64; i++) {
        s_atan_lut[i] = (uint8_t)lroundf(atanf((float)i / 64.0f) * (ORB_ANG / 6.283185307f));
    }

    for (int y = 0; y < ORB_H; y++) {
        int32_t dy = (((y << 8) + 128) - ORB_CY_Q8) >> 8;
        for (int x = 0; x < ORB_W; x++) {
            int32_t dx = (((x << 8) + 128) - ORB_CX_Q8) >> 8;
            s_ang[y * ORB_W + x] = orb_angle_bin(dx, dy);
        }
    }
}
#endif // ORB_DEFORM_GAIN > 0

static void orb_build_sqrt(void)
{
    for (int i = 0; i < ORB_SQRT_N; i++) {
        s_sqrt[i] = (uint16_t)lroundf(sqrtf((float)i) * 16.0f); // Q4
    }
}

// Shapes the auto-ranged level (already 0..255 relative to the loudest recent syllable) into a
// radius. The exponent lifts the quiet end so softer syllables still read as movement, without
// pegging everything at full the way a harder curve did once the input was normalised
static void orb_build_level_lut(void)
{
    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        s_lvl_lut[i] = (uint8_t)orb_clampi((int32_t)lroundf(powf(x, 0.50f) * 255.0f), 0, 255);
    }
    s_lvl_lut[0] = 0;
}

/* --- Colour ------------------------------------------------------------- */

typedef struct { float h, s, v; } orb_hsv_t; // h in turns [0,1)

static orb_hsv_t orb_rgb_to_hsv(lv_color_t c)
{
    float r = c.red / 255.0f, g = c.green / 255.0f, b = c.blue / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
    float d = mx - mn;
    orb_hsv_t o = { 0.0f, (mx <= 0.0f) ? 0.0f : d / mx, mx };

    if (d > 0.0f) {
        if      (mx == r) o.h = (g - b) / d / 6.0f;
        else if (mx == g) o.h = ((b - r) / d + 2.0f) / 6.0f;
        else              o.h = ((r - g) / d + 4.0f) / 6.0f;
        if (o.h < 0.0f) o.h += 1.0f;
    }

    return o;
}

static void orb_hsv_to_rgb(orb_hsv_t c, float *r, float *g, float *b)
{
    float h = c.h - floorf(c.h);
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = c.v * (1.0f - c.s);
    float q = c.v * (1.0f - f * c.s);
    float t = c.v * (1.0f - (1.0f - f) * c.s);

    switch ((int)i % 6) {
        case 0:  *r = c.v; *g = t;   *b = p;   break;
        case 1:  *r = q;   *g = c.v; *b = p;   break;
        case 2:  *r = p;   *g = c.v; *b = t;   break;
        case 3:  *r = p;   *g = q;   *b = c.v; break;
        case 4:  *r = t;   *g = p;   *b = c.v; break;
        default: *r = c.v; *g = p;   *b = q;   break;
    }
}

static inline uint16_t orb_pack(float r, float g, float b)
{
    int32_t ri = orb_clampi((int32_t)lroundf(r * 255.0f), 0, 255);
    int32_t gi = orb_clampi((int32_t)lroundf(g * 255.0f), 0, 255);
    int32_t bi = orb_clampi((int32_t)lroundf(b * 255.0f), 0, 255);

    return (uint16_t)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
}

static inline float orb_lum(float r, float g, float b)
{
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

/**
 * Build the radial palette from the user's theme.
 *
 * Four stops, walked outward: a core lifted hard toward the contrast end, the accent itself
 * hue-nudged one way, a rim hue-nudged the other and darkened, then a final stop that is almost
 * the page background so the orb's edge fuses into the page instead of being cut out of it.
 *
 * Two things here are load-bearing rather than decorative:
 *
 *  - The core lift. RGB565 only has 32/64/32 levels, and a ramp that starts at the raw accent
 *    traverses too few of them on a dark theme: measured over a 40 px radius, Dark Purple on
 *    Black resolves 15 distinct colours and bands every ~2.7 px. Lifting the core 65% toward
 *    white takes that to the full 41 of 41, and every other pair in the settings list with it.
 *    It is also simply what makes a sphere read as lit rather than flat.
 *
 *  - The hue source falls back to the background when the accent has no hue of its own. White
 *    on Dark Blue is the default theme and the common case; with no fallback its ramp would be
 *    white to blue with nothing in between, which is the flat two-colour look we were asked to
 *    avoid. Borrowing the background's hue gives it a pale periwinkle mid-band to travel through.
 */
static void orb_build_palette(void)
{
    lv_color_t acc = user_secondary_color;
    lv_color_t bgc = user_primary_color;

    float br = bgc.red / 255.0f, bg_ = bgc.green / 255.0f, bb = bgc.blue / 255.0f;
    float bg_lum = orb_lum(br, bg_, bb);

    orb_hsv_t a = orb_rgb_to_hsv(acc);
    orb_hsv_t b = orb_rgb_to_hsv(bgc);

    // A desaturated accent has no hue of its own to build a ramp from, so borrow the page's.
    // If the page has none either - white on black, black on white - then inventing one picks a
    // hue out of thin air (black nominally reads as hue 0, which came out orange), so stay
    // neutral instead and let the ramp run white through grey. Deliberate beats arbitrary
    float hue, sat;
    if (a.s >= 0.15f) {
        hue = a.h;  sat = a.s;
    } else if (b.s >= 0.15f) {
        hue = b.h;  sat = 0.55f;
    } else {
        hue = 0.0f; sat = 0.0f;
    }

    // On a dark page the orb glows toward white; on a light one it has to go the other way or
    // it disappears into the background
    float toward = (bg_lum < 0.5f) ? 1.0f : 0.0f;

    // The user can set primary and secondary to the same colour - both settings lists are
    // identical - which would leave the orb invisible. Force the body away from the page
    float body_v = a.v;
    if (fabsf(a.v - b.v) < 0.22f) {
        body_v = (toward > 0.5f) ? fminf(1.0f, b.v + 0.40f) : fmaxf(0.0f, b.v - 0.40f);
    }

    float cr, cg, cb; // Core: the accent, hue-true, lifted most of the way to the contrast end
    orb_hsv_to_rgb((orb_hsv_t){ hue, sat * 0.25f, fmaxf(body_v, 0.85f) }, &cr, &cg, &cb);
    cr += (toward - cr) * 0.65f;
    cg += (toward - cg) * 0.65f;
    cb += (toward - cb) * 0.65f;

    float mr, mg, mb; // Body: full accent, hue nudged forward
    orb_hsv_to_rgb((orb_hsv_t){ hue + 0.039f, fminf(1.0f, sat * 1.10f), body_v }, &mr, &mg, &mb);

    float rr, rg, rb; // Rim: hue nudged back and dimmed, already leaning toward the page
    orb_hsv_to_rgb((orb_hsv_t){ hue - 0.050f, fminf(1.0f, sat * 1.15f), body_v * 0.62f }, &rr, &rg, &rb);
    rr += (br - rr) * 0.35f;
    rg += (bg_ - rg) * 0.35f;
    rb += (bb - rb) * 0.35f;

    // Edge: nearly the page colour, so the antialiased boundary has almost nothing to hide
    float er = rr + (br - rr) * ORB_EDGE_BLEND;
    float eg = rg + (bg_ - rg) * ORB_EDGE_BLEND;
    float eb = rb + (bb - rb) * ORB_EDGE_BLEND;

    static const float STOP[4] = { 0.00f, ORB_STOP_BODY, ORB_STOP_RIM, 1.00f };
    const float R_[4] = { cr, mr, rr, er };
    const float G_[4] = { cg, mg, rg, eg };
    const float B_[4] = { cb, mb, rb, eb };

    for (int i = 0; i < ORB_RAMP_N; i++) {
        float t = (float)i / (float)(ORB_RAMP_N - 1);

        int seg = 0;
        while (seg < 2 && t > STOP[seg + 1]) seg++;

        float u = (t - STOP[seg]) / (STOP[seg + 1] - STOP[seg]);
        u = u * u * (3.0f - 2.0f * u); // Smoothstep, so the stops do not show as creases

        s_ramp[i] = orb_pack(R_[seg] + (R_[seg + 1] - R_[seg]) * u,
                             G_[seg] + (G_[seg + 1] - G_[seg]) * u,
                             B_[seg] + (B_[seg + 1] - B_[seg]) * u);
    }

    s_bg = orb_pack(br, bg_, bb);

    // The ring is the accent held well back toward the page: present as headroom, never
    // competing with the blob for attention
    s_ring_col = orb_pack(mr + (br - mr) * 0.55f,
                          mg + (bg_ - mg) * 0.55f,
                          mb + (bb - mb) * 0.55f);
}

/* ===========================================================================
 * Span primitives
 *
 * Everything on screen is horizontal runs with subpixel ends. The interior is a straight store
 * loop; only the one partial pixel at each end is blended, which is where the smooth edge comes
 * from and is cheap because it is two pixels per span rather than per pixel.
 * ========================================================================= */

// Flat-coloured run from x0 to x1 (Q8, exclusive), blended over what is already there
static void orb_span_solid(uint16_t *row, int32_t x0_q8, int32_t x1_q8, uint16_t col, int32_t opa)
{
    if (x1_q8 <= x0_q8) return;

    if (x0_q8 < 0) x0_q8 = 0;
    if (x1_q8 > (ORB_W << 8)) x1_q8 = ORB_W << 8;
    if (x1_q8 <= x0_q8) return;

    int32_t xi0 = (x0_q8 + 255) >> 8; // First fully covered pixel
    int32_t xi1 = x1_q8 >> 8;         // One past the last fully covered pixel

    if (xi0 > xi1) {
        // Span lands inside a single pixel: one blend weighted by how much of it is covered
        int32_t x = x0_q8 >> 8;
        if (x >= 0 && x < ORB_W) {
            int32_t t = ((x1_q8 - x0_q8) * opa) >> 8;
            row[x] = orb_mix565(row[x], col, t);
        }
        return;
    }

    if (xi0 > 0) { // Partial pixel on the left
        int32_t t = (((xi0 << 8) - x0_q8) * opa) >> 8;
        if (t > 0) row[xi0 - 1] = orb_mix565(row[xi0 - 1], col, t);
    }

    if (opa >= 255) {
        for (int32_t x = xi0; x < xi1; x++) row[x] = col;
    } else {
        for (int32_t x = xi0; x < xi1; x++) row[x] = orb_mix565(row[x], col, opa);
    }

    if (xi1 < ORB_W) { // Partial pixel on the right
        int32_t t = ((x1_q8 - (xi1 << 8)) * opa) >> 8;
        if (t > 0) row[xi1] = orb_mix565(row[xi1], col, t);
    }
}

// Blob run: same geometry, but each interior pixel takes its colour from the radial ramp.
// r^2 advances by 2*dx + 1 per step, so the inner loop has no multiply in it.
//
// Each pixel's ramp index is r / R(theta) with R taken at that pixel's own angle, so the last
// ramp entry lands exactly on the outline wherever the lobes have pushed it. Two cheaper
// approximations were tried and both show: scaling by the undeformed radius clamps every pixel in
// a protruding lobe to the final entry, painting a flat slab with a straight edge across it;
// interpolating the scale along the row fixes the slab but distorts the gradient, because the
// angle sweeps nonlinearly across a row. The angle field makes the exact version a byte load
static void orb_span_ramp(uint16_t *row, int32_t x0_q8, int32_t x1_q8, int32_t dy, int32_t y)
{
    if (x1_q8 <= x0_q8) return;

    if (x0_q8 < 0) x0_q8 = 0;
    if (x1_q8 > (ORB_W << 8)) x1_q8 = ORB_W << 8;
    if (x1_q8 <= x0_q8) return;

    int32_t xi0 = (x0_q8 + 255) >> 8;
    int32_t xi1 = x1_q8 >> 8;
    int32_t edge = s_ramp[ORB_RAMP_N - 1]; // What the rim fades to, used for both partial ends

    if (xi0 > xi1) {
        int32_t x = x0_q8 >> 8;
        if (x >= 0 && x < ORB_W) row[x] = orb_mix565(row[x], (uint16_t)edge, x1_q8 - x0_q8);
        return;
    }

    if (xi0 > 0) {
        int32_t t = (xi0 << 8) - x0_q8;
        if (t > 0) row[xi0 - 1] = orb_mix565(row[xi0 - 1], (uint16_t)edge, t);
    }

    // Seed the incremental square at the first interior pixel
    int32_t dx = ((xi0 << 8) + 128 - ORB_CX_Q8) >> 8; // Whole pixels from the centre
    int32_t dy2 = dy * dy;
    int32_t r2 = dx * dx + dy2;

#if ORB_DEFORM_GAIN > 0
    const uint8_t *ang = &s_ang[y * ORB_W];
#else
    LV_UNUSED(y); // A circle needs no angle: R is the same whichever way this pixel lies
    const int32_t inv_flat = s_out_inv_flat;
#endif

    for (int32_t x = xi0; x < xi1; x++) {
        int32_t r_q4 = orb_sqrt_q4(r2);
#if ORB_DEFORM_GAIN > 0
        int32_t idx = (int32_t)(((int64_t)r_q4 * s_out_inv[ang[x]]) >> 16); // r / R(theta)
#else
        int32_t idx = (int32_t)(((int64_t)r_q4 * inv_flat) >> 16);          // r / R
#endif
        row[x] = s_ramp[idx >= ORB_RAMP_N ? ORB_RAMP_N - 1 : (idx < 0 ? 0 : idx)];

        r2 += dx * 2 + 1; // (dx+1)^2 = dx^2 + 2dx + 1. Multiply, not a shift: dx is negative
                          // left of centre, and shifting a negative signed value is UB
        dx++;
    }

    if (xi1 < ORB_W) {
        int32_t t = x1_q8 - (xi1 << 8);
        if (t > 0) row[xi1] = orb_mix565(row[xi1], (uint16_t)edge, t);
    }
}

// One circle outline of radius r_q8 and half-thickness half_q8, as two runs per row (or one
// across the caps, where the inner circle does not reach)
static void orb_draw_annulus(int32_t r_q8, int32_t half_q8, uint16_t col, int32_t opa)
{
    int32_t r_out = r_q8 + half_q8;
    int32_t r_in  = r_q8 - half_q8;
    if (r_out <= 0) return;
    if (r_in < 0) r_in = 0;

    int32_t out_px = (r_out + 255) >> 8;

    for (int32_t dy = -out_px; dy <= out_px; dy++) {
        int32_t y = (ORB_CY_Q8 >> 8) + dy; // Same centre the dy maths below assumes
        if (y < 0 || y >= ORB_H) continue;

        // Row centre relative to the orb centre, in whole pixels plus the half-pixel offset
        int32_t dyc_q8 = ((y << 8) + 128) - ORB_CY_Q8;
        int32_t dy2 = (dyc_q8 * dyc_q8) >> 16; // Q8 * Q8 -> whole pixels squared

        int32_t ro2 = ((r_out * r_out) >> 16) - dy2;
        if (ro2 <= 0) continue;

        uint16_t *row = (uint16_t *)s_pixels + (size_t)y * s_stride_px;
        int32_t wo_q8 = orb_sqrt_q4(ro2) << 4; // Q4 -> Q8

        int32_t ri2 = ((r_in * r_in) >> 16) - dy2;
        if (ri2 <= 0) { // Cap row: the band closes into a single run
            orb_span_solid(row, ORB_CX_Q8 - wo_q8, ORB_CX_Q8 + wo_q8, col, opa);
            continue;
        }

        int32_t wi_q8 = orb_sqrt_q4(ri2) << 4;
        orb_span_solid(row, ORB_CX_Q8 - wo_q8, ORB_CX_Q8 - wi_q8, col, opa);
        orb_span_solid(row, ORB_CX_Q8 + wi_q8, ORB_CX_Q8 + wo_q8, col, opa);
    }
}

/* ===========================================================================
 * Frame
 * ========================================================================= */

// Reduce the outline to per-row span crossings. With the deformation off this is a circle and
// every row is crossed exactly twice; the crossings are still collected and paired even-odd so
// that turning ORB_DEFORM_GAIN up cannot silently fill over a dent the outline develops
static void orb_build_outline(int32_t r_q8, int32_t h1_q15, int32_t h2_q15, int *y0_out, int *y1_out)
{
#if ORB_DEFORM_GAIN == 0
    LV_UNUSED(h1_q15); // Deformation compiled out; the orb is a plain circle
    LV_UNUSED(h2_q15);
#endif

    for (int i = 0; i < ORB_ANG; i++) {
        int32_t r = r_q8;

#if ORB_DEFORM_GAIN > 0
        int32_t w = ((int32_t)s_sin_tab[(ORB_H1_ORDER * i + s_h1_phase) & (ORB_ANG - 1)] * h1_q15) >> 15;
        w += ((int32_t)s_sin_tab[(ORB_H2_ORDER * i - s_h2_phase) & (ORB_ANG - 1)] * h2_q15) >> 15;

        r += (int32_t)(((int64_t)r_q8 * w) >> 15);
#endif
#if ORB_DEFORM_GAIN > 0
        // Scale so the ramp spans 0..N-1 over 0..R(theta): the index is (r_q4 * inv) >> 16 and
        // r_q4 at the rim is r >> 4, hence N << 20 over r
        s_out_inv[i] = (int32_t)(((int64_t)ORB_RAMP_N << 20) / (r > 0 ? r : 1));
#endif

        s_px[i] = ORB_CX_Q8 + (int32_t)(((int64_t)r * s_sin_tab[(i + (ORB_ANG >> 2)) & (ORB_ANG - 1)]) >> 15);
        s_py[i] = ORB_CY_Q8 + (int32_t)(((int64_t)r * s_sin_tab[i]) >> 15);
    }
    s_px[ORB_ANG] = s_px[0];
    s_py[ORB_ANG] = s_py[0];

#if ORB_DEFORM_GAIN == 0
    s_out_inv_flat = (int32_t)(((int64_t)ORB_RAMP_N << 20) / (r_q8 > 0 ? r_q8 : 1));
#endif

    memset(s_row_n, 0, sizeof(s_row_n));

    int y0 = ORB_H, y1 = -1;

    for (int i = 0; i < ORB_ANG; i++) {
        int32_t ax = s_px[i],     ay = s_py[i];
        int32_t bx = s_px[i + 1], by = s_py[i + 1];
        if (ay == by) continue; // Horizontal edge crosses no row centre

        int32_t lo = ay < by ? ay : by;
        int32_t hi = ay < by ? by : ay;

        // Row centres sit at (y + 0.5); solve for x where the edge passes through each.
        // Half-open in y - lo counts, hi does not - so a vertex landing exactly on a row centre
        // is reported by one of its two edges instead of both. Counting it twice would flip the
        // even-odd parity back and leave an unfilled line straight across the blob
        int32_t r_first = (lo - 128 + 255) >> 8;
        int32_t r_last  = ((hi - 128 + 255) >> 8) - 1;
        if (r_first < 0) r_first = 0;
        if (r_last >= ORB_H) r_last = ORB_H - 1;

        for (int32_t y = r_first; y <= r_last; y++) {
            int32_t yc = (y << 8) + 128;
            int32_t x = ax + (int32_t)(((int64_t)(bx - ax) * (yc - ay)) / (by - ay));

            // Insert in order; dropping the overflow keeps a pathological frame ugly rather
            // than out of bounds, and ORB_XS_MAX is double what the shape can actually produce
            uint8_t n = s_row_n[y];
            if (n < ORB_XS_MAX) {
                int k = n;
                while (k > 0 && s_row_xs[y][k - 1] > x) {
                    s_row_xs[y][k] = s_row_xs[y][k - 1];
                    k--;
                }
                s_row_xs[y][k] = x;
                s_row_n[y] = (uint8_t)(n + 1);
            }

            if (y < y0) y0 = (int)y;
            if (y > y1) y1 = (int)y;
        }
    }

    *y0_out = y0;
    *y1_out = y1;
}

static void orb_render(void)
{
    // Ground. Two pixels per store: the buffer is 4-byte aligned and the stride is checked at init
    uint32_t bg2 = ((uint32_t)s_bg << 16) | s_bg;
    for (int y = 0; y < ORB_H; y++) {
        uint32_t *row = (uint32_t *)(s_pixels + (size_t)y * s_stride_px * 2);
        for (int x = 0; x < (ORB_W >> 1); x++) row[x] = bg2;
    }

    // Ripples first: they read as travelling out from under the blob, past the ring
    for (int i = 0; i < ORB_RIPPLE_MAX; i++) {
        if (s_ripple[i].life <= 0) continue;
        int32_t opa = (s_ripple[i].life * s_ripple[i].life) >> 8; // Fade out on a curve, not a line
        orb_draw_annulus(s_ripple[i].r_q8, 128, s_ring_col, opa >> 1);
    }

    orb_draw_annulus(ORB_RING_R << 8, ORB_RING_HALF_Q8, s_ring_col, 255);

    // Blob last, so it always sits on top of the ring it is growing toward
    int32_t lvl = s_env;
    int32_t r_q8 = (ORB_R_MIN << 8) + (((ORB_R_MAX - ORB_R_MIN) << 8) * lvl) / 255;

    // Breathing, so silence still looks alive - faded out as soon as there is anything to show
    int32_t breath = ((int32_t)s_sin_tab[s_breath_phase] * ORB_BREATH_AMP_Q8) >> 15;
    r_q8 += (breath * (255 - lvl)) / 255;

    // Shallow while quiet, deeper as the voice comes up: a circle at rest, liquid when talking
    int32_t scale = 90 + ((166 * lvl) / 255); // 0.35 -> 1.0 in 1/256ths
    int32_t h1 = (((ORB_H1_BASE_Q15 * ORB_DEFORM_GAIN) / 100) * scale) >> 8;
    int32_t h2 = (((ORB_H2_BASE_Q15 * ORB_DEFORM_GAIN) / 100) * scale) >> 8;

    int y0, y1;
    orb_build_outline(r_q8, h1, h2, &y0, &y1);

    if (y1 < y0) return; // Degenerate outline; ground and ring are already drawn

    for (int y = y0; y <= y1; y++) {
        int n = s_row_n[y] & ~1; // Crossings come in pairs; an odd one means a grazed vertex

        if (n < 2) continue;

        uint16_t *row = (uint16_t *)s_pixels + (size_t)y * s_stride_px;
        int32_t dy = (((y << 8) + 128) - ORB_CY_Q8) >> 8;

        for (int k = 0; k + 1 < n; k += 2) {
            orb_span_ramp(row, s_row_xs[y][k], s_row_xs[y][k + 1], dy, y);
        }
    }
}

/* ===========================================================================
 * Level
 * ========================================================================= */

static void orb_step_level(void)
{
    uint16_t peak = ai_voice_take_level_peak();

    // Adapt to the room: fall fast toward anything quieter, rise by at most one count per frame.
    // The asymmetry is what separates room tone from speech without needing to know which is
    // which - a few seconds of talking can only lift the floor a little, while steady hiss is
    // tracked out within about ten seconds. A proportional rise was tried first and was wrong
    // twice over: it climbed through total silence, and during a long sentence it subtracted
    // away the very signal the orb was meant to be showing
    if (peak < s_noise_floor) {
        s_noise_floor = (uint16_t)(peak + ((s_noise_floor - peak) >> 3));
    } else if (peak > s_noise_floor && s_noise_floor < ORB_NOISE_FLOOR_MAX) {
        s_noise_floor++;
    }

    // Only what stands clear of the floor counts as voice
    int32_t eff = (int32_t)peak - (int32_t)s_noise_floor;
    if (eff < 0) eff = 0;

    // Auto-range against the loudest syllable heard recently (see ORB_REF_MIN)
    if (eff > s_level_ref) {
        s_level_ref = eff; // Instant, so the loudest syllable always reaches full deflection
    } else {
        s_level_ref -= (s_level_ref - ORB_REF_MIN) >> ORB_REF_DECAY_SH;
    }
    if (s_level_ref < ORB_REF_MIN) s_level_ref = ORB_REF_MIN;

    int32_t tgt = s_lvl_lut[orb_clampi((eff * 255) / s_level_ref, 0, 255)];

    s_env_prev = s_env;
    if (tgt > s_env) {
        s_env += ((tgt - s_env) * ORB_ATTACK_NUM) >> 3; // Fast: 90% inside two frames
    } else {
        int32_t dec = s_env >> ORB_RELEASE_SH;          // Release: tau ~48 ms
        s_env -= (dec > 0) ? dec : 1;                   // Or it stalls a few counts above rest
        if (s_env < tgt) s_env = tgt;
    }
    s_env = orb_clampi(s_env, 0, 255);

    // A sharp rise is a syllable starting; let it throw a ring
    if (s_env - s_env_prev >= ORB_RIPPLE_TRIGGER) {
        for (int i = 0; i < ORB_RIPPLE_MAX; i++) {
            if (s_ripple[i].life <= 0) {
                s_ripple[i].r_q8 = (ORB_R_MIN << 8) + (((ORB_R_MAX - ORB_R_MIN) << 8) * s_env) / 255;
                s_ripple[i].r0_q8 = s_ripple[i].r_q8;
                s_ripple[i].life = 255;
                break;
            }
        }
    }

    // Fade by distance covered rather than by frames elapsed. A fixed decrement per frame looks
    // right for a ripple born near the centre and wrong for one born at a wide blob, which runs
    // out of room first and used to wink out at around an eighth opacity instead of fading
    for (int i = 0; i < ORB_RIPPLE_MAX; i++) {
        if (s_ripple[i].life <= 0) continue;

        s_ripple[i].r_q8 += ORB_RIPPLE_SPEED_Q8;

        int32_t span = (ORB_RIPPLE_MAX_R << 8) - s_ripple[i].r0_q8;
        int32_t gone = s_ripple[i].r_q8 - s_ripple[i].r0_q8;
        s_ripple[i].life = (span > 0) ? (255 - (255 * gone) / span) : 0;
        if (s_ripple[i].life < 0) s_ripple[i].life = 0;
    }

    s_h1_phase = (uint8_t)(s_h1_phase + ORB_H1_STEP);
    s_h2_phase = (uint8_t)(s_h2_phase + ORB_H2_STEP);
    s_breath_phase = (uint8_t)(s_breath_phase + ORB_BREATH_STEP);
}

// Our canvas was deleted by someone else - a screen clean, or a parent going away. Forget it, so
// deinit cannot delete it a second time and the timer stops dereferencing it. Belt and braces:
// the firmware only ever deletes it through deinit(), but this module is shared with the
// simulator, whose screen switch does clean the screen
static void orb_canvas_deleted_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_canvas = NULL;
    if (s_timer) lv_timer_pause(s_timer);
}

static void orb_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (!s_canvas || !s_pixels) return; // Init failed or already torn down

    orb_step_level();
    orb_render();
    lv_obj_invalidate(s_canvas); // One redraw for the frame; never a partial rect
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

bool lcd_voice_orb_init(lv_obj_t *parent)
{
    if (s_canvas) return true; // Already up; repeat calls are no-ops

    // Canvas taken from under us since the last init (see orb_canvas_deleted_cb) and never
    // deinit'd - release the orphaned framebuffer rather than leaking it
    if (s_pixels) {
        heap_caps_free(s_pixels);
        s_pixels = NULL;
    }

    s_buf_size = (size_t)ORB_W * ORB_H * 2; // RGB565 = 2 bytes/pixel
    // PSRAM is fine: st7789 CPU-copies into its own staging buffer before SPI, so this is
    // never a DMA source
    s_pixels = heap_caps_malloc(s_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pixels) {
        ESP_LOGE(TAG, "Failed to alloc PSRAM for voice orb canvas");
        return false; // Caller falls back to a plain widget
    }

    // Checked: a failure memzeroes the descriptor, leaving s_stride_px 0 and every row offset
    // collapsed onto the first row
    if (lv_draw_buf_init(&s_draw_buf, ORB_W, ORB_H, LV_COLOR_FORMAT_RGB565,
            LV_STRIDE_AUTO, s_pixels, s_buf_size) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to init voice orb draw buffer");
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    s_stride_px = s_draw_buf.header.stride / 2; // LVGL reports stride in bytes; we index in pixels

    // The background fill stores 32 bits at a time, so bail rather than go unaligned
    if ((((uintptr_t)s_pixels | (uintptr_t)s_draw_buf.header.stride) & 3u) != 0) {
        ESP_LOGE(TAG, "Voice orb buffer/stride not 4-byte aligned");
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }

    s_canvas = lv_canvas_create(parent);
    if (!s_canvas) {
        ESP_LOGE(TAG, "Failed to create voice orb canvas");
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    lv_obj_add_event_cb(s_canvas, orb_canvas_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_canvas_set_draw_buf(s_canvas, &s_draw_buf); // Canvas renders straight out of s_pixels
    lv_obj_set_size(s_canvas, ORB_W, ORB_H);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE); // It is a drawing, not a widget

    orb_build_sin();
    orb_build_sqrt();
#if ORB_DEFORM_GAIN > 0
    orb_build_angle_field();
#endif
    orb_build_level_lut();
    orb_build_palette(); // Reads the theme, so this has to run per page visit, not once per boot

    s_env = 0;
    s_env_prev = 0;
    s_noise_floor = 0;
    s_level_ref = ORB_REF_MIN;
    s_h1_phase = 0;
    s_h2_phase = 0;
    s_breath_phase = 0;
    memset(s_ripple, 0, sizeof(s_ripple));

    orb_render(); // Paint a resting orb now, so the first frame shown is never garbage

    s_timer = lv_timer_create(orb_timer_cb, ORB_FRAME_PERIOD, NULL);
    if (!s_timer) {
        ESP_LOGE(TAG, "Failed to create voice orb timer");
        lv_obj_delete(s_canvas); // Unwind so a later retry starts clean
        s_canvas = NULL;
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    lv_timer_pause(s_timer); // Created idle: the page starts it when recording begins
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);

    return true;
}

void lcd_voice_orb_start(void)
{
    if (!s_canvas) return; // Init failed: stay silent rather than crash

    // Start from rest every time, or the orb re-appears holding the last word's level
    s_env = 0;
    s_env_prev = 0;
    s_level_ref = ORB_REF_MIN; // Re-range per recording rather than inheriting the last one
    memset(s_ripple, 0, sizeof(s_ripple));
    (void)ai_voice_take_level_peak(); // Drop anything the mic left queued before we were looking

    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) lv_timer_resume(s_timer);
}

// Pausing matters as much as hiding: a hidden canvas still burns a full frame
void lcd_voice_orb_stop(void)
{
    if (s_timer) lv_timer_pause(s_timer);
    if (s_canvas) lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
}

void lcd_voice_orb_deinit(void)
{
    // Timer first: it dereferences the canvas, and the canvas points into s_pixels. Freeing in
    // any other order leaves a live callback holding a dangling object for one frame
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_canvas) {
        lv_obj_delete(s_canvas);
        s_canvas = NULL;
    }
    if (s_pixels) {
        heap_caps_free(s_pixels);
        s_pixels = NULL;
    }
    memset(&s_draw_buf, 0, sizeof(s_draw_buf));
    s_buf_size = 0;
    s_stride_px = 0;
}

bool lcd_voice_orb_is_available(void)
{
    return s_canvas != NULL;
}
