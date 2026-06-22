#include <math.h>

#include "esp_log.h"
#include "nvs.h"

#include "gpio_task.h"   // accel/mag queues + button semaphores
#include "lis2dh12.h"    // accel_deg_t
#include "mmc5603.h"     // mmc5603_reading_t
#include "espnow_task.h" // xEspEcompassStreamCtrlQueue, xEspEcompassStreamQueue

#include "lcd_utils.h"
#include "lcd_ecompass.h"

#define TAG "LCD_ECOMPASS"

#define LEVEL_D      88    // Bubble-level circle diameter (px)
#define MAX_TILT_DEG 45.0f // Tilt that pushes the ball to the circle edge
#define DEG2RAD      0.017453292f

#define MODE_FLAT    0
#define MODE_HOLDING 1
#define MODE_REMOTE  2 // Upright, then rotated 90 deg to the right
#define MODE_COUNT   3

#define ARROW_SIZE 34      // Square ARGB image holding the arrow (px)
#define ARROW_FILL_SCALE 0.74f // Inner fill inset, leaving a white border rim
#define ARROW_SMOOTH 0.5f // Heading low-pass factor (0..1, higher = snappier / less lag)

#define HEADING_ARC_W   3  // Heading-trace ring thickness over the bubble rim (px)
#define HEADING_ARC_EXT 2  // How far the ring's outer edge sits beyond the circle rim (px)
#define ARC_TOP_DEG     270 // 12 o'clock in LVGL arc/scale angles (0 = right, increasing clockwise)

#define DIAL_TICK_CNT    12 // Rim tick marks, one every 30deg (matches the 12 calibration sectors)
#define DIAL_MAJOR_EVERY 3  // Every 3rd tick is a long "cardinal" mark -> 4 of them, at 12/3/6/9 o'clock
#define DIAL_TICK_MINOR  6  // Minor tick length, measured inward from the rim (px)
#define DIAL_TICK_MAJOR  8  // Cardinal tick length (px)
#define NORTH_PIP_R      30 // Radius of the drifting "N" marker from the circle centre (px)

#define BULLSEYE_RINGS   3  // Faint concentric tilt-scale rings inside the bubble (indicator dist = tilt)

// Magnetometer hard-/soft-iron calibration persistence (NVS)
// Stores the four X/Y min/max bounds so the compass works on every boot without a fresh calibration turn
#define ECOMPASS_NVS_NS  "ecompass"
#define ECOMPASS_NVS_KEY "minmax"

typedef struct {
    float x_min, x_max, y_min, y_max;
} ecompass_blob_t;

// Static ARGB8888 arrow image, re-rasterised on each page entry to track the accent colour
POLYCAST5_USE_PSRAM_BSS static uint8_t arrow_px[ARROW_SIZE * ARROW_SIZE * 4] __attribute__((aligned(4)));
static lv_image_dsc_t arrow_dsc;

// Hard-/soft-iron compass calibration state: Min/max of the X/Y field: centre = midpoint, radius = half-span
static float mag_cal_x_min = 1e9f, mag_cal_x_max = -1e9f;
static float mag_cal_y_min = 1e9f, mag_cal_y_max = -1e9f;
static bool ecompass_loaded = false; // NVS calibration loaded this boot?
static bool cal_complete = false;  // Do we have a usable calibration (from NVS or the cal page)?

// Centre of the calibration ellipse (hard-iron offset), used by both the compass and cal pages
static inline float ecompass_center_x(void) { return (mag_cal_x_min + mag_cal_x_max) * 0.5f; }
static inline float ecompass_center_y(void) { return (mag_cal_y_min + mag_cal_y_max) * 0.5f; }

// Arrow outline as offsets from the image centre, tip pointing up
static const float arrow_pts[4][2] = {
    {  0.0f, -13.0f }, // Tip
    { 10.0f,  11.0f }, // Right wing
    {  0.0f,   5.0f }, // Centre notch
    {-10.0f,  11.0f }, // Left wing
};

// Current orientation mode
static uint8_t accel_mode = MODE_FLAT;

// Ease one axis of the bubble-level ball from its current offset to a target
static void accel_ball_ease(lv_obj_t *ball, lv_anim_exec_xcb_t cb, int32_t from, int32_t to)
{
    #define ACCEL_EASE_MS 100

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ball);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ACCEL_EASE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Display name for each accelerometer orientation mode
static const char *accel_mode_name(uint8_t m)
{
    static const char *names[] = { "Flat", "Holding", "Remote" };
    return (m < 3) ? names[m] : "";
}

// Signed edge function (>0, <0 or 0 tells which side of segment AB the point P is on)
static inline float arrow_edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// True if (px,py) is inside the concave arrow scaled by k about the image centre
static bool arrow_hit(float px, float py, float k)
{
    const float ctr = ARROW_SIZE / 2.0f;
    const float tx = ctr + arrow_pts[0][0] * k, ty = ctr + arrow_pts[0][1] * k; // tip
    const float rx = ctr + arrow_pts[1][0] * k, ry = ctr + arrow_pts[1][1] * k; // right
    const float nx = ctr + arrow_pts[2][0] * k, ny = ctr + arrow_pts[2][1] * k; // notch
    const float lx = ctr + arrow_pts[3][0] * k, ly = ctr + arrow_pts[3][1] * k; // left

    // Inside if within triangle(tip,right,notch) OR triangle(tip,notch,left).
    // A point is in a triangle when its three edge functions all share one sign.
    float a = arrow_edge(tx, ty, rx, ry, px, py);
    float b = arrow_edge(rx, ry, nx, ny, px, py);
    float c = arrow_edge(nx, ny, tx, ty, px, py);
    bool in1 = (a <= 0 && b <= 0 && c <= 0) || (a >= 0 && b >= 0 && c >= 0);

    a = arrow_edge(tx, ty, nx, ny, px, py);
    b = arrow_edge(nx, ny, lx, ly, px, py);
    c = arrow_edge(lx, ly, tx, ty, px, py);
    bool in2 = (a <= 0 && b <= 0 && c <= 0) || (a >= 0 && b >= 0 && c >= 0);

    return in1 || in2;
}

// Rasterise the arrow into arrow_px (accent fill + white border, transparent elsewhere)
// and point the static descriptor at it
static void arrow_rasterize(lv_color_t fill)
{
    const uint32_t white  = 0xFFFFFFFFu;
    const uint32_t accent = ((uint32_t)0xFF << 24) | ((uint32_t)fill.red << 16) |
                            ((uint32_t)fill.green << 8) | (uint32_t)fill.blue;
    uint32_t *px = (uint32_t *)arrow_px;

    for (int y = 0; y < ARROW_SIZE; y++) {
        for (int x = 0; x < ARROW_SIZE; x++) {
            const float fx = x + 0.5f, fy = y + 0.5f; // sample pixel centres
            uint32_t v = 0; // transparent
            if (arrow_hit(fx, fy, ARROW_FILL_SCALE)) {
                v = accent;
            } else if (arrow_hit(fx, fy, 1.0f)) {
                v = white; // border rim
            }
            px[y * ARROW_SIZE + x] = v;
        }
    }

    arrow_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    arrow_dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    arrow_dsc.header.w      = ARROW_SIZE;
    arrow_dsc.header.h      = ARROW_SIZE;
    arrow_dsc.header.stride = ARROW_SIZE * 4;
    arrow_dsc.data_size     = sizeof(arrow_px);
    arrow_dsc.data          = arrow_px;
}

// Build the bubble-level view: circle + crosshair + a rotating direction-arrow indicator
static void accel_build_bubble(ui_menu_t *ui_menu, lv_obj_t *cont, lv_obj_t **out_ball, lv_obj_t **out_mode_lbl, lv_obj_t **out_val_lbl, lv_obj_t **out_arc, lv_obj_t **out_npip)
{
    #define X_OFFSET 15 // Move all to the right a bit

    // Bubble-level circle (left side)
    lv_obj_t *level_bg = lv_obj_create(cont);
    lv_obj_set_size(level_bg, LEVEL_D, LEVEL_D);
    lv_obj_align(level_bg, LV_ALIGN_LEFT_MID, X_OFFSET, 0);
    lv_obj_set_style_radius(level_bg, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(level_bg, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(level_bg, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(level_bg, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(level_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(level_bg, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Faint crosshair through the centre
    lv_obj_t *h_line = lv_obj_create(level_bg);
    lv_obj_set_size(h_line, LEVEL_D - 8, 1);
    lv_obj_align(h_line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(h_line, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(h_line, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(h_line, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(h_line, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *v_line = lv_obj_create(level_bg);
    lv_obj_set_size(v_line, 1, LEVEL_D - 8);
    lv_obj_align(v_line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(v_line, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(v_line, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(v_line, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(v_line, LV_OBJ_FLAG_SCROLLABLE);

    // Bullseye tilt scale: faint concentric rings the moving indicator crosses as it leaves
    // center, so its distance from the middle reads as a tilt gauge
    float ring_max = (LEVEL_D / 2.0f) - (ARROW_SIZE / 2.0f) - 2.0f;
    for (int i = 1; i <= BULLSEYE_RINGS; i++) { // Create each ring
        int32_t d = (int32_t)lroundf(2.0f * ring_max * (float)i / BULLSEYE_RINGS); // Ring diameter (px)
        lv_obj_t *ring = lv_obj_create(level_bg);
        lv_obj_set_size(ring, d, d);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(ring, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(ring, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ring, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(ring, LV_OPA_30, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }

    // Moving indicator (created last so it draws on top of the crosshair + rings):
    // a rotating direction arrow, translated by tilt and rotated by heading
    arrow_rasterize(user_secondary_color); // Build the arrow in the current accent colour
    lv_obj_t *ball = lv_image_create(level_bg);
    lv_image_set_src(ball, &arrow_dsc);
    lv_obj_set_size(ball, ARROW_SIZE, ARROW_SIZE);
    lv_image_set_pivot(ball, ARROW_SIZE / 2, ARROW_SIZE / 2); // Rotate about its centre
    lv_image_set_antialias(ball, true);
    lv_obj_align(ball, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(ball, LV_OBJ_FLAG_SCROLLABLE);

    // Right-side panel: mode name on top, X/Y readout centred below
    lv_obj_t *right_panel = lv_obj_create(cont);
    lv_obj_set_size(right_panel, 110, lv_pct(100));
    lv_obj_align(right_panel, LV_ALIGN_RIGHT_MID, X_OFFSET, 0);
    lv_obj_set_style_bg_opa(right_panel, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(right_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(right_panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    // Tighter row gap on the stream page, where the extra "Sending:" line crowds the X/Y/Z readout
    lv_obj_set_style_pad_row(right_panel, ui_menu->page == ESPNOW_ECOMPASS_STREAM_PAGE ? 4 : 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(right_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Current mode name (above the readout)
    lv_obj_t *mode_lbl = lv_label_create(right_panel);
    lv_obj_set_style_text_font(mode_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(mode_lbl, user_secondary_color, 0);
    lv_obj_set_style_text_align(mode_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));

    // Sending header shown when streaming via ESP-NOW
    if (ui_menu->page == ESPNOW_ECOMPASS_STREAM_PAGE) {
        lv_obj_t *sending_lbl = lv_label_create(right_panel);
        lv_obj_set_style_text_font(sending_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(sending_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_align(sending_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(sending_lbl, "Sending:");
    }

    // X / Y degree readout
    lv_obj_t *val_lbl = lv_label_create(right_panel);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(val_lbl, user_secondary_color, 0);
    lv_obj_set_style_text_align(val_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(val_lbl, "X: 0\xC2\xB0\nY: 0\xC2\xB0\nZ: 0\xC2\xB0");

    // Compass overlays: a tick dial, a thick arc that traces the turn from the entry pose, and a North pip that drifts as you rotate
    // Heading-trace ring: a thick accent arc that grows from the 12-o'clock "zero" point
    if (out_arc) {
        lv_obj_t *arc = lv_arc_create(cont); // Sibling of level_bg, drawn on top of its rim
        lv_obj_set_size(arc, LEVEL_D + 2 * HEADING_ARC_EXT, LEVEL_D + 2 * HEADING_ARC_EXT);
        lv_obj_align(arc, LV_ALIGN_LEFT_MID, X_OFFSET - HEADING_ARC_EXT, 0); // Centre on the level circle
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE); // Display only; buttons drive the UI
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

        // Hide the widget's own chrome: no rectangle bg, no background track arc, no knob
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB | LV_STATE_DEFAULT);

        // The visible part: a thick accent-coloured arc that thickens the rim as it traces
        lv_obj_set_style_arc_color(arc, user_secondary_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_width(arc, HEADING_ARC_W, LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR | LV_STATE_DEFAULT); // Crisp edge at the 0 mark

        lv_arc_set_rotation(arc, 0);
        lv_arc_set_bg_angles(arc, 0, 360); // Full (invisible) track so the indicator may sweep anywhere
        lv_arc_set_angles(arc, ARC_TOP_DEG, ARC_TOP_DEG); // Zero-length = nothing drawn until the heading moves

        *out_arc = arc;
    }

    // Tick dial: 12 marks at 30deg (the same wedges the calibration walks through), every 3rd one a longer cardinal
    lv_obj_t *dial = lv_scale_create(cont); // Sibling of level_bg, shares its box -> shares its center
    lv_obj_set_size(dial, LEVEL_D, LEVEL_D);
    lv_obj_align(dial, LV_ALIGN_LEFT_MID, X_OFFSET, 0);
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
    lv_scale_set_mode(dial, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(dial, false); // Ticks only, no numbers
    lv_scale_set_total_tick_count(dial, DIAL_TICK_CNT);
    lv_scale_set_major_tick_every(dial, DIAL_MAJOR_EVERY);
    lv_scale_set_angle_range(dial, 30 * (DIAL_TICK_CNT - 1)); // 330deg: 12 ticks at 30deg, last clears 0
    lv_scale_set_rotation(dial, ARC_TOP_DEG); // Tick 0 sits at the top (the 0 mark)

    // Drop the scale's own baseline ring + box
    lv_obj_set_style_bg_opa(dial, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(dial, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(dial, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(dial, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dial, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Minor ticks (the 8 in-between marks): short and dim
    lv_obj_set_style_length(dial, DIAL_TICK_MINOR, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(dial, 2, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(dial, user_secondary_color, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(dial, LV_OPA_40, LV_PART_ITEMS | LV_STATE_DEFAULT);

    // Cardinal ticks (the 4 quarter marks): longer and bold
    lv_obj_set_style_length(dial, DIAL_TICK_MAJOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_line_width(dial, 2, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(dial, user_secondary_color, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_line_opa(dial, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // North pip: a small "N" just inside the rim that points at magnetic north
    if (out_npip) {
        lv_obj_t *npip = lv_label_create(level_bg);
        lv_label_set_text(npip, "N");
        lv_obj_set_style_text_font(npip, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(npip, user_secondary_color, 0);
        lv_obj_remove_flag(npip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(npip, LV_OBJ_FLAG_HIDDEN);
        *out_npip = npip;
    }

    *out_ball = ball;
    *out_mode_lbl = mode_lbl;
    *out_val_lbl = val_lbl;
}

// Update the X/Y/Z readout from a reading and ease the indicator toward the tilt
// indicator_d is the indicator's diameter in px; travel is clamped to it so a larger indicator still stays inside the circle.
// heading is the last compass Z, drawn so an accel-only frame never drops the readout to two lines (mag block rewrites Z when fresh)
static void accel_apply_reading(const accel_deg_t *a, lv_obj_t *ball, float indicator_d, lv_obj_t *val_lbl, float heading, float *out_x, float *out_y)
{
    const float max_travel = (LEVEL_D / 2.0f) - (indicator_d / 2.0f) - 2.0f;

    float dx, dy; // Ball offset from centre (px)
    float read_x, read_y; // X/Y readout in deg, 0 at the current mode's centred pose
    if (accel_mode == MODE_FLAT) {
        dx = (-(a->roll) / MAX_TILT_DEG) * max_travel; // Horizontal inverted
        dy = (a->pitch / MAX_TILT_DEG) * max_travel;
        read_x = -a->roll;
        read_y = -a->pitch;
    } else {
        // Holding/Remote are used with the screen vertical, where atan2 pitch/roll gimbal-lock
        // Reconstruct the corrected gravity unit vector and recentre
        float p = a->pitch * DEG2RAD;
        float r = a->roll  * DEG2RAD;
        float gx = -sinf(p);          // Board +X gravity component
        float gy = cosf(p) * sinf(r); // Board +Y gravity component
        float gz = cosf(p) * cosf(r); // Board +Z gravity component
        const float scale = max_travel / sinf(MAX_TILT_DEG * DEG2RAD);

        if (accel_mode == MODE_HOLDING) {
            // Held upright, board +X points up (centred when gx~1, gy/gz~0)
            dx = -gy * scale; // Lean left/right
            dy = -gz * scale; // Tilt toward/away (screen normal)

            // Recompute text readings
            read_x = -asinf(gy) / DEG2RAD; // Deviation from upright, 0 when centred
            read_y =  asinf(gz) / DEG2RAD;
        } else { // MODE_REMOTE
            // Upright then rotated 90 deg right, board +Y points up (centred when gy~1)
            dx = -gz * scale; // Tilt toward/away (screen normal)
            dy = -gx * scale; // Lean up/down

            // Recompute text readings
            read_x = -asinf(gz) / DEG2RAD; // Deviation from remote pose, 0 when centred
            read_y =  asinf(gx) / DEG2RAD;
        }
    }

    // Keep the indicator inside the circle
    float mag = sqrtf(dx * dx + dy * dy);
    if (mag > max_travel) {
        dx *= max_travel / mag;
        dy *= max_travel / mag;
    }

    // Smoothly ease the indicator from its current offset to the new target
    accel_ball_ease(ball, (lv_anim_exec_xcb_t)lv_obj_set_x, lv_obj_get_style_x(ball, LV_PART_MAIN), (int32_t)dx);
    accel_ball_ease(ball, (lv_anim_exec_xcb_t)lv_obj_set_y, lv_obj_get_style_y(ball, LV_PART_MAIN), (int32_t)dy);

    // X/Y/Z readout: \xC2\xB0 is the degree symbol in UTF-8. Always three lines so the Z line never
    // blinks off on a frame that delivered accel but not mag; the mag block overwrites Z when fresh.
    char buf[64];
    snprintf(buf, sizeof(buf), "X: %+.0f\xC2\xB0\n" "Y: %+.0f\xC2\xB0\n" "Z: %.0f\xC2\xB0",
            (double)read_x, (double)read_y, (double)heading);
    lv_label_set_text(val_lbl, buf);

    // Hand the displayed values back so the stream can send exactly what's shown
    if (out_x) *out_x = read_x;
    if (out_y) *out_y = read_y;
}

static void ecompass_nvs_save(float x_min, float x_max, float y_min, float y_max)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(ECOMPASS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ecompass_nvs_save nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    // Write the four bounds as a blob and commit
    ecompass_blob_t blob = { x_min, x_max, y_min, y_max };
    err = nvs_set_blob(h, ECOMPASS_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    } else {
        ESP_LOGE(TAG, "ecompass_nvs_save set_blob failed: %s", esp_err_to_name(err));
    }

    // Close NVS
    nvs_close(h);
}

// Fill the four bounds from NVS and report whether a valid calibration was loaded
static bool ecompass_nvs_load(float *x_min, float *x_max, float *y_min, float *y_max)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(ECOMPASS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "ecompass_nvs_load nvs_open failed: %s", esp_err_to_name(err));
#endif
        return false;
    }

    // Read the four bounds as a blob
    ecompass_blob_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, ECOMPASS_NVS_KEY, &blob, &sz);
    nvs_close(h); // Close NVS
    if (err != ESP_OK || sz != sizeof(blob)) {
        return false; // Not stored yet
    }

    // Reject corrupt/implausible data so a bad blob can't break the compass
    float xr = (blob.x_max - blob.x_min) * 0.5f;
    float yr = (blob.y_max - blob.y_min) * 0.5f;
    if (xr <= 0.0f || yr <= 0.0f || xr > 500.0f || yr > 500.0f) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "ecompass_nvs_load rejected implausible blob: x_min=%.1f x_max=%.1f y_min=%.1f y_max=%.1f",
                (double)blob.x_min, (double)blob.x_max, (double)blob.y_min, (double)blob.y_max);
#endif
        return false;
    }

    *x_min = blob.x_min; *x_max = blob.x_max;
    *y_min = blob.y_min; *y_max = blob.y_max;
    return true;
}

static void prompt_accel_espnow_qr(ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    static lv_obj_t *qr_canvas = NULL;
    static uint8_t *qr_buf = NULL; // Canvas backing buffer
    
    // Hide arrows
    lv_obj_add_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
    
    // Create and format ins labels
    lv_obj_t *lbl_ask_enc = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_ask_enc, "Use wirelessly:", user_secondary_color,
            &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 11, 6);
    
    lv_obj_t *lbl_qr_ok = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_qr_ok, "OK", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_RIGHT_MID, -17, -1);

    lv_obj_t *lbl_qr_back = lv_label_create(ACTIVE_SCR);
    lcd_format_label(lbl_qr_back, "BACK", user_secondary_color,
            &lv_font_montserrat_18, LV_ALIGN_LEFT_MID, 16, -1);
    
    // Create QR canvas
    qr_canvas = lv_canvas_create(ACTIVE_SCR);
    lv_obj_set_size(qr_canvas, 100, 100);
    lv_obj_align(qr_canvas, LV_ALIGN_CENTER, 11, 12);
    
    // Draw the URL as a QR
    const char *url = "https://polycast5.com/blogs/tutorials/control-custom-builds-with-accelerometer";
    int n = lcd_draw_qr(qr_canvas, url, 100, &qr_buf);
    if (n != 0) {
        ESP_LOGE(TAG, "prompt_accel_espnow_qr lcd_draw_qr failed: %d", n);
    }
    
    while (1) {
        lv_timer_handler();
        
        // OK -> pick an ESP-NOW device to stream the readings to
        if (xSemaphoreTake(xRightButtonSemaphore, 0) == pdTRUE) {
            // Delete used
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_qr_ok);
            lv_obj_delete(lbl_qr_back);
            lv_obj_delete(qr_canvas);

            // Free QR buffer
            if (qr_buf) {
                free(qr_buf);
                qr_buf = NULL;
            }

            qr_canvas = NULL;

            // List-navigation arrows for the device picker
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // No right arrow

            lcd_clear_pending_inputs = true; // Clear any false inputs

            // Enter the ESP-NOW device picker in "accel streaming" mode
            espnow_entry_mode = ESPNOW_ENTRY_ACCEL; // Set picker flag

            // Configure the list for eCompass mode now so it shows correctly on the picker's first refresh
            lcd_espnow_refresh_list_for_mode(espnow_menu);
            ui_menu->page = ESPNOW_PAGE;
            return;
        }

        // BACK
        if (xSemaphoreTake(xLeftButtonSemaphore, 0) == pdTRUE) {
            // Show arrows
            lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);
            
            // Delete used
            lv_obj_delete(lbl_ask_enc);
            lv_obj_delete(lbl_qr_ok);
            lv_obj_delete(lbl_qr_back);
            lv_obj_delete(qr_canvas);
        
            // Free QR buffer
            if (qr_buf) {
                free(qr_buf);
                qr_buf = NULL;
            }
            
            qr_canvas = NULL;
            
            lcd_clear_pending_inputs = true; // Clear any false inputs
            
            // Go back
            return;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lcd_ecompass_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    #define ACCEL_REFRESH_MS 25 // Ask gpio_task for fresh accel + mag samples at ~40 Hz

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *ball = NULL;
    static lv_obj_t *mode_lbl = NULL;
    static lv_obj_t *val_lbl = NULL;
    static lv_obj_t *heading_arc = NULL;  // Rim arc tracing how far the heading has turned from 0
    static lv_obj_t *heading_npip = NULL; // "N" pip that drifts around the rim toward magnetic north
    static TickType_t last_refresh = 0;
    static float arrow_heading = 0.0f; // Smoothed absolute heading
    static float heading_ref = 0.0f;   // Heading captured at entry ("straight ahead" = up)
    static bool heading_init = false;  // Has heading_ref been captured this visit?
    static float arrow_drawn = -1.0f;  // Last relative angle actually rendered (-1 = none yet)
    static float disp_x = 0.0f, disp_y = 0.0f, disp_z = 0.0f; // Latest tilt + heading, Z carried into accel frames too

    if (!init) {
        // Seed the calibration from NVS once per boot and trust a valid stored calibration
        if (!ecompass_loaded) {
            ecompass_loaded = true;
            if (ecompass_nvs_load(&mag_cal_x_min, &mag_cal_x_max, &mag_cal_y_min, &mag_cal_y_max)) {
                cal_complete = true;
            }
        }

        // The compass can only read a heading once calibrated; on first boot there is none
        // Send the user straight to the calibration page rather than show a dead arrow
        if (!cal_complete) {
            ui_menu->page = ESPNOW_ECOMPASS_CAL_PAGE;
            return;
        }

        lv_obj_remove_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Show right arrow

        // Outer container
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Bubble view with the rotating arrow + heading-trace rim arc, tick dial and North pip
        accel_build_bubble(ui_menu, cont, &ball, &mode_lbl, &val_lbl, &heading_arc, &heading_npip);

        // Drop any stale readings left in the queues from a previous visit
        xQueueReset(xAccelReadingsQueue);
        xQueueReset(xMagReadingsQueue);

        last_refresh = 0; // Force an immediate trigger on the first frame
        arrow_heading = 0.0f;
        heading_ref = 0.0f;
        heading_init = false; // Re-capture the "straight ahead" reference on entry
        arrow_drawn = -1.0f;
        disp_x = disp_y = disp_z = 0.0f;
        init = true;
    }

    // Periodically ask gpio_task for a fresh accel (tilt) + mag (heading) sample
    if (xTaskGetTickCount() - last_refresh >= pdMS_TO_TICKS(ACCEL_REFRESH_MS)) {
        last_refresh = xTaskGetTickCount();
        xSemaphoreGive(xReadAccelSemaphore); // Req accel
        xSemaphoreGive(xReadMagSemaphore); // Req mag
    }

    // Update arrow + text whenever reading received
    accel_deg_t accel;
    if (xQueueReceive(xAccelReadingsQueue, &accel, 0) == pdTRUE) {
        // Draws the X/Y/Z readout (Z = last heading) and hands back the tilt for the mag block below
        accel_apply_reading(&accel, ball, ARROW_SIZE, val_lbl, disp_z, &disp_x, &disp_y);
    }

    // Compass heading from the magnetometer using the stored hard-/soft-iron calibration
    mmc5603_reading_t mag;
    if (ball && xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE) {
        // Recover the calibration shape
        float x_half = (mag_cal_x_max - mag_cal_x_min) * 0.5f; // X radius from the stored calibration
        float y_half = (mag_cal_y_max - mag_cal_y_min) * 0.5f; // Y radius

        if (x_half > 0.0f && y_half > 0.0f) { // Always true once calibrated; guards a bad blob
#ifdef POLYCAST5_DEBUG_MAGNETO
            static TickType_t last_cal_log = 0;
            if (xTaskGetTickCount() - last_cal_log >= pdMS_TO_TICKS(500)) {
                last_cal_log = xTaskGetTickCount();
                ESP_LOGI(TAG, "ecompass centre=(%.1f, %.1f) radius=(%.1f, %.1f)",
                         (double)ecompass_center_x(), (double)ecompass_center_y(),
                         (double)x_half, (double)y_half);
            }
#endif
            // Hard-iron: subtract the centre. Soft-iron: normalise each axis to its half-span.
            float cx = (mag.x - ecompass_center_x()) / x_half;
            float cy = (mag.y - ecompass_center_y()) / y_half;

            // atan2 of the centred unit circle -> heading in degrees, +y convention
            // rad -> deg; the driver already corrects the 180deg mount, so this is a true bearing
            float raw_heading = atan2f(cy, cx) / DEG2RAD;
            if (raw_heading < 0.0f) raw_heading += 360.0f;

            if (!heading_init) {
                // First sample this visit: take it as "straight ahead" so the arrow starts up
                arrow_heading = raw_heading;
                heading_ref = raw_heading;
                heading_init = true;
            } else {
                // Low-pass over the shortest angular path (handles the 360->0 wrap)
                float d = raw_heading - arrow_heading;
                while (d > 180.0f) d -= 360.0f;
                while (d < -180.0f) d += 360.0f;

                // Eases a fraction toward it each frame
                arrow_heading += d * ARROW_SMOOTH;
                if (arrow_heading < 0.0f) arrow_heading += 360.0f;
                else if (arrow_heading >= 360.0f) arrow_heading -= 360.0f;
            }
        }

        // Show the turn relative to the entry orientation: 0 = straight ahead = arrow up
        float rel = arrow_heading - heading_ref;
        while (rel < 0.0f) rel += 360.0f;
        while (rel >= 360.0f) rel -= 360.0f;

        // Remember the heading so the next accel frame can redraw Z without a fresh mag sample
        disp_z = rel;

        // Readout: X/Y are the tilt (from the accel), Z is the compass heading (arrow angle)
        // Refreshes Z with the fresh heading (accel_apply_reading already drew X/Y + last Z)
        char buf[64];
        snprintf(buf, sizeof(buf), "X: %+.0f\xC2\xB0\n" "Y: %+.0f\xC2\xB0\n" "Z: %.0f\xC2\xB0",
                (double)disp_x, (double)disp_y, (double)rel);
        lv_label_set_text(val_lbl, buf);

        // Actually spin the arrow on screen
        float dd = rel - arrow_drawn;
        while (dd > 180.0f) dd -= 360.0f;
        while (dd < -180.0f) dd += 360.0f;
        if (arrow_drawn < 0.0f || fabsf(dd) >= 1.0f) {
            lv_image_set_rotation(ball, (int32_t)lroundf(rel * 10.0f) % 3600);
            arrow_drawn = rel;

            // Grow the rim arc to match: from straight-up (0) clockwise through the turn
            if (heading_arc) {
                int32_t rdeg = (int32_t)lroundf(rel);
                if (rdeg > 359) rdeg = 359;
                lv_arc_set_angles(heading_arc, ARC_TOP_DEG, ARC_TOP_DEG + rdeg);
            }

            // Drift the North pip
            if (heading_npip) {
                // arrow_heading is kept in [0,360), so 360 - it is the north bearing CW from "up"
                float north_deg = fmodf(360.0f - arrow_heading, 360.0f);
                float a = (ARC_TOP_DEG + north_deg) * DEG2RAD; // -> LVGL screen angle (0 = right, +y down)
                lv_obj_align(heading_npip, LV_ALIGN_CENTER,
                             (int32_t)lroundf(NORTH_PIP_R * cosf(a)),
                             (int32_t)lroundf(NORTH_PIP_R * sinf(a)));
                lv_obj_remove_flag(heading_npip, LV_OBJ_FLAG_HIDDEN); // Reveal once a heading exists
            }
        }
    }

    /* User input */
    if (ui_btns->up_btn == 1) { // Next mode (wraps)
        accel_mode = (accel_mode + 1) % MODE_COUNT;
        lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));
    } else if (ui_btns->down_btn == 1) { // Open the compass calibration page
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = heading_arc = heading_npip = NULL;
        init = false;

        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Hide right
        ui_menu->page = ESPNOW_ECOMPASS_CAL_PAGE;
    } else if (ui_btns->select_btn) { // Refresh
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = heading_arc = heading_npip = NULL;
        init = false;

        lv_timer_handler(); // Refresh screen
    } else if (ui_btns->left_btn) { // Go back
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = heading_arc = heading_npip = NULL;
        init = false;

        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN); // Hide right

        // Back to ESP-NOW menu - restore the correct list rows before the first refresh
        lcd_espnow_refresh_list_for_mode(espnow_menu);
        ui_menu->page = ESPNOW_PAGE;
    } else if (ui_btns->right_btn) { // Use accel with ESP-NOW
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = heading_arc = heading_npip = NULL;
        init = false;

        // Show tutorial QR to proceed
        prompt_accel_espnow_qr(ui_menu, espnow_menu);
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont); // Deletes children

        cont = NULL;
        ball = mode_lbl = val_lbl = heading_arc = heading_npip = NULL;
        init = false;

        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_ecompass_calibration_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    #define ECOMPASS_REFRESH_MS 25 // Pull fresh mag samples at ~40 Hz while calibrating
    #define ECOMPASS_SECTORS_REQ 12 // Wedges (of 12) that count as a full turn

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;
    static lv_obj_t *status_lbl = NULL;
    static lv_obj_t *arrow_img = NULL;   // Live direction arrow (shown while calibrating)
    static lv_obj_t *prog_bar = NULL;    // Coverage progress bar (shown while calibrating)
    static bool calibrating = false;
    static uint16_t visited_sectors = 0; // Bitmask of the 12x30deg heading wedges seen this run
    static TickType_t last_refresh = 0;

    // Snapshot of the calibration taken when a run starts, restored if the user backs out
    static float bk_x_min, bk_x_max, bk_y_min, bk_y_max;
    static bool bk_cal_complete;

    if (!init) {
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cont, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(cont, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cont, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Title
        title_lbl = lv_label_create(cont);
        lv_label_set_text(title_lbl, "Compass Calibration");
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(title_lbl, user_secondary_color, 0);
        lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 0);

        // Instruction (centre); text changes once a run starts
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100));
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_align(instr_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(instr_lbl, "Lay flat away from metal, then turn a full circle.");
        lv_obj_align(instr_lbl, LV_ALIGN_CENTER, 0, 0);

        // Status / progress line at the bottom
        status_lbl = lv_label_create(cont);
        lv_obj_set_style_text_font(status_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(status_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(status_lbl, "SELECT: start   LEFT: back");
        lv_obj_align(status_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);

        // Live direction arrow (hidden until a run starts), built in the current accent colour
        arrow_rasterize(user_secondary_color);
        arrow_img = lv_image_create(cont);
        lv_image_set_src(arrow_img, &arrow_dsc);
        lv_obj_set_size(arrow_img, ARROW_SIZE, ARROW_SIZE);
        lv_image_set_pivot(arrow_img, ARROW_SIZE / 2, ARROW_SIZE / 2);
        lv_image_set_antialias(arrow_img, true);
        lv_obj_align(arrow_img, LV_ALIGN_CENTER, 0, -8);
        lv_obj_add_flag(arrow_img, LV_OBJ_FLAG_HIDDEN);

        // OTA-style coverage progress bar (hidden until a run starts)
        prog_bar = lv_bar_create(cont);
        lv_bar_set_range(prog_bar, 0, 100);
        lv_bar_set_value(prog_bar, 0, LV_ANIM_OFF);
        lv_obj_set_size(prog_bar, 150, 12);
        lv_obj_align(prog_bar, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_set_style_border_width(prog_bar, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(prog_bar, user_secondary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(prog_bar, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(prog_bar, lv_color_darken(user_primary_color, 100), LV_PART_MAIN);
        lv_obj_set_style_bg_color(prog_bar, user_secondary_color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(prog_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(prog_bar, 6, LV_PART_MAIN | LV_PART_INDICATOR);
        lv_obj_add_flag(prog_bar, LV_OBJ_FLAG_HIDDEN);

        calibrating = false;
        last_refresh = 0;
        init = true;
    }

    // While a run is active, keep pulling samples, grow the bounds, and show coverage
    if (calibrating) {
        if (xTaskGetTickCount() - last_refresh >= pdMS_TO_TICKS(ECOMPASS_REFRESH_MS)) {
            last_refresh = xTaskGetTickCount();
            xSemaphoreGive(xReadMagSemaphore);
        }

        // When a reading is received
        mmc5603_reading_t mag;
        if (xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE) {
            // Update calibration bounds
            if (mag.x < mag_cal_x_min) mag_cal_x_min = mag.x;
            if (mag.x > mag_cal_x_max) mag_cal_x_max = mag.x;
            if (mag.y < mag_cal_y_min) mag_cal_y_min = mag.y;
            if (mag.y > mag_cal_y_max) mag_cal_y_max = mag.y;

            // Live direction from the hard-iron-centred field
            // Only act when the vector is clearly off-centre

            // Field vector relative to the circle's center
            float dx = mag.x - ecompass_center_x();
            float dy = mag.y - ecompass_center_y();

            // Avoid dead zone near the center
            if (dx * dx + dy * dy > 9.0f) { // > ~3 uT from centre -> valid
                // Angle of that vector in degrees [0,360); the driver corrects the 180deg mount
                float heading = atan2f(dy, dx) / DEG2RAD;
                if (heading < 0.0f) heading += 360.0f;
                lv_image_set_rotation(arrow_img, (int32_t)lroundf(heading * 10.0f) % 3600); // Rotate arrow img

                // Split the circle into 12 wedges of 30 degrees and tally which ones the user has covered
                int sector = (int)(heading / 30.0f);
                if (sector >= 0 && sector < 12) {
                    visited_sectors |= (uint16_t)(1u << sector);
                }
            }

            // Counts the set bits = how many distinct wedges have been hit (0–12)
            int covered = __builtin_popcount(visited_sectors);

            // Map and show percentage of wedges covered
            int pct = covered * 100 / ECOMPASS_SECTORS_REQ;
            if (pct > 100) pct = 100;
            lv_bar_set_value(prog_bar, pct, LV_ANIM_ON);
            if (covered >= ECOMPASS_SECTORS_REQ) {
                lv_label_set_text(status_lbl, "Ready - SELECT to finish");
            } else {
                char buf[40];
                snprintf(buf, sizeof(buf), "Keep turning... %d%%", pct);
                lv_label_set_text(status_lbl, buf);
            }
        }
    }

    /* User input */
    if (ui_btns->select_btn) {
        if (!calibrating) {
            // Start: snapshot the current calibration, then clear it and begin collecting
            bk_x_min = mag_cal_x_min; bk_x_max = mag_cal_x_max;
            bk_y_min = mag_cal_y_min; bk_y_max = mag_cal_y_max;
            bk_cal_complete = cal_complete;

            // Min max start values
            mag_cal_x_min = mag_cal_y_min = 1e9f; // Biggest possible float
            mag_cal_x_max = mag_cal_y_max = -1e9f; // Smallest possible float
            cal_complete = false;
            visited_sectors = 0;
            xQueueReset(xMagReadingsQueue);

            calibrating = true;
            last_refresh = 0;
            
            lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_HIDDEN); // Free the top for the arrow
            lv_obj_add_flag(instr_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(arrow_img, LV_OBJ_FLAG_HIDDEN); // Reveal the live arrow + bar
            lv_obj_remove_flag(prog_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(prog_bar, 0, LV_ANIM_OFF);
            lv_label_set_text(status_lbl, "Keep turning... 0%");
        } else {
            // Finish: only accept once enough wedges are covered (a real full turn)
            if (__builtin_popcount(visited_sectors) >= ECOMPASS_SECTORS_REQ) {
                cal_complete = true;
                ecompass_nvs_save(mag_cal_x_min, mag_cal_x_max, mag_cal_y_min, mag_cal_y_max); // Save

                // Clean up
                lv_obj_delete(cont);
                cont = NULL; title_lbl = instr_lbl = status_lbl = arrow_img = prog_bar = NULL;
                calibrating = false; init = false;
                ui_menu->page = ESPNOW_ECOMPASS_PAGE; // Back to the compass
            } else {
                lv_label_set_text(status_lbl, "Not enough - keep turning");
            }
        }
    } else if (ui_btns->left_btn) { // Cancel / back out
        if (calibrating) { // Restore the calibration the run replaced
            mag_cal_x_min = bk_x_min; mag_cal_x_max = bk_x_max;
            mag_cal_y_min = bk_y_min; mag_cal_y_max = bk_y_max;
            cal_complete = bk_cal_complete;
        }

        // Clean up
        lv_obj_delete(cont);
        cont = NULL; title_lbl = instr_lbl = status_lbl = arrow_img = prog_bar = NULL;
        calibrating = false; init = false;

        if (cal_complete) { // Have a calibration -> back to the compass
            ui_menu->page = ESPNOW_ECOMPASS_PAGE;

        // No calibration yet (e.g. first-boot forced cal) -> ESP-NOW menu, not a dead compass
        } else {
            lcd_espnow_refresh_list_for_mode(espnow_menu); // Restore list rows before first refresh
            ui_menu->page = ESPNOW_PAGE;
        }
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        if (calibrating) {
            mag_cal_x_min = bk_x_min; mag_cal_x_max = bk_x_max;
            mag_cal_y_min = bk_y_min; mag_cal_y_max = bk_y_max;
            cal_complete = bk_cal_complete;
        }

        // Clean up
        lv_obj_delete(cont);
        cont = NULL; title_lbl = instr_lbl = status_lbl = arrow_img = prog_bar = NULL;
        calibrating = false; init = false;
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}

void lcd_ecompass_stream_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    #define STREAM_REFRESH_MS 25 // Trigger + transmit a sample at ~40 Hz

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *ball = NULL;
    static lv_obj_t *mode_lbl = NULL;
    static lv_obj_t *val_lbl = NULL;
    static lv_obj_t *heading_arc = NULL;  // Rim arc tracing how far the heading has turned from 0
    static lv_obj_t *heading_npip = NULL; // "N" pip that drifts around the rim toward magnetic north
    static TickType_t last_refresh = 0;
    static float arrow_heading = 0.0f; // Smoothed absolute heading
    static float heading_ref = 0.0f;   // Heading captured at entry ("straight ahead" = up)
    static bool heading_init = false;  // Has heading_ref been captured this visit?
    static float arrow_drawn = -1.0f;  // Last relative angle actually rendered (-1 = none yet)
    static float disp_x = 0.0f, disp_y = 0.0f, disp_z = 0.0f; // Latest tilt + heading, streamed each frame

    if (!init) {
        // Seed the calibration from NVS once per boot (redundant)
        if (!ecompass_loaded) {
            ecompass_loaded = true;
            if (ecompass_nvs_load(&mag_cal_x_min, &mag_cal_x_max, &mag_cal_y_min, &mag_cal_y_max)) {
                cal_complete = true;
            }
        }

        int idx = espnow_menu->index;

        // Encryption is on for this peer if it has a non-zero LMK stored
        uint8_t zero_lmk[LMK_LEN] = {0};
        bool enc = memcmp(espnow_menu->lmk[idx], zero_lmk, LMK_LEN) != 0;

        // Ask the ESP-NOW task to open a streaming session to this peer
        espnow_ecompass_ctrl_t ctrl = {
            .start = true,
            .enc = enc
        };
        memcpy(ctrl.mac_selected, espnow_menu->rx_mac[idx], ESPNOW_MAC_SIZE);
        if (enc) {
            memcpy(ctrl.lmk, espnow_menu->lmk[idx], LMK_LEN);
        }

        // Up/down change the view mode, left goes back; no right action here
        lv_obj_remove_flag(ui_menu->arrow_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_bot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_menu->arrow_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui_menu->arrow_left, LV_OBJ_FLAG_HIDDEN);

        // Outer container
        cont = lv_obj_create(ACTIVE_SCR);
        lv_obj_set_size(cont, 210, 106);
        lv_obj_center(cont);
        lv_obj_set_style_bg_color(cont, user_primary_color, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(cont, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Same compass view as the ecompass page: rotating arrow + heading-trace rim arc, tick dial and North pip
        accel_build_bubble(ui_menu, cont, &ball, &mode_lbl, &val_lbl, &heading_arc, &heading_npip);

        // Drop any stale readings left in the queues from a previous visit
        xQueueReset(xAccelReadingsQueue);
        xQueueReset(xMagReadingsQueue);

        last_refresh = 0; // Force an immediate trigger on the first frame
        arrow_heading = 0.0f;
        heading_ref = 0.0f;
        heading_init = false; // Re-capture the "straight ahead" reference on entry
        arrow_drawn = -1.0f;
        disp_x = disp_y = disp_z = 0.0f;

        // Paint the freshly-built stream UI before the radio bring-up
        lv_timer_handler();
        xQueueSend(xEspEcompassStreamCtrlQueue, &ctrl, portMAX_DELAY); // Bring the radio + peer up

        init = true;
    }

    // Periodically ask gpio_task for a fresh accel (tilt) + mag (heading) sample
    if (xTaskGetTickCount() - last_refresh >= pdMS_TO_TICKS(STREAM_REFRESH_MS)) {
        last_refresh = xTaskGetTickCount();
        xSemaphoreGive(xReadAccelSemaphore); // Req accel
        xSemaphoreGive(xReadMagSemaphore); // Req mag
    }

    // Move the arrow tilt + update the X/Y text
    bool fresh_sample = false;
    accel_deg_t accel;
    if (xQueueReceive(xAccelReadingsQueue, &accel, 0) == pdTRUE) {
        // Draws the X/Y/Z readout (Z = last heading) and hands back the tilt for the mag block below
        accel_apply_reading(&accel, ball, ARROW_SIZE, val_lbl, disp_z, &disp_x, &disp_y);
        fresh_sample = true;
    }

    // Compass heading from the magnetometer using the stored hard-/soft-iron calibration
    mmc5603_reading_t mag;
    if (ball && xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE) {
        // Recover the calibration shape
        float x_half = (mag_cal_x_max - mag_cal_x_min) * 0.5f; // X radius from the stored calibration
        float y_half = (mag_cal_y_max - mag_cal_y_min) * 0.5f; // Y radius

        if (x_half > 0.0f && y_half > 0.0f) { // Always true once calibrated; guards a bad blob
            // Hard-iron: subtract the centre. Soft-iron: normalise each axis to its half-span.
            float cx = (mag.x - ecompass_center_x()) / x_half;
            float cy = (mag.y - ecompass_center_y()) / y_half;

            // atan2 of the centred unit circle -> heading in degrees, +y convention
            // rad -> deg; the driver already corrects the 180deg mount, so this is a true bearing
            float raw_heading = atan2f(cy, cx) / DEG2RAD;
            if (raw_heading < 0.0f) raw_heading += 360.0f;

            if (!heading_init) {
                // First sample this visit: take it as "straight ahead" so the arrow starts up
                arrow_heading = raw_heading;
                heading_ref = raw_heading;
                heading_init = true;
            } else {
                // Low-pass over the shortest angular path (handles the 360->0 wrap)
                float d = raw_heading - arrow_heading;
                while (d > 180.0f) d -= 360.0f;
                while (d < -180.0f) d += 360.0f;

                // Eases a fraction toward it each frame
                arrow_heading += d * ARROW_SMOOTH;
                if (arrow_heading < 0.0f) arrow_heading += 360.0f;
                else if (arrow_heading >= 360.0f) arrow_heading -= 360.0f;
            }
        }

        // Show the turn relative to the entry orientation: 0 = straight ahead = arrow up
        float rel = arrow_heading - heading_ref;
        while (rel < 0.0f) rel += 360.0f;
        while (rel >= 360.0f) rel -= 360.0f;

        // Latest heading the stream should carry (Z); the next accel frame sends it
        disp_z = rel;

        // Readout: X/Y are the tilt (from the accel), Z is the compass heading (arrow angle)
        // Refreshes Z with the fresh heading (accel_apply_reading already drew X/Y + last Z)
        char buf[64];
        snprintf(buf, sizeof(buf), "X: %+.0f\xC2\xB0\n" "Y: %+.0f\xC2\xB0\n" "Z: %.0f\xC2\xB0",
                (double)disp_x, (double)disp_y, (double)rel);
        lv_label_set_text(val_lbl, buf);

        // Actually spin the arrow on screen
        float dd = rel - arrow_drawn;
        while (dd > 180.0f) dd -= 360.0f;
        while (dd < -180.0f) dd += 360.0f;
        if (arrow_drawn < 0.0f || fabsf(dd) >= 1.0f) {
            lv_image_set_rotation(ball, (int32_t)lroundf(rel * 10.0f) % 3600);
            arrow_drawn = rel;

            // Grow the rim arc to match: from straight-up (0) clockwise through the turn
            if (heading_arc) {
                int32_t rdeg = (int32_t)lroundf(rel);
                if (rdeg > 359) rdeg = 359;
                lv_arc_set_angles(heading_arc, ARC_TOP_DEG, ARC_TOP_DEG + rdeg);
            }

            // Drift the North pip
            if (heading_npip) {
                // arrow_heading is kept in [0,360), so 360 - it is the north bearing CW from "up"
                float north_deg = fmodf(360.0f - arrow_heading, 360.0f);
                float a = (ARC_TOP_DEG + north_deg) * DEG2RAD; // -> LVGL screen angle (0 = right, +y down)
                lv_obj_align(heading_npip, LV_ALIGN_CENTER,
                             (int32_t)lroundf(NORTH_PIP_R * cosf(a)),
                             (int32_t)lroundf(NORTH_PIP_R * sinf(a)));
                lv_obj_remove_flag(heading_npip, LV_OBJ_FLAG_HIDDEN); // Reveal once a heading exists
            }
        }
    }

    // Stream exactly what the LCD shows: mode-aware X/Y tilt + the compass heading Z, now that
    // both disp_x/disp_y and disp_z are settled this frame (sent at the accel cadence)
    if (fresh_sample) {
        espnow_ecompass_t sample = {
            .x = disp_x,
            .y = disp_y,
            .z = disp_z
        };
        xQueueOverwrite(xEspEcompassStreamQueue, &sample); // Latest value wins
    }

    /* User input */
    if (ui_btns->up_btn == 1) { // Next view mode (wraps)
        accel_mode = (accel_mode + 1) % MODE_COUNT;
        lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));
    } else if (ui_btns->down_btn == 1) { // Previous view mode (wraps)
        accel_mode = (accel_mode + MODE_COUNT - 1) % MODE_COUNT;
        lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));
    } else if (ui_btns->left_btn) { // Stop streaming, back to the main selection menu
        espnow_ecompass_ctrl_t stop = {
            .start = false
        };
        xQueueSend(xEspEcompassStreamCtrlQueue, &stop, portMAX_DELAY);

        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont);
        cont = NULL;
        ball = mode_lbl = val_lbl = heading_arc = heading_npip = NULL;
        init = false;

        espnow_entry_mode = ESPNOW_ENTRY_NORMAL;

        // Keep the ESP-NOW list hidden and jump straight to the main selection menu
        lv_obj_add_flag(espnow_menu->main_list, LV_OBJ_FLAG_HIDDEN);
        lcd_unhide_selection_widgets(ui_menu);
        ui_menu->page = SELECTION_PAGE;
    } else if (ui_btns->home_btn || ui_btns->pwr_btn) { // Home or power off
        espnow_ecompass_ctrl_t stop = {
            .start = false
        };
        xQueueSend(xEspEcompassStreamCtrlQueue, &stop, portMAX_DELAY);

        lv_anim_delete(ball, NULL); // Stop arrow anims before freeing the object
        lv_obj_delete(cont);
        cont = NULL;
        ball = mode_lbl = val_lbl = heading_arc = heading_npip = NULL;
        init = false;

        espnow_entry_mode = ESPNOW_ENTRY_NORMAL;
        lcd_transition_back(ui_btns->home_btn == 1, ui_menu); // True = home, false = sleep
    }
}