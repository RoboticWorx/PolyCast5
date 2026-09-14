#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_err.h"

#include "polycast5_macros.h"
#include "gpio_task.h"
#include "lcd_utils.h"
#include "ir_exp_task.h"

#define TAG "LCD_IR_EXP"

/* =============== Layout =============== */
#define IRX_CANVAS_W 180 // 4:3 against the 32x24 sensor, so no aspect distortion
#define IRX_CANVAS_H 135 // Full screen height
#define IRX_PANEL_W  60  // Readout column, never overlaps the image
#define IRX_PANEL_H  115
#define IRX_PANEL_Y  20  // Starts below the battery indicator at the top right

#define IRX_ROW_SPOT_CAP 0  // ink   3..12
#define IRX_ROW_SPOT_C   11 // ink  14..24
#define IRX_ROW_SPOT_F   23 // ink  26..36
#define IRX_ROW_DIST_CAP 42 // ink  45..54  <- 9 px below the SPOT group
#define IRX_ROW_DIST_VAL 53 // ink  56..66
#define IRX_ROW_MED_CAP  72 // ink  75..84  <- 9 px below the DIST group
#define IRX_ROW_MED_C    83 // ink  86..96
#define IRX_ROW_MED_F    95 // ink  98..108, and the 16 px box ends exactly on 111

#define IRX_FRAME_MS 125 // 8 fps; the producer publishes at the same rate

/* =============== Sensor orientation =============== */
#define IRX_FLIP_H false
#define IRX_FLIP_V true

/* =============== Auto-range =============== */
// Values are in 0.02 C units, so 150 is a 3 C floor on the displayed span
#define IRX_MIN_SPAN 150
#define IRX_RANGE_SHIFT 2 // Smoothing: new = old + (measured - old) >> 2

/* =============== Teardown =============== */
// Bounded wait for the producer to park every sensor. Generous: session_stop() can chain three BUS_WAIT_MS bus
// takes, and a half-seated module burns the per-transfer I2C timeout on every write through the teardown.
// Overrunning it costs the user a visit, so the budget sits above the worst case, not the typical one.
#define IRX_STOP_WAIT_MS 3500
#define IRX_START_WAIT_MS 6000 // Probing can retry the bus speed and wait on the ToF to boot

/* =============== Page modes =============== */
enum {
    IRX_MODE_VIEW = 0,
    IRX_MODE_DETAIL,
};

// Four source samples and their Q8 weights for one output pixel
typedef struct {
    int16_t idx[4];
    int16_t w[4];
} irx_tap_t;

static bool irx_init = false;
static lv_obj_t *irx_canvas = NULL;
static lv_obj_t *irx_panel = NULL;
static lv_obj_t *irx_cap[3] = { NULL, NULL, NULL };
static lv_obj_t *irx_val[3] = { NULL, NULL, NULL };
// Fahrenheit line under the two temperature readouts. NULL at index 1, which is DIST.
static lv_obj_t *irx_val_f[3] = { NULL, NULL, NULL };
static lv_obj_t *irx_status = NULL;
static bool irx_status_shown = false; // Whether irx_status currently has text worth showing

// Detail view: everything the rangefinder reports beyond the headline distance. One transparent root parents the
// lot, so hiding it is one delete with no chance of leaking a child.
static lv_obj_t *irx_det_root = NULL;
static lv_obj_t *irx_det_chart = NULL;
static lv_chart_series_t *irx_det_ser = NULL;
static lv_obj_t *irx_det_head = NULL;
static lv_obj_t *irx_det_rates = NULL;
static lv_obj_t *irx_det_row[IR_EXP_TOF_MAX_TARGETS] = { NULL };
static lv_obj_t *irx_det_bar[IR_EXP_TOF_MAX_TARGETS] = { NULL };
static lv_timer_t *irx_timer = NULL;

static uint16_t *irx_fb = NULL;      // RGB565 framebuffer behind the canvas
static uint16_t irx_stride_px = IRX_CANVAS_W;
POLYCAST5_USE_PSRAM_BSS static lv_draw_buf_t irx_draw_buf;

static uint16_t *irx_palette = NULL; // 256 RGB565 entries
static irx_tap_t *irx_tap_x = NULL;  // IRX_CANVAS_W entries
static irx_tap_t *irx_tap_y = NULL;  // IRX_CANVAS_H entries
static int16_t *irx_mid = NULL;      // MLX90642_ROWS x IRX_CANVAS_W intermediate

static uint8_t irx_pal_idx = 0;
static int32_t irx_lo = 0;
static int32_t irx_hi = 0;
static bool irx_range_seeded = false;
static bool irx_frozen = false;
static uint32_t irx_last_seq = 0;

static uint8_t irx_mode = IRX_MODE_VIEW;
static TickType_t irx_pal_show_until = 0; // Hold the palette name on screen after a change

// Raw held button state, declared in gpio_task.c
extern volatile bool gpio_select_btn_held;

// Set when the long semaphore is served, cleared only once the button is physically up.
static bool irx_sel_held = false;

// Defined with the rest of the detail view below; the page timer above needs the declaration
static void irx_detail_tick(void);

static const char *const irx_pal_names[] = { "IRONBOW", "GRAY", "RAINBOW" };
#define IRX_PAL_COUNT (sizeof(irx_pal_names) / sizeof(irx_pal_names[0]))

/* =============== Palettes =============== */

typedef struct {
    uint8_t pos;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} irx_stop_t;

// The classic thermal ramp: black through purple and red into orange, yellow and white
static const irx_stop_t irx_ironbow[] = {
    {   0,   0,   0,   0 },
    {  32,  20,   0,  60 },
    {  64,  70,   0, 110 },
    {  96, 130,   0, 110 },
    { 128, 190,  30,  60 },
    { 160, 230,  90,   0 },
    { 192, 250, 150,   0 },
    { 224, 255, 215,  60 },
    { 255, 255, 255, 255 },
};

static const irx_stop_t irx_rainbow[] = {
    {   0,   0,   0, 140 },
    {  64,   0, 190, 220 },
    { 128,   0, 200,  40 },
    { 192, 245, 220,   0 },
    { 255, 220,   0,   0 },
};

// Expand the control points into a full 256-entry RGB565 lookup, once per palette change, so the per-pixel path
// is a single table read
static void irx_build_palette(uint8_t which)
{
    if (irx_palette == NULL) {
        return;
    }

    if (which == 1) { // Grayscale
        for (int i = 0; i < 256; ++i) {
            irx_palette[i] = lv_color_to_u16(lv_color_make((uint8_t)i, (uint8_t)i, (uint8_t)i));
        }
        return;
    }

    const irx_stop_t *stops = (which == 2) ? irx_rainbow : irx_ironbow;
    const int n = (which == 2) ? (int)(sizeof(irx_rainbow) / sizeof(irx_rainbow[0]))
                               : (int)(sizeof(irx_ironbow) / sizeof(irx_ironbow[0]));

    int seg = 0;
    for (int i = 0; i < 256; ++i) {
        while (seg < n - 2 && i > stops[seg + 1].pos) {
            seg++;
        }
        const irx_stop_t *a = &stops[seg];
        const irx_stop_t *b = &stops[seg + 1];

        const int span = (int)b->pos - (int)a->pos;
        int t = (span > 0) ? (((i - (int)a->pos) * 256) / span) : 0;
        if (t < 0) t = 0;
        if (t > 256) t = 256;

        const uint8_t r = (uint8_t)((int)a->r + ((((int)b->r - (int)a->r) * t) >> 8));
        const uint8_t g = (uint8_t)((int)a->g + ((((int)b->g - (int)a->g) * t) >> 8));
        const uint8_t bl = (uint8_t)((int)a->b + ((((int)b->b - (int)a->b) * t) >> 8));
        irx_palette[i] = lv_color_to_u16(lv_color_make(r, g, bl));
    }
}

/* =============== Interpolation tables =============== */

// Precompute, for every output coordinate, the four source samples it reads and their Catmull-Rom weights in Q8.
// Built once at page entry so the per-frame inner loop is pure integer multiply-accumulate - what makes 8 fps
// affordable without an FPU. Floats appear here and nowhere else in the render path.
//
// Mirroring an axis only reverses the source indices; the weight order stays, as the Catmull-Rom basis is
// symmetric and idx[1]/idx[2] still bracket the output sample. The weight-sum correction below always lands on
// w[1], a different physical tap once reversed, so a mirrored axis is accurate to 1 LSB (0.02 C) rather than
// bit-identical - measured, and an order of magnitude under the sensor's own 0.21 C noise.
static void irx_build_taps(irx_tap_t *taps, int dst_n, int src_n, bool flip)
{
    const float scale = (float)src_n / (float)dst_n;

    for (int d = 0; d < dst_n; ++d) {
        // Map output centre to source space, then split into an index and a fraction
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
            if (si < 0) si = 0; // Clamp at the edges rather than wrap
            if (si > src_n - 1) si = src_n - 1;
            if (flip) si = src_n - 1 - si; // Mirror this axis
            taps[d].idx[k] = (int16_t)si;
            taps[d].w[k] = (int16_t)lrintf(w[k] * 256.0f);
            sum += taps[d].w[k];
        }

        // Round-off can leave the weights summing to 255 or 257, which would tint flat
        // regions. Push the error into the dominant tap so the sum is exactly 256.
        taps[d].w[1] = (int16_t)(taps[d].w[1] + (256 - sum));
    }
}

/* =============== Render =============== */

// Separable Catmull-Rom upscale straight into the RGB565 framebuffer. Pass 1 resamples each of the 24 source rows
// to the full output width; pass 2 resamples down the columns and maps through the palette in the same loop, so no
// full-size intermediate is ever materialised.
//
// Both passes clamp the result to the two samples it sits between: Catmull-Rom overshoots, which shows as a bright
// halo beside a hot object, and bracketing removes it without softening genuine edges.
static void irx_render(const int16_t *src)
{
    // Pass 1: horizontal, 32 -> IRX_CANVAS_W
    for (int r = 0; r < MLX90642_ROWS; ++r) {
        const int16_t *srow = src + r * MLX90642_COLS;
        int16_t *drow = irx_mid + r * IRX_CANVAS_W;

        for (int x = 0; x < IRX_CANVAS_W; ++x) {
            const irx_tap_t *tp = &irx_tap_x[x];
            const int32_t s1 = srow[tp->idx[1]];
            const int32_t s2 = srow[tp->idx[2]];

            int32_t v = ((int32_t)srow[tp->idx[0]] * tp->w[0] + s1 * tp->w[1] +
                         s2 * tp->w[2] + (int32_t)srow[tp->idx[3]] * tp->w[3]) >> 8;

            const int32_t lo = (s1 < s2) ? s1 : s2;
            const int32_t hi = (s1 < s2) ? s2 : s1;
            if (v < lo) v = lo;
            else if (v > hi) v = hi;

            drow[x] = (int16_t)v;
        }
    }

    // Pass 2: vertical, 24 -> IRX_CANVAS_H, plus normalise and palette
    const int32_t span = irx_hi - irx_lo;
    const int32_t recip = (span > 0) ? ((255 << 16) / span) : 0;

    for (int y = 0; y < IRX_CANVAS_H; ++y) {
        const irx_tap_t *tp = &irx_tap_y[y];
        const int16_t *r0 = irx_mid + tp->idx[0] * IRX_CANVAS_W;
        const int16_t *r1 = irx_mid + tp->idx[1] * IRX_CANVAS_W;
        const int16_t *r2 = irx_mid + tp->idx[2] * IRX_CANVAS_W;
        const int16_t *r3 = irx_mid + tp->idx[3] * IRX_CANVAS_W;
        uint16_t *out = irx_fb + y * irx_stride_px;

        for (int x = 0; x < IRX_CANVAS_W; ++x) {
            const int32_t s1 = r1[x];
            const int32_t s2 = r2[x];

            int32_t v = ((int32_t)r0[x] * tp->w[0] + s1 * tp->w[1] +
                         s2 * tp->w[2] + (int32_t)r3[x] * tp->w[3]) >> 8;

            const int32_t lo = (s1 < s2) ? s1 : s2;
            const int32_t hi = (s1 < s2) ? s2 : s1;
            if (v < lo) v = lo;
            else if (v > hi) v = hi;

            // Clamp into the displayed span BEFORE scaling. recip can reach (255 << 16) / IRX_MIN_SPAN, so an
            // out-of-range sample would overflow the multiply - signed overflow, not a wrong colour. Bounding
            // the difference first caps the product at 255 << 16 by construction.
            int32_t d = v - irx_lo;
            if (d < 0) d = 0;
            else if (d > span) d = span;

            int32_t n = (d * recip) >> 16;
            if (n > 255) n = 255;

            out[x] = irx_palette[n];
        }
    }
}

// Single pixel write, indexed by the draw buffer's stride rather than the canvas width: LVGL is free to pad rows.
static inline void out_px(int y, int x, uint16_t col)
{
    irx_fb[y * irx_stride_px + x] = col;
}

// Centre reticle. A dark pixel either side of each white core arm keeps it legible over black, white and every
// palette colour in between.
static void irx_draw_crosshair(void)
{
    const int cx = IRX_CANVAS_W / 2;
    const int cy = IRX_CANVAS_H / 2;
    const uint16_t fg = lv_color_to_u16(lv_color_white());
    const uint16_t bg = lv_color_to_u16(lv_color_black());

    for (int d = 2; d <= 7; ++d) { // Leave a 2 px gap so the centre stays visible
        for (int s = -1; s <= 1; ++s) {
            const uint16_t col = (s == 0) ? fg : bg;

            const int yy = cy + s;
            if (yy >= 0 && yy < IRX_CANVAS_H) {
                if (cx - d >= 0) out_px(yy, cx - d, col);
                if (cx + d < IRX_CANVAS_W) out_px(yy, cx + d, col);
            }

            const int xx = cx + s;
            if (xx >= 0 && xx < IRX_CANVAS_W) {
                if (cy - d >= 0) out_px(cy - d, xx, col);
                if (cy + d < IRX_CANVAS_H) out_px(cy + d, xx, col);
            }
        }
    }
}

/* =============== Readout formatting =============== */

// One decimal place with the unit appended. Takes hundredths of whichever unit it is printing, so Fahrenheit shares it.
static void irx_fmt_unit(char *buf, size_t n, int32_t v100, char unit)
{
    // Sign is handled explicitly: integer division would drop it for values like -0.5 C
    const char *sign = (v100 < 0) ? "-" : "";
    const int32_t v = (v100 < 0) ? -v100 : v100;
    snprintf(buf, n, "%s%d.%d%c", sign, (int)(v / 100), (int)((v % 100) / 10), unit);
}

static void irx_fmt_temp(char *buf, size_t n, int32_t c100, bool valid)
{
    if (!valid) {
        snprintf(buf, n, "--");
        return;
    }
    irx_fmt_unit(buf, n, c100, 'C');
}

// Same reading in Fahrenheit, for the second line under each temperature. Integer hundredths - the C5 has no FPU
// and this runs on every published frame. Truncation toward zero costs at most 0.01 F, under the sensor's accuracy.
static void irx_fmt_temp_f(char *buf, size_t n, int32_t c100, bool valid)
{
    if (!valid) {
        snprintf(buf, n, "--");
        return;
    }
    irx_fmt_unit(buf, n, ((c100 * 9) / 5) + 3200, 'F');
}

static void irx_fmt_dist(char *buf, size_t n, int32_t mm, bool valid)
{
    if (!valid || mm == VL53L4CX_NO_TARGET) {
        snprintf(buf, n, "--");
        return;
    }

    // Centimetres, millimetre digit kept below a metre and dropped above it. "300.0cm" is 62 px in montserrat_14
    // and the panel only has 56; and past 110 mm the datasheet's accuracy is a percentage (3% white, 5% gray), so
    // at a metre the reading is +-30..50 mm and a tenth-of-a-millimetre digit invents precision.
    if (mm < 1000) {
        snprintf(buf, n, "%d.%dcm", (int)(mm / 10), (int)(mm % 10));
    } else {
        snprintf(buf, n, "%dcm", (int)((mm + 5) / 10)); // Round, not truncate, at 1 cm
    }
}

/* =============== Allocation =============== */

static void irx_free(void)
{
    heap_caps_free(irx_fb);
    heap_caps_free(irx_palette);
    heap_caps_free(irx_tap_x);
    heap_caps_free(irx_tap_y);
    heap_caps_free(irx_mid);
    irx_fb = NULL;
    irx_palette = NULL;
    irx_tap_x = NULL;
    irx_tap_y = NULL;
    irx_mid = NULL;
}

static bool irx_alloc(void)
{
    const size_t fb_bytes = (size_t)IRX_CANVAS_W * IRX_CANVAS_H * 2;

    irx_fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    irx_palette = heap_caps_malloc(256 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    irx_tap_x = heap_caps_malloc(IRX_CANVAS_W * sizeof(irx_tap_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    irx_tap_y = heap_caps_malloc(IRX_CANVAS_H * sizeof(irx_tap_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    irx_mid = heap_caps_malloc((size_t)MLX90642_ROWS * IRX_CANVAS_W * sizeof(int16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (irx_fb == NULL || irx_palette == NULL || irx_tap_x == NULL ||
            irx_tap_y == NULL || irx_mid == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM for the thermal view");
        irx_free();
        return false;
    }

    memset(irx_fb, 0, fb_bytes);
    irx_build_taps(irx_tap_x, IRX_CANVAS_W, MLX90642_COLS, IRX_FLIP_H);
    irx_build_taps(irx_tap_y, IRX_CANVAS_H, MLX90642_ROWS, IRX_FLIP_V);
    irx_build_palette(irx_pal_idx);
    return true;
}

/* =============== Live update =============== */

// Driven by an lv_timer at IRX_FRAME_MS, not by the page handler: lcd_task calls page handlers only every 200 ms,
// far too coarse for live video.
// Set the corner status text, or hide the label when there is none.
//
// Blanking the text is not enough: the label keeps its 2 px padding and its 50%-black background, so
// an empty one still paints a small dark square over the bottom-left of the thermal image. Visibility
// is tracked so irx_hide_detail() can restore the label without having to re-derive what it should say.
static void irx_set_status(const char *txt)
{
    if (irx_status == NULL) {
        return;
    }

    if (txt == NULL || txt[0] == '\0') {
        lv_obj_add_flag(irx_status, LV_OBJ_FLAG_HIDDEN);
        irx_status_shown = false;
        return;
    }

    lv_label_set_text(irx_status, txt);
    lv_obj_remove_flag(irx_status, LV_OBJ_FLAG_HIDDEN);
    irx_status_shown = true;
}

static void irx_timer_cb(lv_timer_t *t)
{
    (void)t;

    // The sleep path calls lv_timer_handler() from its wait loop, so guard against firing after teardown freed the buffer.
    if (!irx_init || irx_canvas == NULL || irx_fb == NULL) {
        return;
    }
    if (irx_mode == IRX_MODE_DETAIL) {
        irx_detail_tick(); // Reads its own queue; the thermal path below is hidden anyway
        return;
    }
    ir_exp_status_t st;
    if (xIrExpStatusQueue == NULL || xQueuePeek(xIrExpStatusQueue, &st, 0) != pdTRUE) {
        return;
    }
    if (st.seq == irx_last_seq) { // Nothing new since the last redraw
        return;
    }
    irx_last_seq = st.seq;

    // Readouts refresh while the array warms up, but not while held: freezing exists to read the frozen frame's numbers
    char buf[16];
    const bool img_live = (st.flags & IR_EXP_FLAG_IMAGER) && !(st.flags & IR_EXP_FLAG_WARMING);

    if (!irx_frozen) {
        const int32_t spot_c100 = (int32_t)st.center_c50 * 2; // 0.02 C -> 0.01 C
        irx_fmt_temp(buf, sizeof(buf), spot_c100, img_live);
        lv_label_set_text(irx_val[0], buf);
        irx_fmt_temp_f(buf, sizeof(buf), spot_c100, img_live);
        lv_label_set_text(irx_val_f[0], buf);

        irx_fmt_dist(buf, sizeof(buf), st.dist_mm, (st.flags & IR_EXP_FLAG_TOF) != 0);
        lv_label_set_text(irx_val[1], buf);

        const bool med_live = (st.flags & IR_EXP_FLAG_MEDICAL) &&
                st.medical_c100 != IR_EXP_MEDICAL_NONE;
        irx_fmt_temp(buf, sizeof(buf), st.medical_c100, med_live);
        lv_label_set_text(irx_val[2], buf);
        irx_fmt_temp_f(buf, sizeof(buf), st.medical_c100, med_live);
        lv_label_set_text(irx_val_f[2], buf);
    }

    if ((int32_t)(xTaskGetTickCount() - irx_pal_show_until) < 0) {
        // A palette name is being shown; leave it up
    } else if (irx_frozen) {
        irx_set_status("HOLD");
    } else if (st.flags & IR_EXP_FLAG_FAULT) {
        // Ahead of WARMING: a module that stopped answering is not warming up, and its readouts are already blank
        irx_set_status("NO SIGNAL");
    } else if (st.flags & IR_EXP_FLAG_WARMING) {
        irx_set_status("WARMING UP");
    } else if (st.flags & IR_EXP_FLAG_STABILIZING) {
        irx_set_status("STABILIZING"); // Absolute accuracy still settling
    } else {
        irx_set_status("");
    }

    if (irx_frozen || !img_live) { // Keep the last image on screen
        return;
    }

    const int16_t *frame = ir_exp_get_frame(st.buf_idx);
    if (frame == NULL) {
        return;
    }

    // Auto-range: hold a minimum span so a flat scene does not amplify noise, then smooth both ends against flicker
    int32_t lo = st.min_c50;
    int32_t hi = st.max_c50;
    if (hi - lo < IRX_MIN_SPAN) {
        const int32_t mid = (hi + lo) / 2;
        lo = mid - IRX_MIN_SPAN / 2;
        hi = mid + IRX_MIN_SPAN / 2;
    }

    if (!irx_range_seeded) {
        irx_lo = lo;
        irx_hi = hi;
        irx_range_seeded = true;
    } else {
        irx_lo += (lo - irx_lo) >> IRX_RANGE_SHIFT;
        irx_hi += (hi - irx_hi) >> IRX_RANGE_SHIFT;
        if (irx_hi - irx_lo < IRX_MIN_SPAN) {
            irx_hi = irx_lo + IRX_MIN_SPAN;
        }
    }

    irx_render(frame);
    irx_draw_crosshair();
    lv_obj_invalidate(irx_canvas);
}

/* =============== UI construction =============== */

static bool irx_failed = false;   // Module missing: the page shows a card and waits
static bool irx_have_tof = false; // Rangefinder answered, so the detail view has data
static lv_obj_t *irx_card = NULL;
static lv_obj_t *irx_card_title = NULL;
static lv_obj_t *irx_card_body = NULL;

// Plain message card, styled to match the other GPIO pages
static void irx_make_card(const char *title, const char *body)
{
    irx_card = lv_obj_create(ACTIVE_SCR);
    lv_obj_set_size(irx_card, 240, 135);
    lv_obj_center(irx_card);
    lv_obj_set_style_bg_color(irx_card, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(irx_card, LV_OBJ_FLAG_SCROLLABLE);

    irx_card_title = lv_label_create(irx_card);
    lv_label_set_text(irx_card_title, title);
    lv_obj_set_style_text_font(irx_card_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(irx_card_title, user_secondary_color, 0);
    lv_obj_align(irx_card_title, LV_ALIGN_TOP_MID, 0, 0);

    irx_card_body = lv_label_create(irx_card);
    lv_label_set_long_mode(irx_card_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(irx_card_body, lv_pct(100));
    lv_label_set_text(irx_card_body, body);
    lv_obj_set_style_text_font(irx_card_body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(irx_card_body, user_secondary_color, 0);
    lv_obj_set_style_text_align(irx_card_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(irx_card_body, irx_card_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
}

static void irx_drop_card(void)
{
    if (irx_card != NULL) {
        lv_obj_delete(irx_card); // Deletes its children
        irx_card = NULL;
        irx_card_title = NULL;
        irx_card_body = NULL;
    }
}

static void irx_build_ui(void)
{
    const size_t fb_bytes = (size_t)IRX_CANVAS_W * IRX_CANVAS_H * 2;

    lv_draw_buf_init(&irx_draw_buf, IRX_CANVAS_W, IRX_CANVAS_H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO, irx_fb, fb_bytes);
    // LVGL may pad rows, so every pixel write indexes by stride rather than width
    irx_stride_px = (uint16_t)(irx_draw_buf.header.stride / 2);
    if (irx_stride_px == 0) { // Defensive: a zero stride would fold every row onto row 0
        irx_stride_px = IRX_CANVAS_W;
    }

    irx_canvas = lv_canvas_create(ACTIVE_SCR);
    lv_canvas_set_draw_buf(irx_canvas, &irx_draw_buf);
    lv_obj_set_size(irx_canvas, IRX_CANVAS_W, IRX_CANVAS_H);
    lv_obj_align(irx_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    // Readout column. Offset down the screen so the battery indicator stays visible.
    irx_panel = lv_obj_create(ACTIVE_SCR);
    lv_obj_set_size(irx_panel, IRX_PANEL_W, IRX_PANEL_H);
    lv_obj_align(irx_panel, LV_ALIGN_TOP_RIGHT, 0, IRX_PANEL_Y);
    lv_obj_set_style_bg_color(irx_panel, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(irx_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(irx_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(irx_panel, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(irx_panel, LV_OBJ_FLAG_SCROLLABLE);

    static const char *const caps[3] = { "SPOT", "DIST", "MED" };
    static const int16_t cap_y[3] = { IRX_ROW_SPOT_CAP, IRX_ROW_DIST_CAP, IRX_ROW_MED_CAP };
    static const int16_t val_y[3] = { IRX_ROW_SPOT_C, IRX_ROW_DIST_VAL, IRX_ROW_MED_C };
    static const int16_t f_y[3] = { IRX_ROW_SPOT_F, -1, IRX_ROW_MED_F }; // -1: no second unit

    for (int i = 0; i < 3; ++i) {
        irx_cap[i] = lv_label_create(irx_panel);
        lv_label_set_text(irx_cap[i], caps[i]);
        lv_obj_set_style_text_font(irx_cap[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(irx_cap[i], user_secondary_color, 0);
        lv_obj_set_style_opa(irx_cap[i], LV_OPA_60, 0); // Dimmer than the value it labels
        lv_obj_align(irx_cap[i], LV_ALIGN_TOP_LEFT, 0, cap_y[i]);

        irx_val[i] = lv_label_create(irx_panel);
        lv_label_set_text(irx_val[i], "--");
        lv_obj_set_style_text_font(irx_val[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(irx_val[i], user_secondary_color, 0);
        lv_obj_align(irx_val[i], LV_ALIGN_TOP_LEFT, 0, val_y[i]);

        // Fahrenheit under the Celsius in the same font and colour - same reading, so neither unit is subordinate.
        // The 2 px row gap against the 9 px group gap is what says the two lines belong together.
        irx_val_f[i] = NULL;
        if (f_y[i] >= 0) {
            irx_val_f[i] = lv_label_create(irx_panel);
            lv_label_set_text(irx_val_f[i], "--");
            lv_obj_set_style_text_font(irx_val_f[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(irx_val_f[i], user_secondary_color, 0);
            lv_obj_align(irx_val_f[i], LV_ALIGN_TOP_LEFT, 0, f_y[i]);
        }
    }

    irx_status = lv_label_create(ACTIVE_SCR);
    lv_label_set_text(irx_status, "");
    lv_obj_set_style_text_font(irx_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(irx_status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(irx_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(irx_status, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(irx_status, 2, 0);
    lv_obj_align(irx_status, LV_ALIGN_BOTTOM_LEFT, 2, -2);

    // Born hidden to match irx_status_shown: the padding and translucent background would otherwise
    // paint the dark square over the corner until the first timer tick set a state.
    lv_obj_add_flag(irx_status, LV_OBJ_FLAG_HIDDEN);
    irx_status_shown = false;
}

/* =============== Detail view =============== */

// Everything the rangefinder publishes beyond the one distance the tape measure shows: the raw 24-bin
// time-of-flight histogram, every target the histogram pass resolved, the return and ambient rates, and the part's
// own error bars. The live view is HIDDEN rather than torn down while this is up: irx_fb is 48 kB of PSRAM and
// rebuilding the interpolation tables is not free, so a detour here and back costs a couple of flag flips.

// Vertical budget, and it is exact - 135 px of screen with nothing to spare:
//    0..13   header        (targets / SPADs / frames)
//   14..47   histogram     IRX_DET_H_CHART
//   48..60   axis hint     close .. far, so the plot reads as a distance axis
//   61..75   rates         ambient and signal
//   76..133  four target rows at IRX_DET_ROW_H
//
// Work in INK, not line boxes, as the side panel at the top of this file does: montserrat_12 sits in a 15 px box
// but inks only rows 3..12, so a row's visible height is 10 px and what the eye judges is the blank between ink
// blocks. Hence the uneven spacing: 3 px chart to axis hint and hint to rates (one supporting block), 5 px rates
// to the first reading, 5 px between readings from the 15 px row pitch. Do not pack them all at 3 px - it fits,
// but reads as one undifferentiated block with the rates line looking like a fifth reading; the 8 px came out of
// the histogram, whose bars rescale to their own peak. The last row's box overruns the screen by 2 px, invisibly:
// label backgrounds are transparent and its ink ends at 133.
#define IRX_DET_H_CHART 34 // Histogram plot height
#define IRX_DET_Y_HINT  48 // "close .. far" under the plot
#define IRX_DET_Y_RATES 61
#define IRX_DET_Y_ROW0  76
#define IRX_DET_ROW_H   15 // Row pitch, not the line box - see above

// ST's range statuses in words. Only 0 is worth believing; the rest say why the part threw a reading away.
static const char *irx_status_name(uint8_t st)
{
    switch (st) {
    case 0:  return "OK";
    case 1:  return "NOISY";   // Sigma above threshold - smeared return
    case 2:  return "WEAK";    // Signal below threshold
    case 4:  return "OUTSIDE"; // Phase out of the valid window
    case 7:  return "WRAPPED"; // Aliased - no matching phase at the other VCSEL period
    case 8:  return "CALC";    // Internal processing failure
    case 14: return "BAD";     // Negative range
    default: return "OTHER";
    }
}

static void irx_hide_detail(void)
{
    if (irx_det_root != NULL) {
        lv_obj_delete(irx_det_root); // Takes every child with it
        irx_det_root = NULL;
        irx_det_chart = NULL;
        irx_det_ser = NULL;
        irx_det_head = NULL;
        irx_det_rates = NULL;
        for (int i = 0; i < IR_EXP_TOF_MAX_TARGETS; ++i) {
            irx_det_row[i] = NULL;
            irx_det_bar[i] = NULL;
        }
    }

    if (irx_canvas != NULL) lv_obj_remove_flag(irx_canvas, LV_OBJ_FLAG_HIDDEN);
    if (irx_panel != NULL) lv_obj_remove_flag(irx_panel, LV_OBJ_FLAG_HIDDEN);
    // Only if it had text: unhiding an empty one would paint the dark square for a frame until the
    // next timer tick blanked it again.
    if (irx_status != NULL && irx_status_shown) lv_obj_remove_flag(irx_status, LV_OBJ_FLAG_HIDDEN);
}

static void irx_show_detail(void)
{
    if (irx_det_root != NULL) {
        return; // Idempotent: re-entering the detail view must not rebuild it
    }

    // Hide, never delete - see the note above.
    if (irx_canvas != NULL) lv_obj_add_flag(irx_canvas, LV_OBJ_FLAG_HIDDEN);
    if (irx_panel != NULL) lv_obj_add_flag(irx_panel, LV_OBJ_FLAG_HIDDEN);
    if (irx_status != NULL) lv_obj_add_flag(irx_status, LV_OBJ_FLAG_HIDDEN);

    irx_det_root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(irx_det_root);
    lv_obj_set_size(irx_det_root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(irx_det_root, 0, 0);
    lv_obj_set_style_bg_color(irx_det_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(irx_det_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(irx_det_root, LV_OBJ_FLAG_SCROLLABLE);

    irx_det_head = lv_label_create(irx_det_root);
    lv_obj_set_style_text_font(irx_det_head, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(irx_det_head, lv_color_white(), 0);
    lv_obj_set_pos(irx_det_head, 2, 0);
    lv_label_set_text(irx_det_head, "");

    // Histogram, sized for 24 bins across the full 240 px: 8 px bars would run off screen, so only the gap is set
    irx_det_chart = lv_chart_create(irx_det_root);
    lv_obj_set_size(irx_det_chart, 232, IRX_DET_H_CHART);
    lv_obj_set_pos(irx_det_chart, 4, 14);
    lv_obj_set_style_bg_color(irx_det_chart, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(irx_det_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(irx_det_chart, lv_palette_darken(LV_PALETTE_GREY, 3),
            LV_PART_MAIN);
    lv_obj_set_style_pad_all(irx_det_chart, 2, LV_PART_MAIN);
    lv_chart_set_type(irx_det_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(irx_det_chart, IR_EXP_TOF_HIST_BINS);
    lv_chart_set_div_line_count(irx_det_chart, 0, 0);
    lv_obj_set_style_pad_column(irx_det_chart, 1, LV_PART_MAIN);
    irx_det_ser = lv_chart_add_series(irx_det_chart,
            lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_bg_color(irx_det_chart, lv_palette_main(LV_PALETTE_GREEN),
            LV_PART_ITEMS);

    // Tells the reader the plot is a distance axis rather than an arbitrary bar chart. Static, so set once here.
    lv_obj_t *hint = lv_label_create(irx_det_root);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_palette_darken(LV_PALETTE_GREY, 1), 0);
    lv_obj_set_pos(hint, 4, IRX_DET_Y_HINT);
    lv_label_set_text(hint, "close" LV_SYMBOL_RIGHT "                      far");

    irx_det_rates = lv_label_create(irx_det_root);
    lv_obj_set_style_text_font(irx_det_rates, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(irx_det_rates, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_pos(irx_det_rates, 2, IRX_DET_Y_RATES);
    lv_label_set_text(irx_det_rates, "");

    // One row per possible target: text on the left, a range bar on the right carrying the part's own min..max.
    for (int i = 0; i < IR_EXP_TOF_MAX_TARGETS; ++i) {
        const int y = IRX_DET_Y_ROW0 + (i * IRX_DET_ROW_H);

        irx_det_row[i] = lv_label_create(irx_det_root);
        lv_obj_set_style_text_font(irx_det_row[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(irx_det_row[i], lv_color_white(), 0);
        lv_obj_set_pos(irx_det_row[i], 2, y);
        lv_label_set_text(irx_det_row[i], "");

        irx_det_bar[i] = lv_bar_create(irx_det_root);
        lv_obj_set_size(irx_det_bar[i], 48, 5);
        // Centred on the row's ink (rows 3..12 of the box), not on the box itself
        lv_obj_set_pos(irx_det_bar[i], 188, y + 5);
        lv_bar_set_mode(irx_det_bar[i], LV_BAR_MODE_RANGE);
        lv_obj_set_style_bg_color(irx_det_bar[i],
                lv_palette_darken(LV_PALETTE_GREY, 3), LV_PART_MAIN);
        lv_obj_set_style_bg_color(irx_det_bar[i],
                lv_palette_main(LV_PALETTE_CYAN), LV_PART_INDICATOR);
        lv_obj_add_flag(irx_det_bar[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Repaint the detail view from one published snapshot. Called from the page timer, so it must be cheap and must tolerate no data
static void irx_detail_tick(void)
{
    if (irx_det_root == NULL || xIrExpTofDetailQueue == NULL) {
        return;
    }

    // The snapshot queue is written only on a successful read, so once the rangefinder stops answering it keeps
    // handing back the last one. The status flag is the authority on whether it still describes anything.
    ir_exp_status_t sst;
    if (xIrExpStatusQueue != NULL && xQueuePeek(xIrExpStatusQueue, &sst, 0) == pdTRUE
            && !(sst.flags & IR_EXP_FLAG_TOF)) {
        if (irx_det_head != NULL) {
            lv_label_set_text(irx_det_head, "Rangefinder not responding");
        }
        if (irx_det_rates != NULL) {
            lv_label_set_text(irx_det_rates, "");
        }
        if (irx_det_chart != NULL && irx_det_ser != NULL) {
            int32_t *y = lv_chart_get_y_array(irx_det_chart, irx_det_ser);
            for (int i = 0; i < IR_EXP_TOF_HIST_BINS; ++i) {
                y[i] = 0;
            }
            lv_chart_refresh(irx_det_chart);
        }
        for (int i = 0; i < IR_EXP_TOF_MAX_TARGETS; ++i) {
            if (irx_det_row[i] != NULL) {
                lv_label_set_text(irx_det_row[i], "");
            }
            if (irx_det_bar[i] != NULL) {
                lv_obj_add_flag(irx_det_bar[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    static ir_exp_tof_detail_t d; // 196 B; static keeps it off lcd_task's stack
    if (xQueuePeek(xIrExpTofDetailQueue, &d, 0) != pdTRUE) {
        return;
    }

    // Scale to this frame's own peak so a weak return still fills the plot; absolute counts mean nothing, the SHAPE does
    if (irx_det_chart != NULL && irx_det_ser != NULL) {
        const int32_t top = (d.bin_max > 0) ? d.bin_max : 1;
        lv_chart_set_range(irx_det_chart, LV_CHART_AXIS_PRIMARY_Y, 0, top);
        int32_t *y = lv_chart_get_y_array(irx_det_chart, irx_det_ser);
        for (int i = 0; i < IR_EXP_TOF_HIST_BINS; ++i) {
            y[i] = d.bin[i];
        }
        lv_chart_refresh(irx_det_chart);
    }

    if (irx_det_head != NULL) {
        char h[64];
        snprintf(h, sizeof(h), "Found: %u    Pixels: %u    Avg: %u",
                (unsigned)d.n_targets, (unsigned)d.spads, (unsigned)d.merge_nb);
        lv_label_set_text(irx_det_head, h);
    }

    if (irx_det_rates != NULL) {
        // Ambient is the same for every target, so it belongs here rather than per row. The signal shown is the
        // CHOSEN target's: a crosstalk ghost holds its signal steady while a real return falls off as 1/distance^2.
        const uint32_t sig = (d.chosen >= 0) ? d.target[d.chosen].signal_kcps : 0u;
        char r[64];
        snprintf(r, sizeof(r), "Light: %lu    Return: %lu",
                (unsigned long)(d.n_targets ? d.target[0].ambient_kcps : 0u),
                (unsigned long)sig);
        lv_label_set_text(irx_det_rates, r);
    }

    for (int i = 0; i < IR_EXP_TOF_MAX_TARGETS; ++i) {
        if (irx_det_row[i] == NULL) {
            continue;
        }
        if (i >= d.n_targets) {
            lv_label_set_text(irx_det_row[i], "");
            lv_obj_add_flag(irx_det_bar[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const ir_exp_tof_target_t *t = &d.target[i];
        char row[64];
        // A leading arrow marks the one the tape measure reported, so a rejected crosstalk ghost at index 0 is
        // visibly NOT the chosen reading. A good reading gets its error bounds, a rejected one the reason instead:
        // "WRAPPED" answers the user's question and "+/-12mm" on a discarded reading does not.
        if (t->status == 0) {
            const int err = (t->max_mm > t->min_mm)
                    ? (int)((t->max_mm - t->min_mm) / 2) : 0;
            snprintf(row, sizeof(row), "%sDist %4d mm   +/-%d mm",
                    (i == d.chosen) ? ">" : " ", (int)t->dist_mm, err);
        } else {
            snprintf(row, sizeof(row), " Dist %4d mm   %s",
                    (int)t->dist_mm, irx_status_name(t->status));
        }
        lv_label_set_text(irx_det_row[i], row);
        lv_obj_set_style_text_color(irx_det_row[i],
                (i == d.chosen) ? lv_palette_main(LV_PALETTE_GREEN)
                : (t->status != 0) ? lv_palette_darken(LV_PALETTE_GREY, 1)
                : lv_color_white(), 0);

        // Error bar: the part's own min..max, against a window that keeps a tight reading visibly tight
        if (t->status == 0 && t->max_mm > t->min_mm) {
            const int32_t span = 200; // +-100 mm of context around the reading
            const int32_t lo = (int32_t)t->dist_mm - span / 2;
            int32_t a = (int32_t)t->min_mm - lo;
            int32_t b = (int32_t)t->max_mm - lo;
            if (a < 0) a = 0;
            if (b > span) b = span;
            lv_bar_set_range(irx_det_bar[i], 0, span);
            lv_bar_set_start_value(irx_det_bar[i], a, LV_ANIM_OFF);
            lv_bar_set_value(irx_det_bar[i], b, LV_ANIM_OFF);
            lv_obj_remove_flag(irx_det_bar[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(irx_det_bar[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* =============== Session lifecycle =============== */

// Wait for the acknowledgement belonging to one specific command. Every wait here is bounded, so a producer that
// overruns its budget leaves its acknowledgement behind; taking the next thing off the queue would let a late
// START ack satisfy the STOP handshake - and that handshake is all that stands between lcd_device_sleep() taking
// the bus with portMAX_DELAY and a producer halfway through parking its sensors. ir_exp_done_t.cmd exists for it.
static bool irx_wait_done(uint8_t want_cmd, uint32_t timeout_ms, ir_exp_done_t *done)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - deadline) >= 0) {
            return false;
        }

        ir_exp_done_t got;
        if (xQueueReceive(xIrExpDoneQueue, &got, deadline - now) != pdTRUE) {
            return false;
        }
        if (got.cmd == want_cmd) {
            if (done != NULL) {
                *done = got;
            }
            return true;
        }
        // Anything else is an orphan from a command this page already gave up on: drop it
    }
}

// Not just a bool: a start that never came back is NOT the same as a start that came back saying there is no
// module. The producer may be mid bring-up and about to stream, so the page has to stop it rather than tell the
// user to check the seating of a module that is present and working.
typedef enum {
    IRX_START_OK = 0,
    IRX_START_NO_MODULE,  // Producer answered: the array is not there
    IRX_START_NO_MEM,     // Producer answered: array present, its buffers would not allocate
    IRX_START_NO_ANSWER,  // Producer never acknowledged, or could not be asked
} irx_start_result_t;

static irx_start_result_t irx_session_start(ir_exp_done_t *done)
{
    memset(done, 0, sizeof(*done));

    // FIRST, before anything is sent: the producer exists only while this page is open, and this call creates the
    // queues as well as the task, so testing the queue handles ahead of it fails every first visit. Branch on its
    // return value, not ir_exp_task_is_running(): a wedged producer from a previous visit reads true while the
    // queues are NOT clean and nothing will read what we send.
    if (!ir_exp_task_create()) {
        // No heap for the queues or the 8 kB stack, or a previous teardown never finished. Reported here rather
        // than by queueing a START nothing will service and waiting out IRX_START_WAIT_MS with LVGL unserviced.
        ESP_LOGE(TAG, "Could not bring up the expansion producer");
        return IRX_START_NO_ANSWER;
    }

    ir_exp_cmd_t cmd = { .type = IR_EXP_CMD_START, .arg = 0 };
    if (xQueueSend(xIrExpCtrlQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return IRX_START_NO_ANSWER;
    }
    if (!irx_wait_done(IR_EXP_CMD_START, IRX_START_WAIT_MS, done)) {
        ESP_LOGE(TAG, "Expansion did not answer the start request");
        return IRX_START_NO_ANSWER;
    }
    if (done->ok) {
        return IRX_START_OK;
    }
    return done->no_mem ? IRX_START_NO_MEM : IRX_START_NO_MODULE;
}

// Teardown MUST block on the acknowledgement. lcd_device_sleep() takes xI2CBusMutex with portMAX_DELAY and keeps
// it for the entire sleep, so returning before the producer has parked the sensors leaves it blocked on the bus
// forever - with the thermal array still drawing 28 mA while the device is supposedly asleep.
static void irx_session_stop(void)
{
    if (xIrExpCtrlQueue == NULL || xIrExpDoneQueue == NULL) {
        return;
    }

    // Idempotent, and it has to be: the failed-start path stops the producer and then the key that dismisses its
    // error card runs irx_cleanup(), which stops it again. The task frees itself once a STOP is acknowledged, so a
    // second STOP would block for the full IRX_STOP_WAIT_MS with nobody to answer it and then sit unread on the
    // command queue - where the NEXT visit's task dequeues it first and tears that session down before its START.
    if (!ir_exp_task_is_running()) {
        return;
    }

    // Drained for the same reason irx_session_start() drains: irx_wait_done() matches on the command id alone, so
    // a STOP acknowledgement left behind by a previous visit would satisfy this wait before anything was parked.
    ir_exp_done_t stale;
    while (xQueueReceive(xIrExpDoneQueue, &stale, 0) == pdTRUE) {
        // Drop it
    }

    ir_exp_cmd_t cmd = { .type = IR_EXP_CMD_STOP, .arg = 0 };
    if (xQueueSend(xIrExpCtrlQueue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Could not queue the expansion stop request");
        return;
    }

    if (!irx_wait_done(IR_EXP_CMD_STOP, IRX_STOP_WAIT_MS, NULL)) {
        ESP_LOGE(TAG, "Timed out waiting for the expansion to park its sensors");
    }
}

// Stop the producer, delete the timer (never merely pause it - the sleep path runs lv_timer_handler from inside
// its wait loop), delete the objects, free the buffers, and clear the entry gate so a re-entry rebuilds.
static void irx_cleanup(void)
{
    irx_session_stop();

    if (irx_timer != NULL) {
        lv_timer_del(irx_timer);
        irx_timer = NULL;
    }

    irx_hide_detail();
    irx_drop_card();

    if (irx_panel != NULL) {
        lv_obj_delete(irx_panel); // Deletes the caption and value labels with it
        irx_panel = NULL;
    }
    if (irx_status != NULL) {
        lv_obj_delete(irx_status);
        irx_status = NULL;
    }
    irx_status_shown = false;
    if (irx_canvas != NULL) {
        lv_obj_delete(irx_canvas);
        irx_canvas = NULL;
    }
    memset(irx_cap, 0, sizeof(irx_cap));
    memset(irx_val, 0, sizeof(irx_val));
    memset(irx_val_f, 0, sizeof(irx_val_f)); // Deleted with irx_panel, just drop the handles

    irx_free();

    irx_init = false;
    irx_failed = false;
    irx_have_tof = false;
    irx_mode = IRX_MODE_VIEW;
    irx_frozen = false;
    irx_range_seeded = false;
    irx_last_seq = 0;
}

/* =============== Page handler =============== */

void lcd_gpio_ir_exp_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, gpio_menu_t *gpio_menu)
{
    // First entry: probe the module, then build the view
    if (!irx_init && !irx_failed) {
        // If picking this page as a hotkey
        if (!lv_obj_has_flag(ui_menu->lbl_hotkey_icon, LV_OBJ_FLAG_HIDDEN)) {
            lcd_hotkey_save_page_as_hotkey(ui_menu);
        }

        // Probing can retry the bus speed and wait on the rangefinder's firmware, blocking lcd_task for seconds - paint first
        irx_make_card("IR Expansion", "\nDetecting module...");
        lv_timer_handler();

        ir_exp_done_t done;
        const irx_start_result_t sr = irx_session_start(&done);

        if (sr != IRX_START_OK) {
            // Unconditional, including the paths where the producer reported failure and so never set running:
            // STOP is acknowledged immediately and touches no hardware when nothing runs (see IR_EXP_CMD_STOP).
            // What it buys is IRX_START_NO_ANSWER, where a bring-up may still be finishing behind us.
            irx_session_stop();
            lv_label_set_text(irx_card_body,
                    sr == IRX_START_NO_MEM
                            ? "Out of memory.\n\nPress any key."
                    : sr == IRX_START_NO_ANSWER
                            ? "Expansion didn't respond.\n\nPress any key."
                            : "Module not detected.\n\nGet one at polycast5.com!\nPress any key.");
            irx_failed = true;

            // The bring-up above blocked lcd_task for seconds, so any button pressed while it ran is still
            // latched and would arrive on the very next pass, dismissing this card before it had been read.
            lcd_clear_pending_inputs = true;
            return;
        }

        irx_drop_card();

        if (!irx_alloc()) {
            // The producer IS streaming here - it acknowledged a good start - and nothing will consume it
            irx_session_stop();
            irx_make_card("IR Expansion", "Out of memory.\n\nPress any key.");
            irx_failed = true;
            return;
        }

        irx_have_tof = done.tof;
        irx_build_ui();
        irx_timer = lv_timer_create(irx_timer_cb, IRX_FRAME_MS, NULL);
        irx_init = true;

        // The SELECT press that opened this page may still be down, and the menu behind may already have armed
        // the long semaphore. Drop both, or the first pass latches a hold nothing releases and eats a real press.
        if (xSelectButtonLongSemaphore != NULL) {
            xQueueReset(xSelectButtonLongSemaphore);
        }
        irx_sel_held = false;

        // Same reason as the failure path above, and it matters most on the hotkey route: a long-press that jumps
        // straight here leaves the button down, gpio_task starts auto-repeating shorts 100 ms later, and the first
        // to arrive after the bring-up would run the exit branch and close the page the hotkey just opened.
        lcd_clear_pending_inputs = true;

#ifdef POLYCAST5_DEBUG
        ESP_LOGI(TAG, "Thermal view up: tof=%d medical=%d bus=%lu Hz",
                done.tof, done.medical, (unsigned long)done.scl_hz);
#endif
        return; // Let the timer paint the first frame
    }

    // Module missing or out of memory: any key leaves
    if (irx_failed) {
        if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
            irx_cleanup();
            lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
        } else if (ui_btns->up_btn || ui_btns->down_btn || ui_btns->left_btn ||
                ui_btns->right_btn || ui_btns->select_btn) {
            irx_cleanup();
            lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
            ui_menu->page = GPIO_PAGE;
        }
        return;
    }

    // SELECT hold bookkeeping, before the mode dispatch because more than one mode acts on select_btn. Once a
    // hold has been served, gpio_task's auto-repeat hands out short presses every 100 ms while the button is down;
    // those are swallowed here so the hold does not also trigger the short press. The latch clears on release.
    if (irx_sel_held) {
        if (gpio_select_btn_held) {
            ui_btns->select_btn = 0;
        } else {
            irx_sel_held = false;
        }
    }

    // Detail view: LEFT or RIGHT returns to the image, HOME/POWER leaves the page
    if (irx_mode == IRX_MODE_DETAIL) {
        if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) {
            irx_cleanup();
            lcd_transition_back(ui_btns->home_btn == 1, ui_menu);
        } else if (ui_btns->left_btn == 1 || ui_btns->right_btn == 1
                || ui_btns->select_btn == 1) {
            irx_hide_detail();
            irx_mode = IRX_MODE_VIEW;
            irx_last_seq = 0; // Force a full repaint of the image we hid
        }
        return;
    }

    // Live view. The hold is still consumed even though nothing acts on it any more: leaving it signalled would
    // hand the hold to whatever page comes next, and without the latch gpio_task auto-repeat shorts fall through
    // to the freeze toggle and flicker HOLD for as long as the button is held.
    if (xSelectButtonLongSemaphore != NULL
            && xSemaphoreTake(xSelectButtonLongSemaphore, 0) == pdTRUE) {
        irx_sel_held = true; // Swallow the auto-repeat shorts until the button comes up
    } else if (ui_btns->up_btn == 1) {
        irx_pal_idx = (uint8_t)((irx_pal_idx + IRX_PAL_COUNT - 1) % IRX_PAL_COUNT);
        irx_build_palette(irx_pal_idx);
        irx_set_status(irx_pal_names[irx_pal_idx]);
        irx_pal_show_until = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
    } else if (ui_btns->down_btn == 1) {
        irx_pal_idx = (uint8_t)((irx_pal_idx + 1) % IRX_PAL_COUNT);
        irx_build_palette(irx_pal_idx);
        irx_set_status(irx_pal_names[irx_pal_idx]);
        irx_pal_show_until = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
    } else if (ui_btns->right_btn == 1) { // Everything the rangefinder reports
        if (irx_have_tof) {
            irx_mode = IRX_MODE_DETAIL;
            irx_show_detail();
        }
    } else if (ui_btns->select_btn == 1) { // Freeze the image to read the numbers
        irx_frozen = !irx_frozen;
    } else if (ui_btns->left_btn == 1) { // Exit
        irx_cleanup();
        lv_obj_remove_flag(gpio_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        ui_menu->page = GPIO_PAGE;
    } else if (ui_btns->home_btn == 1 || ui_btns->pwr_btn == 1) { // Home or power off
        irx_cleanup();
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}
