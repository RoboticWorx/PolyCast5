#include "lcd_dice.h"

#include <stdint.h>
#include <string.h>

#include "lvgl.h"

#ifdef POLYCAST5_SIM
#include "sim_compat.h"
#else
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lcd_utils.h" // user_primary_color, user_secondary_color
#endif

static const char *TAG = "LCD_DICE";

#define DICE_MAX 255

#define ONE_Q14 16384  // Rotation matrices and unit vectors
#define TURN_Q16 65536 // Angles, in turns
#define UV_ONE 65536   // Face coordinates: u and v run -1..1 across a face, Q16

#define DICE_FILL_PCT 86      // Die edge as a share of its grid cell; the rest is the gap
#define DICE_CORNER_UV 14418  // Body corner radius, 0.22 of the half edge
#define DICE_RIM_PCT 5        // Rim between the body outline and the face, % of the die edge
#define DICE_RIM_MIN_Q8 128   // Rim limits in pixels, Q8
#define DICE_RIM_MAX_Q8 (4 << 8)
#define DICE_ZMIN_Q14 300     // Faces within ~1 degree of edge-on are sub-pixel slivers: skipped

#define DICE_PIP_POS 32768   // Pip grid offset from the face centre, 0.5
#define DICE_PIP_R_Q12 778   // Pip radius 0.19
#define DICE_PIP1_R_Q12 1065 // The lone pip of a 1 is drawn larger, 0.26
#define DICE_NUM_MIN_PX 14   // Numerals on dice smaller than this are unreadable: faces stay plain

#define DICE_SHADOW_PCT 4 // Shadow offset at rest, % of the die edge

#define DICE_ROLL_MS 1000        // Tumble time per die...
#define DICE_ROLL_JITTER_MS 300  // ...plus up to this much, so dice never land in lockstep
#define DICE_STAGGER_MS 70       // Start gap between consecutive dice...
#define DICE_STAGGER_SPAN_MS 450 // ...shrunk so the last die starts within this
#define DICE_SPIN2_END_Q15 21299 // The secondary spin settles by 0.65 of the roll

// Light from the upper left and in front, Q14 unit vector. The shadow falls the other way
#define DICE_LIGHT_X -4915
#define DICE_LIGHT_Y -6554
#define DICE_LIGHT_Z 14189
#define DICE_SHADE_GAIN 200 // How fast faces darken as they turn from the light

// Edge flags of a face: the neighbour across that edge is also drawn
#define DICE_EDGE_UP 1
#define DICE_EDGE_UN 2
#define DICE_EDGE_VP 4
#define DICE_EDGE_VN 8

// REST: drawn and still. WAIT: rolled, waiting out its stagger delay. ROLL: tumbling
enum { DICE_REST, DICE_WAIT, DICE_ROLL };

// One die. Layout fields are set by dice_layout(), roll fields by lcd_dice_roll_start()
typedef struct {
    int16_t x0, y0, x1, y1; // Grid cell, canvas pixels, half-open. Drawing never leaves it
    int32_t cx_q8, cy_q8;   // Die centre, Q8
    int32_t half_q8;        // Half the die edge at rest, Q8
    int32_t shadow_q8;      // Shadow offset at rest, Q8
    int32_t inset_uv;       // Rim width in face units, Q16
    int16_t k1[3], k2[3];   // Spin axes, Q14 unit vectors
    int32_t a1, a2;         // Spin left to unwind at the start of the roll, turns Q16
    uint16_t delay_ms;      // Start offset within the roll
    uint16_t dur_ms;        // Tumble time
    uint8_t val[6];         // Number on each body face, 0 = blank. val[0] (+Z) is the result
    uint8_t rot[6];         // Quarter turns applied to each face's content
    uint8_t state;          // DICE_REST / DICE_WAIT / DICE_ROLL
} dice_die_t;

// A cube face in body space: its outward normal n and the directions its u and v run
typedef struct {
    uint8_t n, a, b; // Body axes (0 x, 1 y, 2 z) of the face normal and of its u and v
    int8_t ns, as, bs;
} dice_face_def_t;

// a x b = n on every face, so content turned to the front is never mirrored
static const dice_face_def_t s_face_def[6] = {
    {2, 0, 1,  1,  1,  1}, // +Z: faces the viewer at rest
    {2, 0, 1, -1, -1,  1}, // -Z
    {0, 2, 1,  1, -1,  1}, // +X
    {0, 2, 1, -1,  1,  1}, // -X
    {1, 0, 2,  1,  1, -1}, // +Y
    {1, 0, 2, -1,  1,  1}, // -Y
};

// Pips on a 3 x 3 grid, bit = row * 3 + column, top row first. Indexed by face value 1..6
static const uint16_t s_pip_mask[7] = {0, 0x010, 0x044, 0x054, 0x145, 0x155, 0x16D};

// sin over a quarter turn in 64 steps, Q14
static const int16_t s_sin_q14[65] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897,
    6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702,
    11003, 11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623, 13842, 14053,
    14256, 14449, 14635, 14811, 14978, 15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986,
    16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379, 16384,
};

// Rotations that carry the front face away: a roll unwinds one of these to land face-on.
// About x and y the die rolls, about a body diagonal or an edge axis it tumbles
#define DICE_D3 9459  // 1 / sqrt(3), Q14
#define DICE_D2 11585 // 1 / sqrt(2), Q14

typedef struct {
    int16_t k[3];  // Axis, Q14
    uint16_t turn; // Angle, turns Q16
} dice_spin_t;

// The 20 cube symmetries that move the front face (the 4 about z keep it, so are left out)
static const dice_spin_t s_spin_start[] = {
    {{ONE_Q14, 0, 0}, 16384}, {{ONE_Q14, 0, 0}, 32768}, {{ONE_Q14, 0, 0}, 49152},
    {{0, ONE_Q14, 0}, 16384}, {{0, ONE_Q14, 0}, 32768}, {{0, ONE_Q14, 0}, 49152},
    {{DICE_D3, DICE_D3, DICE_D3}, 21845}, {{DICE_D3, DICE_D3, DICE_D3}, 43691},
    {{DICE_D3, DICE_D3, -DICE_D3}, 21845}, {{DICE_D3, DICE_D3, -DICE_D3}, 43691},
    {{DICE_D3, -DICE_D3, DICE_D3}, 21845}, {{DICE_D3, -DICE_D3, DICE_D3}, 43691},
    {{-DICE_D3, DICE_D3, DICE_D3}, 21845}, {{-DICE_D3, DICE_D3, DICE_D3}, 43691},
    {{DICE_D2, DICE_D2, 0}, 32768}, {{DICE_D2, -DICE_D2, 0}, 32768},
    {{DICE_D2, 0, DICE_D2}, 32768}, {{DICE_D2, 0, -DICE_D2}, 32768},
    {{0, DICE_D2, DICE_D2}, 32768}, {{0, DICE_D2, -DICE_D2}, 32768},
};
#define DICE_SPIN_START_N (sizeof(s_spin_start) / sizeof(s_spin_start[0]))

// Body face each s_spin_start entry brings to the front (four entries per side face). Built at init
static uint8_t s_spin_face[DICE_SPIN_START_N];

/* ===========================================================================
 * Numerals
 *
 * A monoline digit set built from round-capped strokes, so numerals stay crisp at any size and
 * angle. Glyph space is Q12: 1.0 is the numeral height, the box is 0.56 wide, y points down.
 * ========================================================================= */

enum { DICE_SEG, DICE_RING, DICE_ARC };

typedef struct {
    uint8_t kind;
    int16_t ax, ay, bx, by; // SEG: end points. RING: centre line, a == b for a circle. ARC: centre a
    int16_t r;              // RING / ARC radius
    int32_t inv_len2;       // 2^36 / |b - a|^2 (Q24), 0 when a == b
    int16_t sx, sy, ex, ey; // ARC: drawn clockwise on screen from s to e; the gap must be < 180 deg
} dice_stroke_t;

// Every digit's strokes back to back; s_digit_first[d] .. s_digit_first[d + 1] are digit d's
static const dice_stroke_t s_strokes[] = {
    {DICE_RING, 1147, 1229, 1147, 2867,  840, 25612,    0,    0,    0,    0}, // 0
    {DICE_SEG,  1352,  307, 1352, 3789,    0,  5667,    0,    0,    0,    0}, // 1
    {DICE_SEG,  1352,  307,  492,  819,    0, 68599,    0,    0,    0,    0},
    {DICE_ARC,  1147, 1188,    0,    0,  881,     0,  296, 1416, 1868, 1693}, // 2
    {DICE_SEG,  1868, 1693,  307, 3789,    0, 10061,    0,    0,    0,    0},
    {DICE_SEG,   307, 3789, 2028, 3789,    0, 23201,    0,    0,    0,    0},
    {DICE_ARC,  1147, 1126,    0,    0,  819,     0,  404,  780, 1147, 1946}, // 3
    {DICE_ARC,  1147, 2806,    0,    0,  983,     0, 1147, 1823,  256, 3221},
    {DICE_SEG,  1597,  307,  307, 2703,    0,  9280,    0,    0,    0,    0}, // 4
    {DICE_SEG,   307, 2703, 2068, 2703,    0, 22159,    0,    0,    0,    0},
    {DICE_SEG,  1597,  307, 1597, 3789,    0,  5667,    0,    0,    0,    0},
    {DICE_SEG,  1987,  307,  471,  307,    0, 29900,    0,    0,    0,    0}, // 5
    {DICE_SEG,   471,  307,  389, 1966,    0, 24907,    0,    0,    0,    0},
    {DICE_ARC,  1147, 2724,    0,    0,  983,     0,  296, 2232,  342, 3288},
    {DICE_RING, 1147, 2806, 1147, 2806,  881,     0,    0,    0,    0,    0}, // 6
    {DICE_SEG,   328, 2703, 1597,  307,    0,  9348,    0,    0,    0,    0},
    {DICE_SEG,   307,  307, 1987,  307,    0, 24347,    0,    0,    0,    0}, // 7
    {DICE_SEG,  1987,  307,  696, 3789,    0,  4982,    0,    0,    0,    0},
    {DICE_RING, 1147, 1106, 1147, 1106,  758,     0,    0,    0,    0,    0}, // 8
    {DICE_RING, 1147, 2826, 1147, 2826,  922,     0,    0,    0,    0,    0},
    {DICE_RING, 1147, 1290, 1147, 1290,  881,     0,    0,    0,    0,    0}, // 9
    {DICE_SEG,  1966, 1393,  696, 3789,    0,  9344,    0,    0,    0,    0},
};
#define DICE_STROKE_N (sizeof(s_strokes) / sizeof(s_strokes[0]))

static const uint8_t s_digit_first[11] = {0, 1, 3, 6, 8, 11, 14, 16, 18, 20, 22};

#define DICE_GLYPH_HW 307   // Stroke half-width, 0.075
#define DICE_GLYPH_W 2294   // Digit box width, 0.56
#define DICE_GLYPH_ADV 2785 // Digit advance, 0.68

// Numeral height on the face (uv, Q16) for 1, 2 and 3 digits
static const int32_t s_num_h[3] = {65536, 56361, 41943};

// Derived once per init: per-stroke bounds and the radii the coverage ramps need
typedef struct {
    int16_t x0, y0, x1, y1; // Stroke bounds in glyph space, outer edge included
    int32_t ro2, ri2;     // Outer and inner edge radii squared (Q24); SEG and caps use ro2
    int32_t inv_o, inv_i; // 2^24 / 2r for the outer and inner edge
} dice_stroke_k_t;

static dice_stroke_k_t s_stroke_k[DICE_STROKE_N];
static int32_t s_cap2, s_inv_cap; // Round caps: radius DICE_GLYPH_HW

/* ===========================================================================
 * State
 * ========================================================================= */

static lv_obj_t *s_canvas = NULL;   // NULL = not initialized (or deleted from outside)
static lv_draw_buf_t s_draw_buf;    // Wraps s_pixels for the canvas
static uint16_t *s_pixels = NULL;   // RGB565 framebuffer, PSRAM
static size_t s_buf_size = 0;
static int32_t s_w, s_h;     // Arena size
static int32_t s_stride_px;  // Row stride in pixels
static dice_die_t *s_dice = NULL;   // DICE_MAX entries, PSRAM; the first s_count are live
static uint8_t s_count = 0;
static uint8_t s_sides = 6;
static uint32_t s_roll_t0;          // now_ms passed to lcd_dice_roll_start()
static bool s_rolling = false;      // A roll is in progress: lcd_dice_roll_step() has work
static uint32_t s_rng = 1;          // Tumble variety only; results come from the caller

static uint16_t s_col_bg;     // Page background
static uint16_t s_col_face;   // Lit face
static uint16_t s_col_dark;   // What a face fades toward as it turns from the light
static uint16_t s_col_rim;    // Body outline between faces
static uint16_t s_col_ink;    // Pips and numerals
static uint16_t s_col_shadow;

/* ===========================================================================
 * Fixed-point helpers
 * ========================================================================= */

// Blend b over a. t is 0..255 (0 = all a)
static inline uint16_t dice_mix565(uint16_t a, uint16_t b, int32_t t)
{
    int32_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int32_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;

    ar += ((br - ar) * t) >> 8;
    ag += ((bg - ag) * t) >> 8;
    ab += ((bb - ab) * t) >> 8;

    return (uint16_t)((ar << 11) | (ag << 5) | ab);
}

static inline int32_t dice_clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t dice_absi(int32_t v)
{
    return v < 0 ? -v : v;
}

// |(x, y)| to within 6%: only ever sizes an anti-aliasing ramp
static inline int32_t dice_hypot(int32_t x, int32_t y)
{
    x = dice_absi(x);
    y = dice_absi(y);
    int32_t hi = x > y ? x : y, lo = x > y ? y : x;
    return hi - (hi >> 4) + (lo >> 1) - (lo >> 5);
}

// sin of an angle in turns (Q16), Q14 out. cos is sin a quarter turn on
static int32_t dice_sin_q14(int32_t turn_q16)
{
    uint32_t a = (uint32_t)turn_q16 & 0xFFFF; // Whole turns drop out; negative angles wrap
    uint32_t quad = a >> 14;
    uint32_t f = a & 0x3FFF;
    if (quad & 1) f = 0x4000 - f; // Falling quadrants mirror the rising ones

    // Table step i plus a linear blend toward step i + 1
    uint32_t i = f >> 8, w = f & 0xFF;
    int32_t s = (i >= 64) ? s_sin_q14[64]
            : s_sin_q14[i] + (((s_sin_q14[i + 1] - s_sin_q14[i]) * (int32_t)w) >> 8);
    return (quad & 2) ? -s : s; // Second half-turn is negative
}

// xorshift32, seeded per roll
static uint32_t dice_rand(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

// floor(sqrt(v)), bit by bit
static uint32_t dice_isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

// floor(a / b) for b > 0
static inline int32_t dice_floor_div(int32_t a, int32_t b)
{
    int32_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

// Rotation by turn_q16 about the unit axis k (Rodrigues), Q14
static void dice_rot(int32_t m[3][3], const int16_t k[3], int32_t turn_q16)
{
    int32_t s = dice_sin_q14(turn_q16);
    int32_t c = dice_sin_q14(turn_q16 + TURN_Q16 / 4);
    int32_t t = ONE_Q14 - c;
    int32_t x = k[0], y = k[1], z = k[2];
    int32_t tx = (t * x) >> 14, ty = (t * y) >> 14, tz = (t * z) >> 14;
    int32_t sx = (s * x) >> 14, sy = (s * y) >> 14, sz = (s * z) >> 14;

    // R = c I + s [k]x + (1 - c) k k^T
    m[0][0] = c + ((tx * x) >> 14);
    m[0][1] = ((tx * y) >> 14) - sz;
    m[0][2] = ((tx * z) >> 14) + sy;
    m[1][0] = ((tx * y) >> 14) + sz;
    m[1][1] = c + ((ty * y) >> 14);
    m[1][2] = ((ty * z) >> 14) - sx;
    m[2][0] = ((tx * z) >> 14) - sy;
    m[2][1] = ((ty * z) >> 14) + sx;
    m[2][2] = c + ((tz * z) >> 14);
}

// out = a * b, Q14
static void dice_mul(int32_t out[3][3], int32_t a[3][3], int32_t b[3][3])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out[i][j] = (a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]) >> 14;
        }
    }
}

// Index into s_face_def of the face whose normal is body axis `axis` with this sign
static inline int dice_face_of(int axis, int sign)
{
    if (axis == 2) return sign > 0 ? 0 : 1;
    if (axis == 0) return sign > 0 ? 2 : 3;
    return sign > 0 ? 4 : 5;
}

// The body face that pose m turns toward the viewer
static int dice_front_face(int32_t m[3][3])
{
    for (int f = 0; f < 6; f++) {
        const dice_face_def_t *fd = &s_face_def[f];
        if (fd->ns * m[2][fd->n] > ONE_Q14 / 2) return f;
    }
    return 0;
}

// Fill s_spin_face[] from the spin table
static void dice_build_spins(void)
{
    int32_t m[3][3];
    for (size_t i = 0; i < DICE_SPIN_START_N; i++) {
        dice_rot(m, s_spin_start[i].k, s_spin_start[i].turn);
        s_spin_face[i] = (uint8_t)dice_front_face(m);
    }
}

/* ===========================================================================
 * Coverage
 * ========================================================================= */

// Anti-aliasing ramp one pixel wide, centred on the edge. dist_q12 is how far inside the shape
// the pixel centre lies (uv or glyph units, Q12); kg is 255 / pixel footprint in those units (Q16).
// Clamped after scaling, so a pixel wider than the shape still falls to 0 outside it
static inline int32_t dice_ramp(int32_t dist_q12, int32_t kg)
{
    int64_t c = 128 + (((int64_t)dist_q12 * kg) >> 16);
    return c < 0 ? 0 : (c > 255 ? 255 : (int32_t)c);
}

// Clamp to +-32767, so two squares still sum inside int32. Anything that far out has no coverage
static inline int32_t dice_sat15(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32767 ? -32767 : v);
}

// Round edge of radius r: (r^2 - d^2) / 2r tracks r - d within a pixel of the rim, which is all
// the ramp reads, and needs no square root. r2_d2 is Q24, inv2r = 2^24 / 2r (r in Q12)
static inline int32_t dice_round(int32_t r2_d2, int32_t inv2r, int32_t kg)
{
    return dice_ramp((int32_t)(((int64_t)r2_d2 * inv2r) >> 24), kg);
}

// Coverage of one digit at glyph point (gx, gy): the strongest of its strokes.
// pad: how far past a stroke its anti-aliasing still reaches, in glyph units
static int32_t dice_glyph_cov(uint8_t digit, int32_t gx, int32_t gy, int32_t kg, int32_t pad)
{
    int32_t best = 0;

    for (int i = s_digit_first[digit]; i < s_digit_first[digit + 1]; i++) {
        const dice_stroke_t *s = &s_strokes[i];
        const dice_stroke_k_t *k = &s_stroke_k[i];
        // Cheap reject: outside the stroke's bounds plus its AA reach
        if (gx < k->x0 - pad || gx > k->x1 + pad || gy < k->y0 - pad || gy > k->y1 + pad) {
            continue;
        }

        int32_t px = gx - s->ax, py = gy - s->ay; // Relative to a (the centre for ARC)
        int32_t cov;

        if (s->kind == DICE_ARC) {
            int32_t ex = s->ex - s->ax, ey = s->ey - s->ay;
            int32_t sx = s->sx - s->ax, sy = s->sy - s->ay;
            if (ex * py - ey * px > 0 && px * sy - py * sx > 0) {
                // In the gap between the ends: distance to the nearer round cap
                int32_t dsx = gx - s->sx, dsy = gy - s->sy, dex = gx - s->ex, dey = gy - s->ey;
                int32_t ds2 = dsx * dsx + dsy * dsy, de2 = dex * dex + dey * dey;
                cov = dice_round(s_cap2 - (ds2 < de2 ? ds2 : de2), s_inv_cap, kg);
            } else {
                // On the arc's sweep: inside the outer edge and outside the inner one
                int32_t d2 = px * px + py * py;
                int32_t co = dice_round(k->ro2 - d2, k->inv_o, kg);
                int32_t ci = dice_round(d2 - k->ri2, k->inv_i, kg);
                cov = co < ci ? co : ci;
            }
        } else {
            if (s->inv_len2) {
                // Nearest point on a..b
                int32_t dx = s->bx - s->ax, dy = s->by - s->ay;
                int32_t t = (int32_t)(((int64_t)(px * dx + py * dy) * s->inv_len2) >> 24);
                t = dice_clampi(t, 0, 4096);
                px -= (dx * t) >> 12;
                py -= (dy * t) >> 12;
            }
            int32_t d2 = px * px + py * py;
            if (s->kind == DICE_SEG) {
                // Round-capped line: within the half-width of a..b
                cov = dice_round(k->ro2 - d2, k->inv_o, kg);
            } else {
                // Ring around a..b (a circle, or the stadium of the 0)
                int32_t co = dice_round(k->ro2 - d2, k->inv_o, kg);
                int32_t ci = dice_round(d2 - k->ri2, k->inv_i, kg);
                cov = co < ci ? co : ci;
            }
        }
        if (cov > best) best = cov;
    }
    return best;
}

// Fill s_stroke_k[] and the cap constants from the stroke table
static void dice_build_strokes(void)
{
    s_cap2 = DICE_GLYPH_HW * DICE_GLYPH_HW;
    s_inv_cap = (1 << 24) / (2 * DICE_GLYPH_HW);

    for (size_t i = 0; i < DICE_STROKE_N; i++) {
        const dice_stroke_t *s = &s_strokes[i];
        dice_stroke_k_t *k = &s_stroke_k[i];
        int32_t ro = (s->kind == DICE_SEG) ? DICE_GLYPH_HW : s->r + DICE_GLYPH_HW;
        int32_t ri = s->r - DICE_GLYPH_HW;
        int32_t reach = ro;
        int32_t bx = (s->kind == DICE_ARC) ? s->ax : s->bx; // An arc is bounded by its circle
        int32_t by = (s->kind == DICE_ARC) ? s->ay : s->by;

        k->x0 = (int16_t)((s->ax < bx ? s->ax : bx) - reach);
        k->x1 = (int16_t)((s->ax > bx ? s->ax : bx) + reach);
        k->y0 = (int16_t)((s->ay < by ? s->ay : by) - reach);
        k->y1 = (int16_t)((s->ay > by ? s->ay : by) + reach);
        k->ro2 = ro * ro;
        k->inv_o = (1 << 24) / (2 * ro);
        k->ri2 = ri > 0 ? ri * ri : 0;
        k->inv_i = ri > 0 ? (1 << 24) / (2 * ri) : 0;
    }
}

/* ===========================================================================
 * Face raster
 *
 * A projected face is the parallelogram c +- U +- V. Inverting that 2x2 map gives (u, v) as a
 * linear function of the pixel, so each row is walked with two adds per pixel.
 * ========================================================================= */

typedef struct {
    int32_t x0, y0, x1, y1;         // Pixel box, half-open, clipped to the cell
    int32_t u, v;                   // At the centre of pixel (x0, y0), Q16
    int32_t dudx, dudy, dvdx, dvdy; // Per pixel, Q16
    int32_t lim_u, lim_v;           // |u|, |v| beyond this never touch a pixel
    int32_t seam_u, seam_v;         // Half a pixel: how far a shared edge overdraws
    int32_t kgu, kgv, kgi;          // 255 / pixel footprint along u, along v, and the larger (Q16)
    int32_t g_q16;                  // The larger footprint, uv per pixel
    uint8_t shared;                 // DICE_EDGE_* bits
} dice_raster_t;

// Set up the inverse map for the face spanning c +- U +- V (all Q8 pixels)
static bool dice_raster_setup(dice_raster_t *r, const dice_die_t *d, int32_t cx, int32_t cy,
        int32_t ux, int32_t uy, int32_t vx, int32_t vy, int32_t pad_px)
{
    int64_t det = (int64_t)ux * vy - (int64_t)uy * vx; // Q16 px^2
    if (det == 0) return false;
    int64_t inv = ((int64_t)1 << 46) / det; // One 64-bit divide per face; the rest multiply

    // Inverse of [U V]: how far u and v move per pixel step in x and in y
    int64_t dudx = (vy * inv) >> 22, dudy = -((vx * inv) >> 22);
    int64_t dvdx = -((uy * inv) >> 22), dvdy = (ux * inv) >> 22;
    const int64_t gmax = 1 << 22; // 1/64 px across: nothing to draw
    if (dudx > gmax || dudx < -gmax || dudy > gmax || dudy < -gmax ||
            dvdx > gmax || dvdx < -gmax || dvdy > gmax || dvdy < -gmax) {
        return false;
    }
    r->dudx = (int32_t)dudx;
    r->dudy = (int32_t)dudy;
    r->dvdx = (int32_t)dvdx;
    r->dvdy = (int32_t)dvdy;

    // Pixel footprint in face units, for anti-aliasing ramps one pixel wide
    int32_t gu = dice_hypot(r->dudx, r->dudy), gv = dice_hypot(r->dvdx, r->dvdy);
    if (gu < 16) gu = 16;
    if (gv < 16) gv = 16;
    r->g_q16 = gu > gv ? gu : gv;
    r->kgu = (255 << 16) / ((gu >> 4) > 0 ? (gu >> 4) : 1);
    r->kgv = (255 << 16) / ((gv >> 4) > 0 ? (gv >> 4) : 1);
    r->kgi = (255 << 16) / ((r->g_q16 >> 4) > 0 ? (r->g_q16 >> 4) : 1);
    r->seam_u = gu >> 1;
    r->seam_v = gv >> 1;
    r->lim_u = UV_ONE + gu * (pad_px + 1);
    r->lim_v = UV_ONE + gv * (pad_px + 1);

    // Pixel box of the parallelogram plus its AA reach, clipped to the die's cell
    int32_t ex = dice_absi(ux) + dice_absi(vx) + ((pad_px + 2) << 8);
    int32_t ey = dice_absi(uy) + dice_absi(vy) + ((pad_px + 2) << 8);
    r->x0 = dice_clampi((cx - ex) >> 8, d->x0, d->x1);
    r->x1 = dice_clampi(((cx + ex) >> 8) + 1, d->x0, d->x1);
    r->y0 = dice_clampi((cy - ey) >> 8, d->y0, d->y1);
    r->y1 = dice_clampi(((cy + ey) >> 8) + 1, d->y0, d->y1);
    if (r->x0 >= r->x1 || r->y0 >= r->y1) return false;

    // (u, v) at the centre of the box's first pixel; the loops step from here
    int32_t dx = (r->x0 << 8) + 128 - cx, dy = (r->y0 << 8) + 128 - cy;
    r->u = (int32_t)((((int64_t)vy * dx - (int64_t)vx * dy) * inv) >> 30);
    r->v = (int32_t)((((int64_t)ux * dy - (int64_t)uy * dx) * inv) >> 30);
    return true;
}

// Narrow [*lo, *hi] to the pixels where |w + dw * i| <= lim
static inline void dice_span(int32_t w, int32_t dw, int32_t lim, int32_t *lo, int32_t *hi)
{
    if (dw == 0) {
        if (w > lim || w < -lim) *hi = *lo - 1; // Constant along the row and out: empty span
        return;
    }
    int32_t a = -lim - w, b = lim - w; // Need a <= dw * i <= b
    if (dw < 0) {
        int32_t t = a;
        a = -b;
        b = -t;
        dw = -dw;
    }
    int32_t i0 = dice_floor_div(a, dw), i1 = dice_floor_div(b, dw) + 1; // One pixel of slack
    if (i0 > *lo) *lo = i0;
    if (i1 < *hi) *hi = i1;
}

// Outline coverage shared by the shadow and body passes: shared edges overdraw by half a pixel
// so neighbouring faces leave no seam, silhouette edges and outer corners are anti-aliased
static inline int32_t dice_outline_cov(const dice_raster_t *r, int32_t u, int32_t v,
        int32_t au, int32_t av, int32_t cc, int32_t rb2, int32_t inv_rb, int32_t kgu,
        int32_t kgv, int32_t kgi)
{
    int32_t cov = 255;
    uint8_t eu = (u < 0) ? DICE_EDGE_UN : DICE_EDGE_UP; // The u edge this pixel is nearer
    uint8_t ev = (v < 0) ? DICE_EDGE_VN : DICE_EDGE_VP;

    if (r->shared & eu) {
        if (au > UV_ONE + r->seam_u) return 0;
    } else {
        cov = dice_ramp((UV_ONE - au) >> 4, kgu);
    }
    if (r->shared & ev) {
        if (av > UV_ONE + r->seam_v) return 0;
    } else {
        int32_t c = dice_ramp((UV_ONE - av) >> 4, kgv);
        if (c < cov) cov = c;
    }
    // Round only outline corners, where neither edge is shared: rounding the end of a shared edge
    // would notch the silhouette
    if (cov && au > cc && av > cc && !(r->shared & (eu | ev))) {
        int32_t du = dice_sat15((au - cc) >> 4), dv = dice_sat15((av - cc) >> 4);
        int32_t c = dice_round(rb2 - (du * du + dv * dv), inv_rb, kgi);
        if (c < cov) cov = c;
    }
    return cov;
}

// ox, oy: the shadow's offset from the face (Q8 pixels). Pixels the face itself will paint over
// are skipped, which is most of the shadow
static void dice_shadow_face(const dice_raster_t *r, int32_t soft_q8, int32_t ox, int32_t oy)
{
    int32_t cc = UV_ONE - DICE_CORNER_UV; // Corner circle centre, |u| and |v|
    int32_t rb = DICE_CORNER_UV >> 4;
    int32_t rb2 = rb * rb, inv_rb = (1 << 24) / (2 * rb);
    // A soft shadow is the same outline with a ramp soft_q8 / 256 pixels wide
    int32_t kgu = (int32_t)(((int64_t)r->kgu << 8) / soft_q8);
    int32_t kgv = (int32_t)(((int64_t)r->kgv << 8) / soft_q8);
    int32_t kgi = (int32_t)(((int64_t)r->kgi << 8) / soft_q8);
    // Inside this, every ramp is already full: the fast path skips the edge maths
    int32_t solid = UV_ONE - (int32_t)(((int64_t)r->g_q16 * (soft_q8 + 256)) >> 8);
    // Same pixel in the unshifted face's coordinates, and where that face covers fully
    int32_t su = (int32_t)(((int64_t)r->dudx * ox + (int64_t)r->dudy * oy) >> 8);
    int32_t sv = (int32_t)(((int64_t)r->dvdx * ox + (int64_t)r->dvdy * oy) >> 8);
    int32_t hidden = UV_ONE - r->g_q16;
    int32_t w = r->x1 - r->x0;
    int32_t ur = r->u, vr = r->v;

    for (int32_t y = r->y0; y < r->y1; y++, ur += r->dudy, vr += r->dvdy) {
        // Only the pixels of this row that can be near the face
        int32_t lo = 0, hi = w - 1;
        dice_span(ur, r->dudx, r->lim_u, &lo, &hi);
        dice_span(vr, r->dvdx, r->lim_v, &lo, &hi);
        if (lo > hi) continue;

        uint16_t *row = s_pixels + y * s_stride_px + r->x0;
        int32_t u = ur + lo * r->dudx, v = vr + lo * r->dvdx;
        for (int32_t i = lo; i <= hi; i++, u += r->dudx, v += r->dvdx) {
            // Under the die body: the body pass overwrites it, so skip
            int32_t bu = dice_absi(u + su), bv = dice_absi(v + sv);
            if (bu < hidden && bv < hidden && (bu <= cc || bv <= cc)) continue;

            int32_t au = dice_absi(u), av = dice_absi(v);
            if (au < solid && av < solid && (au <= cc || av <= cc)) {
                row[i] = s_col_shadow; // Clear of every edge and corner
                continue;
            }
            int32_t cov = dice_outline_cov(r, u, v, au, av, cc, rb2, inv_rb, kgu, kgv, kgi);
            if (cov == 0) continue;
            row[i] = (cov == 255) ? s_col_shadow : dice_mix565(row[i], s_col_shadow, cov);
        }
    }
}

// Face content in content coordinates (cu, cv): pips or a numeral, coverage 0..255
typedef struct {
    uint8_t val;        // 0 = blank
    bool pips;
    uint16_t pip_mask;
    int32_t pip_r2, pip_inv;
    uint8_t ndig;
    uint8_t dig[3];
    int32_t gx0, gy0;   // Numeral top-left in uv, Q16
    int32_t gs;         // uv (Q16) to glyph (Q12) scale, Q16
    int32_t gw;         // Numeral width in glyph units
    int32_t kgg;        // 255 / pixel footprint in glyph units
    int32_t gpad;       // Anti-aliasing reach past a stroke, glyph units
} dice_content_t;

// Decide what a face shows: pips for dice up to 6 sides, a centred numeral above (if legible)
static void dice_content_setup(dice_content_t *c, uint8_t val, int32_t g_q16, bool numerals)
{
    c->val = val;
    c->ndig = 0;
    c->pips = false;
    if (val == 0) return;

    if (s_sides <= 6) {
        int32_t r = (val == 1) ? DICE_PIP1_R_Q12 : DICE_PIP_R_Q12;
        c->pips = true;
        c->pip_mask = s_pip_mask[val <= 6 ? val : 0];
        c->pip_r2 = r * r;
        c->pip_inv = (1 << 24) / (2 * r);
        return;
    }
    if (!numerals) return;

    // Split into digits, most significant first
    c->ndig = (val >= 100) ? 3 : (val >= 10) ? 2 : 1;
    for (int i = c->ndig - 1, n = val; i >= 0; i--, n /= 10) c->dig[i] = (uint8_t)(n % 10);

    // Size by digit count and centre the numeral on the face
    int32_t h = s_num_h[c->ndig - 1];
    c->gw = (c->ndig - 1) * DICE_GLYPH_ADV + DICE_GLYPH_W;
    c->gs = (int32_t)(((int64_t)4096 << 16) / h);
    c->gx0 = -(int32_t)(((int64_t)c->gw * h) >> 13); // Half the width, in uv: gw * h / 4096 / 2
    c->gy0 = -(h >> 1);
    int32_t g_gl = (int32_t)(((int64_t)g_q16 * c->gs) >> 16); // Pixel footprint in glyph units
    c->kgg = (255 << 16) / (g_gl > 0 ? g_gl : 1);
    c->gpad = g_gl + (g_gl >> 1) + 1;
}

// Ink coverage at content point (cu, cv), 0..255
static inline int32_t dice_content_cov(const dice_content_t *c, int32_t cu, int32_t cv,
        int32_t kgi)
{
    if (c->pips) {
        // Nearest pip slot on the 3 x 3 grid; a 1 always uses the centre
        int32_t iu = 0, iv = 0;
        if (c->val != 1) {
            iu = cu > (DICE_PIP_POS >> 1) ? 1 : (cu < -(DICE_PIP_POS >> 1) ? -1 : 0);
            iv = cv > (DICE_PIP_POS >> 1) ? 1 : (cv < -(DICE_PIP_POS >> 1) ? -1 : 0);
            if (!((c->pip_mask >> ((iv + 1) * 3 + iu + 1)) & 1)) return 0;
        }
        int32_t du = dice_sat15((cu - iu * DICE_PIP_POS) >> 4);
        int32_t dv = dice_sat15((cv - iv * DICE_PIP_POS) >> 4);
        return dice_round(c->pip_r2 - (du * du + dv * dv), c->pip_inv, kgi);
    }
    if (c->ndig) {
        // Face point to glyph space; skip everything outside the numeral's box. Held within four
        // glyph heights so the stroke maths stays inside int32 on near edge-on faces
        int32_t gx = dice_clampi((int32_t)(((int64_t)(cu - c->gx0) * c->gs) >> 16), -16384, 16384);
        int32_t gy = dice_clampi((int32_t)(((int64_t)(cv - c->gy0) * c->gs) >> 16), -16384, 16384);
        if (gy < -c->gpad || gy > 4096 + c->gpad || gx < -c->gpad || gx > c->gw + c->gpad) {
            return 0;
        }

        // Each pixel belongs to the digit whose box is nearest
        int i = 0;
        int32_t split = DICE_GLYPH_W + ((DICE_GLYPH_ADV - DICE_GLYPH_W) >> 1);
        while (i < c->ndig - 1 && gx > split) {
            gx -= DICE_GLYPH_ADV;
            i++;
        }
        return dice_glyph_cov(c->dig[i], gx, gy, c->kgg, c->gpad);
    }
    return 0;
}

// One visible face: the body outline in the rim colour, the inset face on top, its content on
// that. rot turns the content (see lcd_dice_roll_start)
static void dice_body_face(const dice_raster_t *r, const dice_die_t *d, uint16_t face_col,
        const dice_content_t *c, uint8_t rot)
{
    int32_t cc = UV_ONE - DICE_CORNER_UV; // Corner circle centre, |u| and |v|
    int32_t rb = DICE_CORNER_UV >> 4;
    int32_t rb2 = rb * rb, inv_rb = (1 << 24) / (2 * rb);
    int32_t fe = UV_ONE - d->inset_uv;           // Face edge, inside the rim
    int32_t rf = (DICE_CORNER_UV - d->inset_uv) >> 4; // Face corner, concentric with the body's
    if (rf < 128) rf = 128;
    int32_t rf2 = rf * rf, inv_rf = (1 << 24) / (2 * rf);
    bool content = c->pips || c->ndig;
    // Inside this, the outline and face ramps are already full: the fast path skips them
    int32_t solid = fe - r->g_q16;
    int32_t w = r->x1 - r->x0;
    int32_t ur = r->u, vr = r->v;

    for (int32_t y = r->y0; y < r->y1; y++, ur += r->dudy, vr += r->dvdy) {
        // Only the pixels of this row that can be near the face
        int32_t lo = 0, hi = w - 1;
        dice_span(ur, r->dudx, r->lim_u, &lo, &hi);
        dice_span(vr, r->dvdx, r->lim_v, &lo, &hi);
        if (lo > hi) continue;

        uint16_t *row = s_pixels + y * s_stride_px + r->x0;
        int32_t u = ur + lo * r->dudx, v = vr + lo * r->dvdx;
        for (int32_t i = lo; i <= hi; i++, u += r->dudx, v += r->dvdx) {
            int32_t au = dice_absi(u), av = dice_absi(v);
            int32_t cov = 255, cf = 255; // Body outline and inset face coverage

            // Near an edge or in a corner: work out both coverages
            if (au >= solid || av >= solid || (au > cc && av > cc)) {
                cov = dice_outline_cov(r, u, v, au, av, cc, rb2, inv_rb, r->kgu, r->kgv, r->kgi);
                if (cov == 0) continue;

                // Face inside the rim
                cf = dice_ramp((fe - au) >> 4, r->kgu);
                int32_t t = dice_ramp((fe - av) >> 4, r->kgv);
                if (t < cf) cf = t;
                if (cf && au > cc && av > cc) {
                    int32_t du = dice_sat15((au - cc) >> 4), dv = dice_sat15((av - cc) >> 4);
                    t = dice_round(rf2 - (du * du + dv * dv), inv_rf, r->kgi);
                    if (t < cf) cf = t;
                }
            }

            // Compose: rim, then the face over it, then the ink over the face
            uint16_t col = s_col_rim;
            if (cf) {
                uint16_t fc = face_col;
                if (content) {
                    int32_t cu, cv;
                    switch (rot) {
                    case 1:  cu = -v; cv = u;  break;
                    case 2:  cu = -u; cv = -v; break;
                    case 3:  cu = v;  cv = -u; break;
                    default: cu = u;  cv = v;  break;
                    }
                    int32_t ci = dice_content_cov(c, cu, cv, r->kgi);
                    if (ci) fc = (ci == 255) ? s_col_ink : dice_mix565(fc, s_col_ink, ci);
                }
                col = (cf == 255) ? fc : dice_mix565(s_col_rim, fc, cf);
            }
            // Partial outline coverage blends over what is already there (background or shadow)
            row[i] = (cov == 255) ? col : dice_mix565(row[i], col, cov);
        }
    }
}

/* ===========================================================================
 * Dice
 * ========================================================================= */

// Where a die is at one instant of its roll
typedef struct {
    int32_t m[3][3];  // Body axis j on screen is column j: x, y, toward the viewer
    int32_t half_q8;  // Half edge this frame
    int32_t shadow_q8;
    int32_t soft_q8;  // Shadow edge softness
} dice_pose_t;

// Pose of die d t_ms into its own tumble. At or past dur_ms it is at rest, face-on
static void dice_pose(const dice_die_t *d, uint32_t t_ms, dice_pose_t *p)
{
    if (t_ms >= d->dur_ms) {
        // Landed: identity, so the +Z face (the result) faces the viewer upright
        memset(p->m, 0, sizeof(p->m));
        p->m[0][0] = p->m[1][1] = p->m[2][2] = ONE_Q14;
        p->half_q8 = d->half_q8;
        p->shadow_q8 = d->shadow_q8;
        p->soft_q8 = 256;
        return;
    }

    // Constant deceleration, like a die sliding to a stop: the spin left is (1 - tau)^2
    int32_t tau = (int32_t)((t_ms << 15) / d->dur_ms); // Q15
    int32_t rem = 32768 - tau;
    int32_t f1 = (rem * rem) >> 15;
    int32_t tau2 = tau >= DICE_SPIN2_END_Q15 ? 32768 : (tau << 15) / DICE_SPIN2_END_Q15;
    int32_t rem2 = 32768 - tau2;
    int32_t f2 = (rem2 * rem2) >> 15;

    // Both spins unwind to zero together with the roll, which leaves the identity
    int32_t ra[3][3], rb[3][3];
    dice_rot(ra, d->k1, (int32_t)(((int64_t)d->a1 * f1) >> 15));
    dice_rot(rb, d->k2, (int32_t)(((int64_t)d->a2 * f2) >> 15));
    dice_mul(p->m, ra, rb);

    // Airborne: a few shrinking hops, shown by the shadow pulling away
    int32_t hop = dice_absi(dice_sin_q14(tau * 3)); // 1.5 turns over the roll, Q14
    int32_t lift = (int32_t)(((int64_t)hop * rem) >> 15); // Q14
    int32_t shadow = d->shadow_q8 + (int32_t)(((int64_t)d->shadow_q8 * 2 * lift) >> 14);
    p->soft_q8 = 256 + (int32_t)(((int64_t)512 * lift) >> 14);

    // A tumbling cube is up to sqrt(3) wider than its face: shrink to stay inside the cell
    int32_t ext_x = dice_absi(p->m[0][0]) + dice_absi(p->m[0][1]) + dice_absi(p->m[0][2]);
    int32_t ext_y = dice_absi(p->m[1][0]) + dice_absi(p->m[1][1]) + dice_absi(p->m[1][2]);
    int32_t avail = ((d->x1 - d->x0) << 7); // Half the cell, Q8
    int32_t need_x = (int32_t)(((int64_t)d->half_q8 * ext_x) >> 14) + ((shadow * 3) >> 2);
    int32_t need_y = (int32_t)(((int64_t)d->half_q8 * ext_y) >> 14) + shadow;
    int32_t need = need_x > need_y ? need_x : need_y;
    int32_t scale = (need > avail) ? (int32_t)(((int64_t)avail << 14) / need) : ONE_Q14;

    p->half_q8 = (int32_t)(((int64_t)d->half_q8 * scale) >> 14);
    p->shadow_q8 = (int32_t)(((int64_t)shadow * scale) >> 14);
}

// Paint a canvas rectangle with the page background, half-open
static void dice_fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    for (int32_t y = y0; y < y1; y++) {
        uint16_t *row = s_pixels + y * s_stride_px;
        for (int32_t x = x0; x < x1; x++) row[x] = s_col_bg;
    }
}

// Redraw die d's whole cell in pose p. at_rest: the landed frame, drawn flat in the theme colour
static void dice_draw(const dice_die_t *d, const dice_pose_t *p, bool at_rest)
{
    // A face is visible when its normal points toward the viewer
    bool vis[6];
    for (int f = 0; f < 6; f++) {
        const dice_face_def_t *fd = &s_face_def[f];
        vis[f] = fd->ns * p->m[2][fd->n] > DICE_ZMIN_Q14;
    }

    dice_fill(d->x0, d->y0, d->x1, d->y1);

    bool numerals = (d->half_q8 >> 7) >= DICE_NUM_MIN_PX;
    bool shadow = d->shadow_q8 > 0 && s_col_shadow != s_col_bg;

    // Pass 0 lays every face's shadow first, pass 1 the faces themselves on top
    for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
        int32_t ox = pass ? 0 : (p->shadow_q8 * 3) >> 2;
        int32_t oy = pass ? 0 : p->shadow_q8;

        for (int f = 0; f < 6; f++) {
            if (!vis[f]) continue;
            const dice_face_def_t *fd = &s_face_def[f];

            // Normal, u and v directions on screen: columns of the pose matrix
            int32_t nx = fd->ns * p->m[0][fd->n], ny = fd->ns * p->m[1][fd->n];
            int32_t nz = fd->ns * p->m[2][fd->n];
            int32_t ux = fd->as * p->m[0][fd->a], uy = fd->as * p->m[1][fd->a];
            int32_t vx = fd->bs * p->m[0][fd->b], vy = fd->bs * p->m[1][fd->b];
            int32_t h = p->half_q8;
            // Face centre: die centre pushed out along the normal by half an edge
            int32_t cx = d->cx_q8 + ox + ((nx * h) >> 14);
            int32_t cy = d->cy_q8 + oy + ((ny * h) >> 14);

            dice_raster_t r;
            int32_t pad = pass ? 0 : (p->soft_q8 >> 8);
            if (!dice_raster_setup(&r, d, cx, cy, (ux * h) >> 14, (uy * h) >> 14,
                    (vx * h) >> 14, (vy * h) >> 14, pad)) {
                continue;
            }
            // Edges shared with another visible face are interior: no AA, no rounding
            r.shared = 0;
            if (vis[dice_face_of(fd->a, fd->as)]) r.shared |= DICE_EDGE_UP;
            if (vis[dice_face_of(fd->a, -fd->as)]) r.shared |= DICE_EDGE_UN;
            if (vis[dice_face_of(fd->b, fd->bs)]) r.shared |= DICE_EDGE_VP;
            if (vis[dice_face_of(fd->b, -fd->bs)]) r.shared |= DICE_EDGE_VN;

            if (!pass) {
                dice_shadow_face(&r, p->soft_q8, ox, oy);
                continue;
            }

            // Faces fade toward the dark tone as they turn from the light; the front face at
            // rest gets exactly the theme colour
            int32_t ndl = (nx * DICE_LIGHT_X + ny * DICE_LIGHT_Y + nz * DICE_LIGHT_Z) >> 14;
            int32_t shade = dice_clampi(((DICE_LIGHT_Z - ndl) * DICE_SHADE_GAIN) >> 14, 0, 255);
            if (at_rest) shade = 0;
            uint16_t face_col = shade ? dice_mix565(s_col_face, s_col_dark, shade) : s_col_face;

            dice_content_t c;
            dice_content_setup(&c, d->val[f], r.g_q16, numerals);
            dice_body_face(&r, d, face_col, &c, d->rot[f]);
        }
    }
}

// Grid the s_count dice over the arena: the column count that gives the largest square cells wins
static void dice_layout(void)
{
    int32_t n = s_count;
    int32_t best = 0, cols = 1;

    for (int32_t c = 1; c <= n; c++) {
        int32_t rows = (n + c - 1) / c;
        int32_t s = s_w / c < s_h / rows ? s_w / c : s_h / rows;
        if (s > best) {
            best = s;
            cols = c;
        }
    }

    // Cell size s, grid centred in the arena; the die fills DICE_FILL_PCT of its cell
    int32_t s = best;
    int32_t rows = (n + cols - 1) / cols;
    int32_t ox = (s_w - cols * s) / 2, oy = (s_h - rows * s) / 2;
    int32_t die = (s * DICE_FILL_PCT) / 100;
    if (die > s - 1) die = s - 1;
    if (die < 2) die = 2;

    for (int32_t i = 0; i < n; i++) {
        dice_die_t *d = &s_dice[i];
        int32_t row = i / cols, col = i % cols;
        int32_t in_row = (row == rows - 1) ? n - row * cols : cols;
        int32_t x = ox + ((cols - in_row) * s) / 2 + col * s; // Short last row is centred
        int32_t y = oy + row * s;

        d->x0 = (int16_t)x;
        d->y0 = (int16_t)y;
        d->x1 = (int16_t)(x + s);
        d->y1 = (int16_t)(y + s);

        // Edges on whole pixels, so a die at rest is crisp
        int32_t dx = x + (s - die) / 2, dy = y + (s - die) / 2;
        d->half_q8 = die << 7;
        d->cx_q8 = (dx << 8) + d->half_q8;
        d->cy_q8 = (dy << 8) + d->half_q8;

        // Shadow at rest, limited to the gap so a landed die needs no shrinking
        int32_t shadow = (die * DICE_SHADOW_PCT * 256) / 100;
        int32_t room = (s << 7) - d->half_q8;
        d->shadow_q8 = shadow < room ? shadow : room;
        if (d->shadow_q8 < 256) d->shadow_q8 = 0; // Under a pixel it cannot be seen, only paid for

        // Rim width in face units, so it scales with the die as it shrinks mid-tumble
        int32_t inset = dice_clampi((die * DICE_RIM_PCT * 256) / 100, DICE_RIM_MIN_Q8,
                DICE_RIM_MAX_Q8);
        d->inset_uv = (int32_t)(((int64_t)inset << 16) / d->half_q8);
    }
}

// The result goes on the front face (+Z). The rest get other values, so no number shows twice
static void dice_fill_faces(dice_die_t *d, uint8_t result)
{
    uint8_t n = s_sides;
    d->val[0] = result;

    if (n == 6) {
        // Laid out like a real die: opposite faces sum to 7
        uint8_t pair[2], k = 0;
        for (uint8_t a = 1; a <= 3; a++) {
            if (a != result && a != 7 - result) pair[k++] = a;
        }
        if (dice_rand() & 1) {
            uint8_t t = pair[0];
            pair[0] = pair[1];
            pair[1] = t;
        }
        uint8_t x = (dice_rand() & 1) ? pair[0] : 7 - pair[0];
        uint8_t y = (dice_rand() & 1) ? pair[1] : 7 - pair[1];
        d->val[1] = 7 - result;
        d->val[2] = x;
        d->val[3] = 7 - x;
        d->val[4] = y;
        d->val[5] = 7 - y;
        return;
    }

    for (int f = 1; f < 6; f++) {
        uint8_t v;
        if (n < 2) {
            v = 1;
        } else if (n < 6) {
            // Too few values to go round: avoid the result only
            v = (uint8_t)(1 + dice_rand() % (n - 1));
            if (v >= result) v++;
        } else {
            // Probe upward from a random value to the next one not yet on the die
            v = (uint8_t)(1 + dice_rand() % n);
            for (;;) {
                bool taken = false;
                for (int g = 0; g < f; g++) taken |= (d->val[g] == v);
                if (!taken) break;
                v = (uint8_t)(v % n + 1);
            }
        }
        d->val[f] = v;
    }
}

// Invalidate a canvas rectangle (canvas pixels, half-open) so the next refresh flushes just it
static void dice_invalidate(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    lv_area_t a;
    lv_obj_get_coords(s_canvas, &a);
    lv_area_t dirty = {a.x1 + x0, a.y1 + y0, a.x1 + x1 - 1, a.y1 + y1 - 1}; // Screen, inclusive
    lv_obj_invalidate_area(s_canvas, &dirty);
}

// Our canvas was deleted by someone else (a screen clean in the simulator). Forget it so
// deinit cannot delete it a second time
static void dice_canvas_deleted_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_canvas = NULL;
    s_rolling = false;
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

bool lcd_dice_init(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (s_canvas) return true;

    // Canvas deleted from under us and never deinit'd: release the orphaned buffers
    lcd_dice_deinit();

    s_w = w;
    s_h = h;
    s_buf_size = (size_t)w * h * 2;
    // PSRAM is fine: st7789 CPU-copies into its own staging buffer before SPI
    s_pixels = heap_caps_malloc(s_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_dice = heap_caps_malloc(sizeof(dice_die_t) * DICE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pixels || !s_dice) {
        ESP_LOGE(TAG, "Failed to alloc PSRAM for dice arena");
        lcd_dice_deinit();
        return false;
    }

    if (lv_draw_buf_init(&s_draw_buf, w, h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, s_pixels,
            s_buf_size) != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to init dice draw buffer");
        lcd_dice_deinit();
        return false;
    }
    s_stride_px = s_draw_buf.header.stride / 2; // LVGL reports stride in bytes

    // The canvas renders straight out of s_pixels
    s_canvas = lv_canvas_create(parent);
    if (!s_canvas) {
        ESP_LOGE(TAG, "Failed to create dice canvas");
        lcd_dice_deinit();
        return false;
    }
    lv_obj_add_event_cb(s_canvas, dice_canvas_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_canvas_set_draw_buf(s_canvas, &s_draw_buf);
    lv_obj_set_size(s_canvas, w, h);
    lv_obj_set_pos(s_canvas, x, y);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    // Behind the screen's other widgets: the status icons reach into the arena and stay on top
    lv_obj_move_background(s_canvas);

    // Palette from the theme, per page visit: secondary dice on the primary background, with
    // pips and numerals in the background colour
    s_col_bg = lv_color_to_u16(user_primary_color);
    s_col_ink = s_col_bg;
    s_col_face = lv_color_to_u16(user_secondary_color);
    s_col_rim = dice_mix565(s_col_face, s_col_ink, 72);
    s_col_dark = dice_mix565(s_col_face, s_col_ink, 150);
    s_col_shadow = dice_mix565(s_col_bg, 0x0000, 90);

    dice_build_strokes();
    dice_build_spins();

    // Empty arena until lcd_dice_reset() lays out the dice
    s_count = 0;
    s_rolling = false;
    dice_fill(0, 0, w, h);

    return true;
}

void lcd_dice_reset(uint8_t count, uint8_t sides)
{
    if (!s_canvas) return;

    s_count = count ? count : 1;
    s_sides = sides ? sides : 1;
    s_rolling = false; // Any roll in progress is abandoned
    dice_layout();

    dice_fill(0, 0, s_w, s_h);

    // Every die at rest showing its highest face (its side count) until the first roll; dur_ms 0
    // makes dice_pose() return the rest pose
    dice_pose_t p;
    for (int32_t i = 0; i < s_count; i++) {
        dice_die_t *d = &s_dice[i];
        memset(d->val, 0, sizeof(d->val));
        memset(d->rot, 0, sizeof(d->rot));
        d->val[0] = s_sides;
        d->state = DICE_REST;
        d->dur_ms = 0;
        dice_pose(d, 0, &p);
        dice_draw(d, &p, true);
    }
    lv_obj_invalidate(s_canvas);
}

void lcd_dice_roll_start(const uint8_t *values, uint32_t seed, uint32_t now_ms)
{
    if (!s_canvas || !s_count) return;

    // Dice start one after another, the gap shrinking as the count grows
    s_rng = seed ? seed : 0x9E3779B9u;
    uint32_t stagger = 0;
    if (s_count > 1) {
        stagger = DICE_STAGGER_SPAN_MS / (s_count - 1);
        if (stagger > DICE_STAGGER_MS) stagger = DICE_STAGGER_MS;
    }

    for (int32_t i = 0; i < s_count; i++) {
        dice_die_t *d = &s_dice[i];

        // Result on +Z, other values round the sides
        uint8_t shown = d->val[0]; // What the die shows now, before the roll
        dice_fill_faces(d, values[i]);
        memset(d->rot, 0, sizeof(d->rot));

        // Start face: the one already carrying the shown value, so no number appears twice and a
        // d6 keeps opposites summing to 7. 0 when only the front has it (rolling the same value)
        int start = 0;
        for (int f = 5; f >= 1; f--) {
            if (d->val[f] == shown) start = f;
        }
        if (start == 0 && d->val[0] != shown) {
            start = 1 + (int)(dice_rand() % 5); // Not on the die (d7+): it takes over a side face
            d->val[start] = shown;
        }

        // Spin that brings the start face to the front; the roll unwinds it
        const dice_spin_t *sp;
        int32_t a;
        if (start == 0) {
            // Front stays front: one whole turn about a random axis
            sp = &s_spin_start[dice_rand() % DICE_SPIN_START_N];
            a = (dice_rand() & 1) ? TURN_Q16 : -TURN_Q16;
        } else {
            // Random pick among the spins for that face, plus whole turns either way for
            // 0.6-1.6 turns of travel
            uint32_t n = 0, pick;
            for (size_t j = 0; j < DICE_SPIN_START_N; j++) n += (s_spin_face[j] == start);
            pick = n ? dice_rand() % n : 0;
            sp = &s_spin_start[0];
            for (size_t j = 0; j < DICE_SPIN_START_N; j++) {
                if (s_spin_face[j] == start && pick-- == 0) {
                    sp = &s_spin_start[j];
                    break;
                }
            }
            a = sp->turn;
            if (dice_rand() & 1) a -= TURN_Q16;
            if (dice_absi(a) < (TURN_Q16 * 6) / 10) a += (a < 0) ? -TURN_Q16 : TURN_Q16;
        }
        memcpy(d->k1, sp->k, sizeof(d->k1));
        d->a1 = a;

        // Turn the start face's content so the value shown now does not jump when the roll starts
        int32_t m[3][3];
        dice_rot(m, d->k1, a);
        const dice_face_def_t *sd = &s_face_def[start];
        int32_t ax = sd->as * m[0][sd->a], ay = sd->as * m[1][sd->a];
        d->rot[start] = (ax > ONE_Q14 / 2) ? 0 : (ay > ONE_Q14 / 2) ? 1 : (ax < -ONE_Q14 / 2) ? 2 : 3;

        // A second, steeper spin about a random axis makes each tumble its own
        int32_t ang = (int32_t)(dice_rand() & 0xFFFF);
        int32_t kz = (int32_t)(dice_rand() % 9001) - 4500;
        int32_t kx = dice_sin_q14(ang + TURN_Q16 / 4), ky = dice_sin_q14(ang);
        int32_t len = (int32_t)dice_isqrt((uint32_t)(kx * kx + ky * ky + kz * kz));
        d->k2[0] = (int16_t)(kx * ONE_Q14 / len);
        d->k2[1] = (int16_t)(ky * ONE_Q14 / len);
        d->k2[2] = (int16_t)(kz * ONE_Q14 / len);
        d->a2 = (dice_rand() % 3 == 0) ? 0 : ((dice_rand() & 1) ? TURN_Q16 : -TURN_Q16);

        d->dur_ms = (uint16_t)(DICE_ROLL_MS + dice_rand() % DICE_ROLL_JITTER_MS);
        d->delay_ms = (uint16_t)(i * stagger);
        d->state = DICE_WAIT;
    }

    s_roll_t0 = now_ms;
    s_rolling = true;
}

bool lcd_dice_roll_step(uint32_t now_ms)
{
    if (!s_canvas || !s_rolling) return false;

    uint32_t el = now_ms - s_roll_t0; // Elapsed since the roll started; wrap-safe
    bool busy = false;
    int32_t x0 = s_w, y0 = s_h, x1 = 0, y1 = 0; // Union of the cells redrawn this frame
    dice_pose_t p;

    for (int32_t i = 0; i < s_count; i++) {
        dice_die_t *d = &s_dice[i];
        if (d->state == DICE_REST) continue; // Landed earlier: its pixels are already final
        if (el < d->delay_ms) {
            busy = true; // Not started yet: still shows the old face
            continue;
        }

        // Draw this frame; the frame at or past dur_ms is the landed one, drawn once
        uint32_t t = el - d->delay_ms;
        bool landed = t >= d->dur_ms;
        if (landed) {
            d->state = DICE_REST;
        } else {
            d->state = DICE_ROLL;
            busy = true;
        }
        dice_pose(d, t, &p);
        dice_draw(d, &p, landed);

        if (d->x0 < x0) x0 = d->x0;
        if (d->y0 < y0) y0 = d->y0;
        if (d->x1 > x1) x1 = d->x1;
        if (d->y1 > y1) y1 = d->y1;
    }

    // One invalidated area per frame: only moving dice get flushed
    if (x0 < x1) dice_invalidate(x0, y0, x1, y1);
    s_rolling = busy;
    return busy;
}

void lcd_dice_deinit(void)
{
    // Canvas first: it points into s_pixels
    if (s_canvas) {
        lv_obj_delete(s_canvas);
        s_canvas = NULL;
    }
    if (s_pixels) {
        heap_caps_free(s_pixels);
        s_pixels = NULL;
    }
    if (s_dice) {
        heap_caps_free(s_dice);
        s_dice = NULL;
    }
    memset(&s_draw_buf, 0, sizeof(s_draw_buf));
    s_buf_size = 0;
    s_stride_px = 0;
    s_count = 0;
    s_rolling = false;
}

bool lcd_dice_is_available(void)
{
    return s_canvas != NULL;
}
