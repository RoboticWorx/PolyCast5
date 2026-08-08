#include "lcd_anim_fluid.h"

#ifdef POLYCAST5_EN_WATER_ANIM

#include <string.h>
#include <math.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_attr.h"   // EXT_RAM_BSS_ATTR (POLYCAST5_USE_PSRAM_BSS)

#include "lvgl.h"

#include "lis2dh12.h" // Accelerometer (gravity + shake input)

static const char *TAG = "LCD_FLUID";

#define FP_SHIFT 8   // Q8: 8 fractional bits, so 1.0 is stored as 256
#define FP_ONE   256 // 1.0 in Q8 (1 << FP_SHIFT)
// Float -> Q8, rounded half away from zero. Only ever applied to constant expressions,
// so the float math happens at build time and never on the device.
#define TO_FP(x) ((int32_t)((x) * (float)FP_ONE + ((x) < 0 ? -0.5f : 0.5f)))

// Output canvas (physical LCD area)
#define FLUID_CANVAS_W      240
#define FLUID_CANVAS_H      135

// Simulation box, fluidbox pixel scale, reshaped to the canvas aspect (float for render).
// The solver works in its own larger "sim px" space and is scaled down only at render
// time; keeping the reference's box size lets its tuning constants (smoothing radius,
// rest spacing, pressure k) carry over unchanged instead of needing a full re-tune.
#define FLUID_BOX_W         360.0f
#define FLUID_BOX_H         202.5f  // 360 * 135 / 240
#define FLUID_BOX_D         75.0f   // depth; z=0 is the near glass, z=D the far wall
#define FLUID_CANVAS_SCALE  (FLUID_CANVAS_W / FLUID_BOX_W) // sim px -> canvas px

// Timestep / gravity scaling (used only to derive the folded constants below)
#define FLUID_DT            0.0022f
#define FLUID_GRAVITY_GAIN  4.5f // ball-speed knob (fluidbox default 2.2; higher = faster/livelier)
#define FLUID_G_ACCEL       (9.81f * 12677.0f * FLUID_GRAVITY_GAIN) // px/s^2 per g (9.81 m/s^2 x the reference's sim-px-per-metre scale)
// Per-step velocity increment (Q8 px/step) for a 1 g input component. dt appears twice
// because velocity is stored pre-multiplied by dt: an acceleration a adds a*dt to v
// (px/s), which is a*dt*dt in the px/step units actually kept in s_vel.
#define GRAV_STEP_SCALE     (FLUID_G_ACCEL * FLUID_DT * FLUID_DT * (float)FP_ONE) // ~693 at gain 4.5 (scales with FLUID_GRAVITY_GAIN)

// Pressure coefficients with 0.5*dt^2 folded in (px per density-unit), Q8. Folding the
// timestep in here means fluid_pressures() emits a displacement directly, so the hot
// relaxation loop never has to touch dt at all.
#define FLUID_K_PRESSURE    400000.0f
#define FLUID_K_NEAR        800000.0f
#define P_COEF_FP           TO_FP(0.5f * FLUID_DT * FLUID_DT * FLUID_K_PRESSURE) // ~248
#define PN_COEF_FP          TO_FP(0.5f * FLUID_DT * FLUID_DT * FLUID_K_NEAR)     // ~496

// Particles / solver
#define FLUID_PARTICLE_COUNT 240   // fixed-point makes the sim cheap; flush-limited anyway
#define FLUID_PARTICLE_MAX   320   // pool size; arrays are sized to this, only s_count used
#define FLUID_FRAME_PERIOD   50    // ms (~20 fps; matched to the 20 MHz full-screen flush floor)
#define FLUID_REST_SPACING   17.0f // target inter-particle gap; sets both the seed lattice and rest density

// Q8 geometry / tuning constants
#define FLUID_H_FP           TO_FP(28.0f)                       // smoothing radius (7168)
#define FLUID_H2_FP          ((int32_t)FLUID_H_FP * (int32_t)FLUID_H_FP) // h^2 in Q16 (~51.4M)
#define FLUID_MIN_R2         1024                               // skip near-coincident pairs (r < ~0.13 px)
#define FLUID_MAXD_FP        TO_FP(4.0f)                        // max relax displacement per pair (1024)
#define FLUID_DAMP_FP        TO_FP(0.99f)                       // global velocity damping (viscosity substitute)
#define FLUID_REST_FP        TO_FP(0.25f)                       // wall restitution
#define FLUID_FRICT_FP       TO_FP(0.96f)                       // wall tangential friction
#define FLUID_WALL_JITTER_FP TO_FP(0.35f)                       // wall anti-stick jitter (max)
#define FLUID_WALL_MARGIN    2.0f
#define FLUID_XMIN_FP        TO_FP(FLUID_WALL_MARGIN)
#define FLUID_XMAX_FP        TO_FP(FLUID_BOX_W - FLUID_WALL_MARGIN)
#define FLUID_YMIN_FP        TO_FP(FLUID_WALL_MARGIN)
#define FLUID_YMAX_FP        TO_FP(FLUID_BOX_H - FLUID_WALL_MARGIN)
#define FLUID_ZMIN_FP        TO_FP(FLUID_WALL_MARGIN)
#define FLUID_ZMAX_FP        TO_FP(FLUID_BOX_D - FLUID_WALL_MARGIN)

// IMU shaping (float, once per frame)
#define FLUID_GRAVITY_LP     0.18f // low-pass coefficient: how fast the tilt vector tracks the device
#define FLUID_SHAKE_GAIN     1.6f  // extra gain on the fast (raw - low-pass) residual, i.e. shake response

// Rendering
#define FLUID_PROJ_FOCAL     140.0f // lower = stronger perspective (reference 220); makes far balls clearly smaller on our small canvas
#define FLUID_PARTICLE_RADIUS_PX 6.5f // base radius (reference 6.5); widened so the near/far size gradient spans real pixels
#define FLUID_DISC_MAX_R     8      // hard cap on drawn radius; also sizes the span table
#define FLUID_HIGHLIGHT_LIFT 0.55f  // how far the highlight colour is lifted toward white
#define FLUID_SPEED_LEVELS   64     // speed quantisation (colour LUT columns)
#define FLUID_DEPTH_LEVELS   16     // depth quantisation (colour LUT rows + sort buckets)
#define FLUID_SPEED_COLOR_MAX 12000.0f // white-spray threshold px/s; raised to match the higher gravity so only the fastest balls whiten (reference 5000 at gain 2.2)
#define FLUID_SPEED_COLOR_GAMMA 0.55f
#define FLUID_DEPTH_DIM_MIN  0.60f

// Neighbour grid (cell == smoothing radius, in px). ceil(360/28)=13, ceil(202.5/28)=8, ceil(75/28)=3
#define FLUID_CELL           28
#define FLUID_GRID_W         13
#define FLUID_GRID_H         8
#define FLUID_GRID_D         3
#define FLUID_GRID_CELLS     (FLUID_GRID_W * FLUID_GRID_H * FLUID_GRID_D)

/* ===========================================================================
 * State
 * ========================================================================= */

static lv_obj_t     *s_canvas;    // Our LVGL canvas (parented to the homescreen); NULL = uninitialized
static lv_draw_buf_t s_draw_buf;  // LVGL descriptor wrapping s_pixels as RGB565
static uint8_t      *s_pixels;    // PSRAM framebuffer, FLUID_CANVAS_W * H * 2 bytes
static size_t        s_buf_size;  // Byte size of s_pixels
static int           s_stride_px; // Row stride in PIXELS (the draw-buf stride is in bytes)
static lv_timer_t   *s_timer;     // Drives one substep + one render every FLUID_FRAME_PERIOD

// Particle pools, Q8 fixed-point. The big arrays live in PSRAM (POLYCAST5_USE_PSRAM_BSS)
// to keep internal SRAM free; only a few scalars/pointers remain in SRAM. Doom uses
// the same pattern for its hot arrays and still hits its frame rate, so PSRAM-backed
// per-frame access is fine here. pos/vel/old are px and px/step in Q8.
static int   s_count = FLUID_PARTICLE_COUNT;                                // live particles (<= MAX)
POLYCAST5_USE_PSRAM_BSS static int32_t s_pos[FLUID_PARTICLE_MAX][3];        // position, Q8 sim px
POLYCAST5_USE_PSRAM_BSS static int32_t s_vel[FLUID_PARTICLE_MAX][3];        // velocity, Q8 px/step (dt folded in)
POLYCAST5_USE_PSRAM_BSS static int32_t s_old[FLUID_PARTICLE_MAX][3];        // pre-move position, for velocity recovery
POLYCAST5_USE_PSRAM_BSS static int32_t s_density[FLUID_PARTICLE_MAX];       // Q8, sum of q^2 over neighbours
POLYCAST5_USE_PSRAM_BSS static int32_t s_density_near[FLUID_PARTICLE_MAX];  // Q8, sum of q^3 (short range)
POLYCAST5_USE_PSRAM_BSS static int32_t s_pressure[FLUID_PARTICLE_MAX];      // Q8, may be negative (cohesion)
POLYCAST5_USE_PSRAM_BSS static int32_t s_pressure_near[FLUID_PARTICLE_MAX]; // Q8, always >= 0 (repulsion)
static int32_t s_rest_density_fp;                   // Q8 target density; what pressure is measured against

// Neighbour grid as a linked list: head index per cell + next index per particle, -1 = end
POLYCAST5_USE_PSRAM_BSS static int16_t s_grid_head[FLUID_GRID_CELLS];
POLYCAST5_USE_PSRAM_BSS static int16_t s_next[FLUID_PARTICLE_MAX];

// IMU
static bool  s_accel_present;    // false = no accelerometer fitted, fall back to fixed gravity
static float s_grav_lp[3];       // float low-pass of the raw g-vector
static int32_t s_grav[3];        // per-step velocity increment (Q8 px/step)

// Render scratch, filled by fluid_project_and_color() and consumed by fluid_render().
// Parallel arrays rather than a struct so each pass walks memory linearly, which matters
// more than usual for PSRAM-backed data.
POLYCAST5_USE_PSRAM_BSS static int16_t  s_sx[FLUID_PARTICLE_MAX];    // projected canvas X (px)
POLYCAST5_USE_PSRAM_BSS static int16_t  s_sy[FLUID_PARTICLE_MAX];    // projected canvas Y (px)
POLYCAST5_USE_PSRAM_BSS static uint8_t  s_sr[FLUID_PARTICLE_MAX];    // drawn radius, 1..FLUID_DISC_MAX_R
POLYCAST5_USE_PSRAM_BSS static uint8_t  s_dl[FLUID_PARTICLE_MAX];    // depth level (0 = nearest/brightest)
POLYCAST5_USE_PSRAM_BSS static uint16_t s_scol[FLUID_PARTICLE_MAX];  // body colour, RGB565
POLYCAST5_USE_PSRAM_BSS static uint16_t s_shi[FLUID_PARTICLE_MAX];   // highlight colour, RGB565
POLYCAST5_USE_PSRAM_BSS static uint16_t s_order[FLUID_PARTICLE_MAX]; // indices sorted far -> near

// Built once at init so the frame path is pure table lookups
POLYCAST5_USE_PSRAM_BSS static uint16_t s_color_lut[FLUID_DEPTH_LEVELS * FLUID_SPEED_LEVELS]; // [depth][speed] -> RGB565
POLYCAST5_USE_PSRAM_BSS static uint16_t s_hi_lut[FLUID_DEPTH_LEVELS * FLUID_SPEED_LEVELS];    // same, lifted toward white
POLYCAST5_USE_PSRAM_BSS static uint8_t  s_disc_span[FLUID_DISC_MAX_R + 1][2 * FLUID_DISC_MAX_R + 1]; // [r][dy] -> row half-width

/* ===========================================================================
 * Fixed-point helpers
 * ========================================================================= */

// floor(sqrt(x)) for a uint32 (bit-by-bit; sqrt of a Q16 value yields Q8)
static inline uint32_t fp_isqrt(uint32_t x)
{
    uint32_t res = 0;             // partial root, built one bit at a time from the top
    uint32_t bit = 1UL << 30;     // highest power of 4 that fits in a uint32
    while (bit > x) {             // skip leading zero digit-pairs so we start at x's top bit
        bit >>= 2;                // base-4 digits, hence 2 bits per step
    }
    while (bit) {
        if (x >= res + bit) {     // this digit fits: subtract it and set the bit in the root
            x -= res + bit;       // x carries the running remainder
            res = (res >> 1) + bit;
        } else {                  // digit doesn't fit: just shift the partial root down
            res >>= 1;
        }
        bit >>= 2;                // at most 16 iterations, all shift/add - no divide, no FPU
    }
    return res;
}

// Clamp to [lo, hi]; used on grid cells and LUT indices where a stray value would
// index out of bounds rather than merely look wrong
static inline int fp_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ===========================================================================
 * Precompute: colour ramp, disc spans, rest density
 * ========================================================================= */

static void fluid_build_color_luts(void)
{
    static const float stop_t[4] = {0.00f, 0.45f, 0.78f, 1.00f};
    static const int   stop_c[4][3] = {
        { 10,  45, 165}, // deep blue
        { 40, 125, 235}, // lighter blue
        {150, 205, 250}, // pale
        {255, 255, 255}, // white spray
    };

    for (int d = 0; d < FLUID_DEPTH_LEVELS; d++) { // LUT row = depth (0 = nearest)
        float depth_t = (float)d / (float)(FLUID_DEPTH_LEVELS - 1);
        // Brightness falls off with distance: full at the near glass, DIM_MIN at the back
        float dim = FLUID_DEPTH_DIM_MIN + (1.0f - FLUID_DEPTH_DIM_MIN) * (1.0f - depth_t);

        for (int s = 0; s < FLUID_SPEED_LEVELS; s++) { // LUT column = speed
            float linear = (float)s / (float)(FLUID_SPEED_LEVELS - 1);
            // Gamma < 1 pushes the ramp toward the bright end, so moderate motion already
            // shows colour instead of everything sitting in the deep blue for most speeds
            float speed_t = powf(linear, FLUID_SPEED_COLOR_GAMMA);

            int seg = 0; // find which of the 3 gradient segments speed_t falls in
            while (seg < 2 && speed_t > stop_t[seg + 1]) {
                seg++;
            }
            float span = stop_t[seg + 1] - stop_t[seg];
            float f = (span > 1e-6f) ? (speed_t - stop_t[seg]) / span : 0.0f; // position within the segment
            if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;

            float rr = stop_c[seg][0] + (stop_c[seg + 1][0] - stop_c[seg][0]) * f; // lerp the stop colours
            float gg = stop_c[seg][1] + (stop_c[seg + 1][1] - stop_c[seg][1]) * f;
            float bb = stop_c[seg][2] + (stop_c[seg + 1][2] - stop_c[seg][2]) * f;

            // Body colour: gradient colour dimmed for depth, packed to RGB565 once here so
            // the per-frame path only ever does an array lookup (no powf, no lerp, no convert)
            int br = (int)(rr * dim), bg = (int)(gg * dim), bb2 = (int)(bb * dim);
            s_color_lut[d * FLUID_SPEED_LEVELS + s] =
                lv_color_to_u16(lv_color_make((uint8_t)br, (uint8_t)bg, (uint8_t)bb2));

            // Highlight colour: same hue lifted toward white, splatted later as a small
            // offset disc to fake a specular dot on each ball. Dimmed by depth as well so
            // the highlight recedes with the particle instead of popping at the back.
            int hr = (int)((rr + (255.0f - rr) * FLUID_HIGHLIGHT_LIFT) * dim);
            int hg = (int)((gg + (255.0f - gg) * FLUID_HIGHLIGHT_LIFT) * dim);
            int hb = (int)((bb + (255.0f - bb) * FLUID_HIGHLIGHT_LIFT) * dim);
            if (hr > 255) hr = 255; 
            if (hg > 255) hg = 255;
            if (hb > 255) hb = 255;
            s_hi_lut[d * FLUID_SPEED_LEVELS + s] =
                lv_color_to_u16(lv_color_make((uint8_t)hr, (uint8_t)hg, (uint8_t)hb));
        }
    }
}

static void fluid_build_disc_spans(void)
{
    for (int r = 0; r <= FLUID_DISC_MAX_R; r++) {
        for (int dy = -r; dy <= r; dy++) { // dy + r biases the row index to 0..2r
            float w = sqrtf((float)(r * r - dy * dy)); // half-width of the circle at this row
            // Store it so drawing a disc becomes (2r + 1) horizontal runs with no
            // per-pixel distance test in the hot path
            s_disc_span[r][dy + r] = (uint8_t)(w + 0.5f);
        }
    }
}

// Rest density via kernel summation over a perfect lattice at REST_SPACING (float,
// once), stored as Q8. Uses the same q = 1 - r/h kernel as the solver.
static void fluid_calc_rest_density(void)
{
    const float h = 28.0f;
    const float inv_h = 1.0f / h;
    float rho = 0.0f;
    int range = (int)ceilf(h / FLUID_REST_SPACING) + 1; // lattice steps needed to cover h
    for (int dz = -range; dz <= range; dz++) {
        for (int dy = -range; dy <= range; dy++) {
            for (int dx = -range; dx <= range; dx++) {
                if (dx == 0 && dy == 0 && dz == 0) continue; // a particle isn't its own neighbour
                float r = sqrtf((float)(dx * dx + dy * dy + dz * dz)) * FLUID_REST_SPACING;
                if (r < h) {            // only lattice sites inside the smoothing radius contribute
                    float q = 1.0f - r * inv_h;
                    rho += q * q;       // same q^2 kernel fluid_density() uses
                }
            }
        }
    }
    // This is the value fluid_pressures() measures against: deriving it from the actual
    // kernel (rather than hand-tuning) is what keeps the fluid from either slowly
    // collapsing or steadily inflating
    s_rest_density_fp = (int32_t)(rho * FP_ONE + 0.5f);
}

// Uniform random float in [0, 1] from the hardware RNG
static inline float fluid_rand_unit(void)
{
    return (float)esp_random() / (float)UINT32_MAX;
}

static void fluid_seed(void)
{
    const float sp = FLUID_REST_SPACING; // lattice pitch == rest spacing, so the fluid starts relaxed
    const float x0 = FLUID_BOX_W * 0.18f, x1 = FLUID_BOX_W * 0.82f; // inset from the walls
    const float z0 = FLUID_BOX_D * 0.20f, z1 = FLUID_BOX_D * 0.80f;

    int idx = 0;
    float y = FLUID_BOX_H * 0.88f; // start high in the box so the first frames visibly fall
    while (idx < s_count) {
        for (float z = z0; z <= z1 && idx < s_count; z += sp) {     // fill a slab at this height,
            for (float x = x0; x <= x1 && idx < s_count; x += sp) { // x-major within each z row
                // +-1 px of noise: a perfectly regular lattice looks crystalline and
                // relaxes in an unnaturally synchronised way for the first few frames
                s_pos[idx][0] = TO_FP(x + (fluid_rand_unit() - 0.5f) * 2.0f);
                s_pos[idx][1] = TO_FP(y + (fluid_rand_unit() - 0.5f) * 2.0f);
                s_pos[idx][2] = TO_FP(z + (fluid_rand_unit() - 0.5f) * 2.0f);
                s_vel[idx][0] = s_vel[idx][1] = s_vel[idx][2] = 0; // start at rest; gravity does the rest
                idx++;
            }
        }
        y -= sp;                                                   // next slab down
        if (y < FLUID_BOX_H * 0.05f) y = FLUID_BOX_H * 0.88f;      // ran out of box: wrap and stack another layer
    }
}

/* ===========================================================================
 * Solver (Clavet double-density relaxation, fixed-point)
 * ========================================================================= */

static void fluid_read_gravity(void)
{
    float mx, my, mz; // mapped acceleration input (g units), sim axes
    if (s_accel_present) {
        float ax, ay, az;
        // Non-blocking: never stall the lcd_task render path on I2C bus contention.
        // On a busy bus or transient failure we keep the last s_grav for this frame.
        if (lis2dh12_read_g_nonblocking(&ax, &ay, &az) != ESP_OK) {
            return;
        }
        // Board -> sim axes. x/y anchored to the eCompass MODE_FLAT convention
        // (driver compensates the 180-degree mount). These three signs are the
        // only bench-tunable: flip an axis if the fluid flows the wrong way; z
        // uses -az so a flat screen-up device pools at the near glass (bright).
        float gx = -ay, gy = -ax, gz = -az;
        s_grav_lp[0] += (gx - s_grav_lp[0]) * FLUID_GRAVITY_LP;
        s_grav_lp[1] += (gy - s_grav_lp[1]) * FLUID_GRAVITY_LP;
        s_grav_lp[2] += (gz - s_grav_lp[2]) * FLUID_GRAVITY_LP;
        // Low-pass tracks slow tilt (= gravity); the leftover (raw - low-pass) is the
        // fast shake component, re-added with extra gain so a flick visibly throws the
        // water around instead of being smoothed away
        mx = s_grav_lp[0] + (gx - s_grav_lp[0]) * FLUID_SHAKE_GAIN;
        my = s_grav_lp[1] + (gy - s_grav_lp[1]) * FLUID_SHAKE_GAIN;
        mz = s_grav_lp[2] + (gz - s_grav_lp[2]) * FLUID_SHAKE_GAIN;
    } else {
        mx = 0.0f; my = 1.0f; mz = 0.0f; // gentle pull down the screen
    }

    // g units -> Q8 px/step, the form fluid_integrate() adds straight onto velocity
    s_grav[0] = (int32_t)(mx * GRAV_STEP_SCALE);
    s_grav[1] = (int32_t)(my * GRAV_STEP_SCALE);
    s_grav[2] = (int32_t)(mz * GRAV_STEP_SCALE);
}

static void fluid_integrate(void)
{
    const int32_t gx = s_grav[0], gy = s_grav[1], gz = s_grav[2];
    for (int i = 0; i < s_count; i++) {
        // v += gravity, then light global damping (viscosity substitute)
        int32_t vx = s_vel[i][0] + gx;
        int32_t vy = s_vel[i][1] + gy;
        int32_t vz = s_vel[i][2] + gz;
        // int64 intermediate: velocity can be large in Q8 and the damping multiply
        // would otherwise overflow int32 before the shift brings it back down
        vx = (int32_t)(((int64_t)vx * FLUID_DAMP_FP) >> FP_SHIFT);
        vy = (int32_t)(((int64_t)vy * FLUID_DAMP_FP) >> FP_SHIFT);
        vz = (int32_t)(((int64_t)vz * FLUID_DAMP_FP) >> FP_SHIFT);
        s_vel[i][0] = vx; s_vel[i][1] = vy; s_vel[i][2] = vz;

        // Stash the pre-move position: fluid_recover_vel() diffs against it later to get
        // the velocity, which is how the relaxation pass ends up affecting momentum
        s_old[i][0] = s_pos[i][0]; s_old[i][1] = s_pos[i][1]; s_old[i][2] = s_pos[i][2];
        s_pos[i][0] += vx; s_pos[i][1] += vy; s_pos[i][2] += vz; // velocity is already px/step
    }
}

static inline int fluid_cell_of(int32_t p, int gmax)
{
    // Arithmetic shift, so a slightly negative position (possible mid-step, before
    // fluid_walls() runs) floors to -1 and clamps to 0 rather than wrapping huge
    int c = (p >> FP_SHIFT) / FLUID_CELL; // Q8 px -> px -> cell
    return fp_clampi(c, 0, gmax - 1);
}

// Rebuilt from scratch every substep: cheaper than incremental maintenance at this count
static void fluid_build_grid(void)
{
    memset(s_grid_head, 0xFF, sizeof(s_grid_head)); // 0xFF bytes == -1 in int16_t, i.e. "empty cell"
    for (int i = 0; i < s_count; i++) {
        int cx = fluid_cell_of(s_pos[i][0], FLUID_GRID_W);
        int cy = fluid_cell_of(s_pos[i][1], FLUID_GRID_H);
        int cz = fluid_cell_of(s_pos[i][2], FLUID_GRID_D);
        int cell = (cz * FLUID_GRID_H + cy) * FLUID_GRID_W + cx; // flatten 3D cell -> 1D index
        // Push onto the front of this cell's singly linked list. Storing head-per-cell +
        // next-per-particle avoids any per-cell counting pass or variable-size buckets;
        // lists end up in reverse insertion order, which no consumer cares about.
        s_next[i] = s_grid_head[cell];
        s_grid_head[cell] = (int16_t)i;
    }
}

// Density + near-density (self-accumulated; each particle sums over its neighbours).
static void fluid_density(void)
{
    for (int i = 0; i < s_count; i++) {
        const int32_t xi = s_pos[i][0], yi = s_pos[i][1], zi = s_pos[i][2];
        int cx = fluid_cell_of(xi, FLUID_GRID_W);
        int cy = fluid_cell_of(yi, FLUID_GRID_H);
        int cz = fluid_cell_of(zi, FLUID_GRID_D);

        int32_t rho = 0, rho_near = 0;

        // Cell size == smoothing radius h, so the 3x3x3 block around this particle is
        // guaranteed to contain every neighbour within h - nothing outside can matter
        for (int oz = -1; oz <= 1; oz++) {
            int nz = cz + oz; if (nz < 0 || nz >= FLUID_GRID_D) continue; // skip cells off the grid
            for (int oy = -1; oy <= 1; oy++) {
                int ny = cy + oy; if (ny < 0 || ny >= FLUID_GRID_H) continue;
                for (int ox = -1; ox <= 1; ox++) {
                    int nx = cx + ox; if (nx < 0 || nx >= FLUID_GRID_W) continue;
                    int j = s_grid_head[(nz * FLUID_GRID_H + ny) * FLUID_GRID_W + nx];
                    while (j != -1) { // walk this cell's linked list
                        if (j != i) {
                            // Axis-aligned box rejects before the expensive r2/sqrt: two
                            // compares each throw out most candidates, and this is the
                            // single biggest win in the whole solver
                            int32_t dx = s_pos[j][0] - xi;
                            if (dx < FLUID_H_FP && dx > -FLUID_H_FP) {
                                int32_t dy = s_pos[j][1] - yi;
                                if (dy < FLUID_H_FP && dy > -FLUID_H_FP) {
                                    int32_t dz = s_pos[j][2] - zi;
                                    if (dz < FLUID_H_FP && dz > -FLUID_H_FP) {
                                        int32_t r2 = dx * dx + dy * dy + dz * dz; // < 3*h^2, int32-safe
                                        // MIN_R2 drops near-coincident pairs, which would
                                        // divide by ~0 in fluid_relax() and fling particles
                                        if (r2 < FLUID_H2_FP && r2 >= FLUID_MIN_R2) {
                                            int32_t r = (int32_t)fp_isqrt((uint32_t)r2);       // Q16 in -> Q8 out
                                            int32_t q = FP_ONE - (r * FP_ONE) / FLUID_H_FP;    // kernel q = 1 - r/h, Q8 [0..256]
                                            int32_t q2 = (q * q) >> FP_SHIFT;                  // Q8
                                            rho += q2;                          // q^2: ordinary density, holds rest spacing
                                            rho_near += (q2 * q) >> FP_SHIFT;   // q^3: sharper short-range term, resists clumping
                                        }
                                    }
                                }
                            }
                        }
                        j = s_next[j];
                    }
                }
            }
        }
        s_density[i] = rho;
        s_density_near[i] = rho_near;
    }
}

static void fluid_pressures(void)
{
    for (int i = 0; i < s_count; i++) {
        // P = k * (rho - rho_rest): goes NEGATIVE where the neighbourhood is sparser than
        // rest, which fluid_relax() turns into cohesion (the fluid pulls itself together)
        s_pressure[i] = (int32_t)(((int64_t)P_COEF_FP * (s_density[i] - s_rest_density_fp)) >> FP_SHIFT);
        // Pnear = k_near * rho_near: always >= 0, so it is purely repulsive at close range
        s_pressure_near[i] = (int32_t)(((int64_t)PN_COEF_FP * s_density_near[i]) >> FP_SHIFT);
    }
}

// Double-density relaxation: self-accumulated, in-place. The symmetric formula
// makes this equivalent to displacing both particles of every pair.
static void fluid_relax(void)
{
    for (int i = 0; i < s_count; i++) {
        const int32_t xi = s_pos[i][0], yi = s_pos[i][1], zi = s_pos[i][2];
        const int32_t p_i = s_pressure[i], pn_i = s_pressure_near[i];
        int cx = fluid_cell_of(xi, FLUID_GRID_W);
        int cy = fluid_cell_of(yi, FLUID_GRID_H);
        int cz = fluid_cell_of(zi, FLUID_GRID_D);

        int32_t mx = 0, my = 0, mz = 0; // accumulated displacement for this particle

        for (int oz = -1; oz <= 1; oz++) { // same 3x3x3 neighbourhood walk as fluid_density()
            int nz = cz + oz; if (nz < 0 || nz >= FLUID_GRID_D) continue;
            for (int oy = -1; oy <= 1; oy++) {
                int ny = cy + oy; if (ny < 0 || ny >= FLUID_GRID_H) continue;
                for (int ox = -1; ox <= 1; ox++) {
                    int nx = cx + ox; if (nx < 0 || nx >= FLUID_GRID_W) continue;
                    int j = s_grid_head[(nz * FLUID_GRID_H + ny) * FLUID_GRID_W + nx];
                    while (j != -1) {
                        if (j != i) {
                            int32_t dx = s_pos[j][0] - xi;
                            if (dx < FLUID_H_FP && dx > -FLUID_H_FP) {
                                int32_t dy = s_pos[j][1] - yi;
                                if (dy < FLUID_H_FP && dy > -FLUID_H_FP) {
                                    int32_t dz = s_pos[j][2] - zi;
                                    if (dz < FLUID_H_FP && dz > -FLUID_H_FP) {
                                        int32_t r2 = dx * dx + dy * dy + dz * dz;
                                        if (r2 < FLUID_H2_FP && r2 >= FLUID_MIN_R2) {
                                            int32_t r = (int32_t)fp_isqrt((uint32_t)r2);    // Q8
                                            int32_t q = FP_ONE - (r * FP_ONE) / FLUID_H_FP; // Q8
                                            int32_t q2 = (q * q) >> FP_SHIFT;               // Q8
                                            // d = (P_i+P_j)*q + (Pn_i+Pn_j)*q^2  (Q8 px)
                                            int32_t d = (int32_t)((
                                                (int64_t)(p_i + s_pressure[j]) * q +
                                                (int64_t)(pn_i + s_pressure_near[j]) * q2) >> FP_SHIFT);
                                            // Cap per-pair displacement: a momentarily huge
                                            // density (right after a hard shake) would
                                            // otherwise fling particles across the box
                                            if (d > FLUID_MAXD_FP) d = FLUID_MAXD_FP;
                                            else if (d < -FLUID_MAXD_FP) d = -FLUID_MAXD_FP;
                                            // unit vector via one reciprocal: inv_r = 65536/r (Q8 of 1/r_real)
                                            int32_t inv_r = (FP_ONE * FP_ONE) / r; // one divide per pair, not three
                                            int32_t ux = (dx * inv_r) >> FP_SHIFT; // Q8 [-256..256]
                                            int32_t uy = (dy * inv_r) >> FP_SHIFT;
                                            int32_t uz = (dz * inv_r) >> FP_SHIFT;
                                            // d points from i toward j, so subtract to push i
                                            // AWAY from j (and toward it when d went negative)
                                            mx -= (ux * d) >> FP_SHIFT;
                                            my -= (uy * d) >> FP_SHIFT;
                                            mz -= (uz * d) >> FP_SHIFT;
                                        }
                                    }
                                }
                            }
                        }
                        j = s_next[j];
                    }
                }
            }
        }
        // Apply in place. Velocity is deliberately untouched here - deriving it from the
        // net position change is what makes this position-based rather than force-based
        s_pos[i][0] = xi + mx;
        s_pos[i][1] = yi + my;
        s_pos[i][2] = zi + mz;
    }
}

// Position-based dynamics: whatever net motion integration + relaxation produced this
// step IS the velocity for the next one, so pressure affects momentum with no forces
static void fluid_recover_vel(void)
{
    for (int i = 0; i < s_count; i++) {
        s_vel[i][0] = s_pos[i][0] - s_old[i][0]; // px/step (Q8)
        s_vel[i][1] = s_pos[i][1] - s_old[i][1];
        s_vel[i][2] = s_pos[i][2] - s_old[i][2];
    }
}

// Random inward nudge (0 .. FLUID_WALL_JITTER_FP, Q8 px) applied when clamping to a wall.
// Without it, particles pinned to the exact boundary every step form a visibly flat,
// motionless film along the edge.
static inline int32_t fluid_jitter(void)
{
    return (int32_t)(esp_random() % (FLUID_WALL_JITTER_FP + 1));
}

static void fluid_walls(void)
{
    // Each axis handled independently: clamp just inside the wall (plus jitter), reflect
    // the inbound velocity component scaled by restitution (bounce loses energy), and
    // scale the two tangential components by friction (drag along the surface).
    for (int i = 0; i < s_count; i++) {
        if (s_pos[i][0] < FLUID_XMIN_FP) {
            s_pos[i][0] = FLUID_XMIN_FP + fluid_jitter();
            // Only flip if still travelling INTO the wall: relaxation may have already
            // shoved this particle back inward, and flipping again would trap it here
            if (s_vel[i][0] < 0) s_vel[i][0] = (int32_t)(((int64_t)(-s_vel[i][0]) * FLUID_REST_FP) >> FP_SHIFT);
            s_vel[i][1] = (int32_t)(((int64_t)s_vel[i][1] * FLUID_FRICT_FP) >> FP_SHIFT); // tangential drag
            s_vel[i][2] = (int32_t)(((int64_t)s_vel[i][2] * FLUID_FRICT_FP) >> FP_SHIFT);
        } else if (s_pos[i][0] > FLUID_XMAX_FP) {
            s_pos[i][0] = FLUID_XMAX_FP - fluid_jitter();
            if (s_vel[i][0] > 0) s_vel[i][0] = -(int32_t)(((int64_t)s_vel[i][0] * FLUID_REST_FP) >> FP_SHIFT);
            s_vel[i][1] = (int32_t)(((int64_t)s_vel[i][1] * FLUID_FRICT_FP) >> FP_SHIFT);
            s_vel[i][2] = (int32_t)(((int64_t)s_vel[i][2] * FLUID_FRICT_FP) >> FP_SHIFT);
        }
        if (s_pos[i][1] < FLUID_YMIN_FP) {
            s_pos[i][1] = FLUID_YMIN_FP + fluid_jitter();
            if (s_vel[i][1] < 0) s_vel[i][1] = (int32_t)(((int64_t)(-s_vel[i][1]) * FLUID_REST_FP) >> FP_SHIFT);
            s_vel[i][0] = (int32_t)(((int64_t)s_vel[i][0] * FLUID_FRICT_FP) >> FP_SHIFT);
            s_vel[i][2] = (int32_t)(((int64_t)s_vel[i][2] * FLUID_FRICT_FP) >> FP_SHIFT);
        } else if (s_pos[i][1] > FLUID_YMAX_FP) {
            s_pos[i][1] = FLUID_YMAX_FP - fluid_jitter();
            if (s_vel[i][1] > 0) s_vel[i][1] = -(int32_t)(((int64_t)s_vel[i][1] * FLUID_REST_FP) >> FP_SHIFT);
            s_vel[i][0] = (int32_t)(((int64_t)s_vel[i][0] * FLUID_FRICT_FP) >> FP_SHIFT);
            s_vel[i][2] = (int32_t)(((int64_t)s_vel[i][2] * FLUID_FRICT_FP) >> FP_SHIFT);
        }
        if (s_pos[i][2] < FLUID_ZMIN_FP) {
            s_pos[i][2] = FLUID_ZMIN_FP + fluid_jitter();
            if (s_vel[i][2] < 0) s_vel[i][2] = (int32_t)(((int64_t)(-s_vel[i][2]) * FLUID_REST_FP) >> FP_SHIFT);
            s_vel[i][0] = (int32_t)(((int64_t)s_vel[i][0] * FLUID_FRICT_FP) >> FP_SHIFT);
            s_vel[i][1] = (int32_t)(((int64_t)s_vel[i][1] * FLUID_FRICT_FP) >> FP_SHIFT);
        } else if (s_pos[i][2] > FLUID_ZMAX_FP) {
            s_pos[i][2] = FLUID_ZMAX_FP - fluid_jitter();
            if (s_vel[i][2] > 0) s_vel[i][2] = -(int32_t)(((int64_t)s_vel[i][2] * FLUID_REST_FP) >> FP_SHIFT);
            s_vel[i][0] = (int32_t)(((int64_t)s_vel[i][0] * FLUID_FRICT_FP) >> FP_SHIFT);
            s_vel[i][1] = (int32_t)(((int64_t)s_vel[i][1] * FLUID_FRICT_FP) >> FP_SHIFT);
        }
    }
}

static void fluid_substep(void)
{
    fluid_integrate();    // predict: apply gravity + damping, advance positions
    fluid_build_grid();   // re-bucket AFTER the move, so neighbour lookups match new positions
    fluid_density();      // measure rho / rho_near at the predicted positions
    fluid_pressures();    // equations of state: density -> pressure
    fluid_relax();        // correct: push overlapping neighbours apart
    fluid_recover_vel();  // velocity = net move, so it must run after relax...
    fluid_walls();        // ...and before the wall response, which overwrites velocity
}

/* ===========================================================================
 * Rendering (float projection + depth/speed colour; low volume)
 * ========================================================================= */

// Scale factors folded once at load time so the per-frame path has no divides
static const float s_speed_scale = (FLUID_SPEED_LEVELS - 1) / FLUID_SPEED_COLOR_MAX; // px/s -> LUT column
static const float s_depth_scale = (FLUID_DEPTH_LEVELS - 1) / FLUID_BOX_D;           // sim z -> LUT row
static const float s_fp_to_px    = 1.0f / (float)FP_ONE;                             // Q8 -> float px
static const float s_step_to_pxs = (1.0f / (float)FP_ONE) / FLUID_DT; // Q8 px/step -> px/s

// O(particles) with no inner loop, so this float work costs far less than the O(pairs) solver
static void fluid_project_and_color(void)
{
    for (int i = 0; i < s_count; i++) {
        float x = s_pos[i][0] * s_fp_to_px;
        float y = s_pos[i][1] * s_fp_to_px;
        float z = s_pos[i][2] * s_fp_to_px;
        // Pinhole divide: larger z (further away) shrinks toward the box centre
        float scale = FLUID_PROJ_FOCAL / (FLUID_PROJ_FOCAL + z);

        // Project about the box centre, then convert sim px -> canvas px
        float px = (FLUID_BOX_W * 0.5f + (x - FLUID_BOX_W * 0.5f) * scale) * FLUID_CANVAS_SCALE;
        float py = (FLUID_BOX_H * 0.5f + (y - FLUID_BOX_H * 0.5f) * scale) * FLUID_CANVAS_SCALE;
        s_sx[i] = (int16_t)(px + 0.5f);
        s_sy[i] = (int16_t)(py + 0.5f);

        // Same perspective scale drives the radius, giving the near/far size gradient
        int r = (int)(FLUID_PARTICLE_RADIUS_PX * scale * FLUID_CANVAS_SCALE + 0.5f);
        s_sr[i] = (uint8_t)fp_clampi(r, 1, FLUID_DISC_MAX_R); // min 1 so distant particles stay visible

        int32_t vx = s_vel[i][0], vy = s_vel[i][1], vz = s_vel[i][2];
        // int64 before the sum: three squared Q8 velocities can exceed int32
        float vmag = sqrtf((float)((int64_t)vx * vx + (int64_t)vy * vy + (int64_t)vz * vz));
        float speed = vmag * s_step_to_pxs; // px/s, so SPEED_COLOR_MAX is in real units
        int sl = fp_clampi((int)(speed * s_speed_scale), 0, FLUID_SPEED_LEVELS - 1); // colour column
        int dl = fp_clampi((int)(z * s_depth_scale + 0.5f), 0, FLUID_DEPTH_LEVELS - 1); // colour row + sort bucket
        s_dl[i] = (uint8_t)dl;
        s_scol[i] = s_color_lut[dl * FLUID_SPEED_LEVELS + sl]; // body colour, one lookup
        s_shi[i] = s_hi_lut[dl * FLUID_SPEED_LEVELS + sl];     // matching highlight colour
    }
}

// Counting sort into depth buckets: two linear passes, no comparisons, and it gives
// fluid_render() the far -> near order the painter's algorithm needs
static void fluid_sort_by_depth(void)
{
    int cnt[FLUID_DEPTH_LEVELS];
    int base[FLUID_DEPTH_LEVELS];
    memset(cnt, 0, sizeof(cnt));
    for (int i = 0; i < s_count; i++) cnt[s_dl[i]]++; // how many particles per depth level
    int acc = 0;
    // Walk depth high -> low so the FARTHEST level gets the lowest output offset, i.e.
    // ends up first in s_order and is therefore drawn first (underneath everything else)
    for (int d = FLUID_DEPTH_LEVELS - 1; d >= 0; d--) { base[d] = acc; acc += cnt[d]; }
    for (int i = 0; i < s_count; i++) s_order[base[s_dl[i]]++] = (uint16_t)i; // scatter into place
}

// Splat one filled disc straight into the RGB565 framebuffer - no LVGL drawing calls
static void fluid_draw_disc(uint16_t *buf, int cx, int cy, int r, uint16_t col)
{
    if (r < 0) return;
    if (r > FLUID_DISC_MAX_R) r = FLUID_DISC_MAX_R; // guard the span table bound
    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= FLUID_CANVAS_H) continue; // row off-canvas, skip it
        int span = s_disc_span[r][dy + r];            // precomputed half-width for this row
        int x0 = cx - span, x1 = cx + span;
        if (x0 < 0) x0 = 0;                           // clip the run; a centre partly (or
        if (x1 >= FLUID_CANVAS_W) x1 = FLUID_CANVAS_W - 1; // wholly) off-screen stays safe
        uint16_t *row = buf + py * s_stride_px;       // stride, not width - they can differ
        for (int px = x0; px <= x1; px++) row[px] = col;
    }
}

static void fluid_render(void)
{
    uint16_t *buf = (uint16_t *)s_pixels;
    memset(s_pixels, 0, s_buf_size); // black background

    fluid_project_and_color();
    fluid_sort_by_depth();

    for (int k = 0; k < s_count; k++) { // back-to-front, so near particles overwrite far ones
        int i = s_order[k];
        int r = s_sr[i];
        fluid_draw_disc(buf, s_sx[i], s_sy[i], r, s_scol[i]); // body
        if (r >= 2) { // below that the highlight is a pixel or two of noise, so skip it
            // Offset up-left by r/3 at half size: a cheap fake specular dot
            fluid_draw_disc(buf, s_sx[i] - r / 3, s_sy[i] - r / 3, r / 2, s_shi[i]);
        }
    }
}

// Runs on lcd_task via lv_timer_handler(), once per FLUID_FRAME_PERIOD, so everything
// it touches must stay non-blocking (hence the non-blocking accelerometer read)
static void fluid_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_canvas || !s_pixels) return; // init failed or not run yet: nothing to drive

    fluid_read_gravity();
    fluid_substep();

    fluid_render();              // writes the framebuffer directly (no LVGL invalidation)
    lv_obj_invalidate(s_canvas); // schedule the single canvas redraw for this frame
}

/* ===========================================================================
 * Public API
 * ========================================================================= */

bool lcd_anim_fluid_init(lv_obj_t *parent)
{
    if (s_canvas) return true; // already initialized; repeat calls are no-ops

    s_count = FLUID_PARTICLE_COUNT;
    if (s_count > FLUID_PARTICLE_MAX) s_count = FLUID_PARTICLE_MAX; // never outrun the pools

    s_buf_size = (size_t)FLUID_CANVAS_W * FLUID_CANVAS_H * 2; // RGB565 = 2 bytes/pixel
    // PSRAM: ~63 KB is far too big for internal SRAM, and the st7789 flush CPU-copies
    // into its own internal staging buffer before SPI, so a PSRAM source is fine here
    s_pixels = heap_caps_malloc(s_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pixels) {
        ESP_LOGE(TAG, "Failed to alloc PSRAM for fluid canvas");
        return false; // caller falls back to another animation
    }

    lv_draw_buf_init(&s_draw_buf, FLUID_CANVAS_W, FLUID_CANVAS_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, s_pixels, s_buf_size);
    s_stride_px = s_draw_buf.header.stride / 2; // LVGL reports stride in bytes; we index in pixels

    s_canvas = lv_canvas_create(parent);
    if (!s_canvas) {
        ESP_LOGE(TAG, "Failed to create fluid canvas");
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    lv_canvas_set_draw_buf(s_canvas, &s_draw_buf); // canvas renders straight out of s_pixels
    lv_obj_set_size(s_canvas, FLUID_CANVAS_W, FLUID_CANVAS_H);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE); // it's a backdrop, not a widget

    fluid_build_color_luts();  // one-off float/powf work, kept out of the frame path
    fluid_build_disc_spans();
    fluid_calc_rest_density();

    // lis2dh12_init() already ran (synchronously, before the tasks started), so this
    // latches whether the chip is actually fitted and responding
    s_accel_present = lis2dh12_is_present();
    s_grav_lp[0] = s_grav_lp[1] = 0.0f;
    s_grav_lp[2] = s_accel_present ? -1.0f : 0.0f; // seed toward "flat" (gz uses -az)
    // With no IMU, seed a downward pull so the very first frames already fall; with one,
    // the first fluid_read_gravity() overwrites this before it is ever used
    s_grav[0] = 0; s_grav[1] = s_accel_present ? 0 : (int32_t)GRAV_STEP_SCALE; s_grav[2] = 0;

    fluid_seed();
    memset(s_pixels, 0, s_buf_size); // black first frame, in case we're shown before the first tick

    // Created running: lcd_anim.c pauses it immediately if WATER isn't the active anim
    s_timer = lv_timer_create(fluid_timer_cb, FLUID_FRAME_PERIOD, NULL);
    if (!s_timer) {
        ESP_LOGE(TAG, "Failed to create fluid timer");
        lv_obj_delete(s_canvas); // unwind everything so a later retry starts clean
        s_canvas = NULL;         // NULL again, so the s_canvas guard above won't claim success
        heap_caps_free(s_pixels);
        s_pixels = NULL;
        return false;
    }
    return true;
}

void lcd_anim_fluid_start(void)
{
    if (!s_canvas) return; // init failed: stay silent rather than crash
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    if (s_timer) lv_timer_resume(s_timer);
}

// Stop simulating but leave the canvas visible (frozen on its last frame)
void lcd_anim_fluid_pause(void)
{
    if (s_timer) lv_timer_pause(s_timer);
}

// Hide and stop: pausing the timer matters as much as hiding, since a hidden canvas
// would otherwise keep burning a full solver step every frame period
void lcd_anim_fluid_stop(void)
{
    if (s_timer) lv_timer_pause(s_timer);
    if (s_canvas) lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
}

#endif // POLYCAST5_EN_WATER_ANIM
