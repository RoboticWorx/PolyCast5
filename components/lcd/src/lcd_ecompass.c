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
#define ARROW_SMOOTH 0.95f // Heading low-pass factor (0..1, higher = snappier / less lag)

#define HEADING_ARC_W   3  // Heading-trace ring thickness over the bubble rim (px)
#define HEADING_ARC_EXT 2  // How far the ring's outer edge sits beyond the circle rim (px)
#define ARC_TOP_DEG     270 // 12 o'clock in LVGL arc/scale angles (0 = right, increasing clockwise)

#define DIAL_TICK_CNT    12 // Rim tick marks, one every 30deg
#define DIAL_MAJOR_EVERY 3  // Every 3rd tick is a long "cardinal" mark -> 4 of them, at 12/3/6/9 o'clock
#define DIAL_TICK_MINOR  6  // Minor tick length, measured inward from the rim (px)
#define DIAL_TICK_MAJOR  8  // Cardinal tick length (px)
#define NORTH_PIP_R      30 // Radius of the drifting "N" marker from the circle centre (px)

#define BULLSEYE_RINGS   3  // Faint concentric tilt-scale rings inside the bubble (indicator dist = tilt)

// Magnetometer hard-/soft-iron calibration persistence (NVS)
// Stores the fitted 3-axis centre + soft-iron correction so the compass works on every boot without recalibrating
#define ECOMPASS_NVS_NS      "ecompass"
#define ECOMPASS_NVS_KEY     "cal3d"
#define ECOMPASS_NVS_KEY_OLD "minmax" // Old flat X/Y-only calibration: never read, erased on save

// Plausible calibration: Earth's field is ~22-67 uT anywhere, with headroom for soft iron
#define ECOMPASS_R_MIN   12.0f   // Min radius along each ellipsoid axis (uT)
#define ECOMPASS_R_MAX   150.0f  // Max radius along each ellipsoid axis (uT)
#define ECOMPASS_R_RATIO 0.6f    // Smallest / largest radius; lower = partial coverage or a distorted fit
#define ECOMPASS_C_MAX   3000.0f // Max |centre| per axis (uT), the sensor's +-30 G full scale

// Calibration (also the NVS blob): w * (m - c) maps a reading m onto the unit sphere
typedef struct {
    float c[3];    // Hard-iron centre per axis (uT)
    float w[3][3]; // Soft-iron correction, symmetric (1/uT); diagonal 1/radius when the axes aren't coupled
} ecompass_cal_t;

// Static ARGB8888 arrow image, re-rasterised on each page entry to track the accent colour
POLYCAST5_USE_PSRAM_BSS static uint8_t arrow_px[ARROW_SIZE * ARROW_SIZE * 4] __attribute__((aligned(4)));
static lv_image_dsc_t arrow_dsc;

// Compass calibration in use (all zero = none)
static ecompass_cal_t mag_cal;
static bool ecompass_loaded = false; // NVS calibration loaded this boot?
static bool cal_complete = false;  // Do we have a usable calibration (from NVS or the cal page)?

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

    // Tick dial: 12 marks at 30deg, every 3rd one a longer cardinal
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

// Board-frame unit "up" vector (the accel reading at rest) rebuilt from pitch/roll
static void accel_up_vector(const accel_deg_t *a, float up[3])
{
    float p = a->pitch * DEG2RAD;
    float r = a->roll  * DEG2RAD;
    up[0] = -sinf(p);          // Board +X component
    up[1] = cosf(p) * sinf(r); // Board +Y component
    up[2] = cosf(p) * cosf(r); // Board +Z component
}

// Update the X/Y/Z readout from a reading and ease the indicator toward the tilt
// up is the reading's accel_up_vector(), used by the upright modes
// indicator_d is the indicator's diameter in px; travel is clamped to it so a larger indicator still stays inside the circle.
// heading is the last compass Z, drawn so an accel-only frame never drops the readout to two lines (mag block rewrites Z when fresh)
static void accel_apply_reading(const accel_deg_t *a, const float up[3], lv_obj_t *ball, float indicator_d, lv_obj_t *val_lbl, float heading, float *out_x, float *out_y)
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
        // Recentre on the corrected gravity unit vector instead
        float gx = up[0]; // Board +X gravity component
        float gy = up[1]; // Board +Y gravity component
        float gz = up[2]; // Board +Z gravity component
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

// Tilt-compensated heading (deg, [0,360)) of the current mode's forward direction from a magnetometer sample
// up is the board-frame unit up vector, or NULL to assume the mode's nominal pose. False when no heading is possible.
static bool ecompass_heading(const mmc5603_reading_t *m, const float *up, float *deg)
{
    static const float nominal_up[MODE_COUNT][3] = {
        { 0.0f, 0.0f, 1.0f }, // Flat: screen up
        { 1.0f, 0.0f, 0.0f }, // Holding: board +X up
        { 0.0f, 1.0f, 0.0f }, // Remote: board +Y up
    };

    if (!(mag_cal.w[0][0] > 0.0f && mag_cal.w[1][1] > 0.0f && mag_cal.w[2][2] > 0.0f)) {
        return false; // Always calibrated here; guards a bad state
    }
    if (!up) {
        up = nominal_up[accel_mode];
    }

    // Hard-iron: subtract the centre. Soft-iron: map the ellipsoid back onto the unit sphere.
    const float d[3] = { m->x - mag_cal.c[0], m->y - mag_cal.c[1], m->z - mag_cal.c[2] };
    float v[3];
    for (int i = 0; i < 3; i++) {
        v[i] = mag_cal.w[i][0] * d[0] + mag_cal.w[i][1] * d[1] + mag_cal.w[i][2] * d[2];
    }

#ifdef POLYCAST5_DEBUG_MAGNETO
    // v z < 0 lying flat screen-up (northern hemisphere) confirms the mag Z axis matches the accel frame
    static TickType_t last_cal_log = 0;
    if (xTaskGetTickCount() - last_cal_log >= pdMS_TO_TICKS(500)) {
        last_cal_log = xTaskGetTickCount();
        ESP_LOGI(TAG, "ecompass centre=(%.1f, %.1f, %.1f) w diag=(%.4f, %.4f, %.4f) v=(%.2f, %.2f, %.2f) up=(%.2f, %.2f, %.2f)",
                 (double)mag_cal.c[0], (double)mag_cal.c[1], (double)mag_cal.c[2],
                 (double)mag_cal.w[0][0], (double)mag_cal.w[1][1], (double)mag_cal.w[2][2],
                 (double)v[0], (double)v[1], (double)v[2],
                 (double)up[0], (double)up[1], (double)up[2]);
    }
#endif

    // East = field x up, north = up x east: both horizontal and equal length, so tilt drops out
    const float e[3] = { v[1] * up[2] - v[2] * up[1], v[2] * up[0] - v[0] * up[2], v[0] * up[1] - v[1] * up[0] };
    const float n[3] = { up[1] * e[2] - up[2] * e[1], up[2] * e[0] - up[0] * e[2], up[0] * e[1] - up[1] * e[0] };

    // Forward = the mode's screen left-right axis x up, which stays level as the device tips toward/away
    // Flat/Holding: left-right is board +Y (forward +X lying flat, -Z upright). Remote: board -X (forward -Z upright).
    float f[3];
    if (accel_mode == MODE_REMOTE) {
        f[0] = 0.0f;  f[1] = up[2]; f[2] = -up[1];
    } else {
        f[0] = up[2]; f[1] = 0.0f;  f[2] = -up[0];
    }

    // No heading with the left-right axis near vertical (wrong pose for the mode) or the field vertical
    if (f[0] * f[0] + f[1] * f[1] + f[2] * f[2] < 0.12f) return false;
    if (e[0] * e[0] + e[1] * e[1] + e[2] * e[2] < 1e-4f) return false;

    // Bearing of forward clockwise from magnetic north; equals atan2(y, x) when lying flat
    float h = atan2f(f[0] * e[0] + f[1] * e[1] + f[2] * e[2], f[0] * n[0] + f[1] * n[1] + f[2] * n[2]) / DEG2RAD;
    if (h < 0.0f) h += 360.0f;
    *deg = h;
    return true;
}

// LVGL angle of the viewer's "up" on screen (arrow start, arc start, N pip reference)
// Remote turns the screen 90 deg right, so its viewer-up is the screen's left edge, matching the Remote bubble
static int32_t ecompass_view_up_deg(void)
{
    return accel_mode == MODE_REMOTE ? ARC_TOP_DEG - 90 : ARC_TOP_DEG;
}

// Eigen-decompose a symmetric 3x3 in place (cyclic Jacobi): eigenvalues end on a's diagonal, eigenvectors in v's columns
static void ecompass_jacobi(float a[3][3], float v[3][3])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) v[i][j] = (i == j) ? 1.0f : 0.0f;
    }

    for (int sweep = 0; sweep < 8; sweep++) {
        for (int p = 0; p < 2; p++) {
            for (int q = p + 1; q < 3; q++) {
                if (a[p][q] == 0.0f) continue;

                // Rotation in the p-q plane that zeroes a[p][q]
                float th = (a[q][q] - a[p][p]) / (2.0f * a[p][q]);
                float t = copysignf(1.0f, th) / (fabsf(th) + sqrtf(th * th + 1.0f));
                float c = 1.0f / sqrtf(t * t + 1.0f), s = t * c;
                for (int k = 0; k < 3; k++) {
                    float kp = a[k][p], kq = a[k][q];
                    a[k][p] = c * kp - s * kq;
                    a[k][q] = s * kp + c * kq;
                }
                for (int k = 0; k < 3; k++) {
                    float pk = a[p][k], qk = a[q][k];
                    a[p][k] = c * pk - s * qk;
                    a[q][k] = s * pk + c * qk;
                }
                for (int k = 0; k < 3; k++) {
                    float kp = v[k][p], kq = v[k][q];
                    v[k][p] = c * kp - s * kq;
                    v[k][q] = s * kp + c * kq;
                }
            }
        }
    }
}

// True if a calibration is finite and physically plausible: centre in range, w symmetric, and every
// ellipsoid radius (1 / an eigenvalue of w) in range
static bool ecompass_cal_plausible(const ecompass_cal_t *cal)
{
    float a[3][3], v[3][3];
    for (int i = 0; i < 3; i++) {
        if (!isfinite(cal->c[i]) || fabsf(cal->c[i]) > ECOMPASS_C_MAX) {
            return false;
        }
        for (int j = 0; j < 3; j++) {
            if (!isfinite(cal->w[i][j]) || cal->w[i][j] != cal->w[j][i]) {
                return false;
            }
            a[i][j] = cal->w[i][j];
        }
    }

    ecompass_jacobi(a, v);
    float r_lo = ECOMPASS_R_MAX, r_hi = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (!(a[i][i] > 0.0f)) {
            return false;
        }
        float r = 1.0f / a[i][i];
        if (r < ECOMPASS_R_MIN || r > ECOMPASS_R_MAX) {
            return false;
        }
        r_lo = fminf(r_lo, r);
        r_hi = fmaxf(r_hi, r);
    }
    return r_lo >= ECOMPASS_R_RATIO * r_hi;
}

static void ecompass_nvs_save(const ecompass_cal_t *cal)
{
    nvs_handle_t h;

    // Open NVS
    esp_err_t err = nvs_open(ECOMPASS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ecompass_nvs_save nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    // Write the calibration as a blob and commit
    err = nvs_set_blob(h, ECOMPASS_NVS_KEY, cal, sizeof(*cal));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    } else {
        ESP_LOGE(TAG, "ecompass_nvs_save set_blob failed: %s", esp_err_to_name(err));
    }

    // Drop the old flat-only calibration (ESP_ERR_NVS_NOT_FOUND once gone)
    nvs_erase_key(h, ECOMPASS_NVS_KEY_OLD);

    // Close NVS
    nvs_close(h);
}

// Fill the calibration from NVS and report whether a valid one was loaded
static bool ecompass_nvs_load(ecompass_cal_t *cal)
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

    // Read the calibration as a blob (another size is an older format: recalibrate)
    ecompass_cal_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, ECOMPASS_NVS_KEY, &blob, &sz);
    nvs_close(h); // Close NVS
    if (err != ESP_OK || sz != sizeof(blob)) {
        return false; // Not stored yet
    }

    // Reject corrupt/implausible data so a bad blob can't break the compass
    if (!ecompass_cal_plausible(&blob)) {
#ifdef POLYCAST5_DEBUG
        ESP_LOGW(TAG, "ecompass_nvs_load rejected implausible blob: c=(%.1f, %.1f, %.1f) w diag=(%.4f, %.4f, %.4f)",
                (double)blob.c[0], (double)blob.c[1], (double)blob.c[2],
                (double)blob.w[0][0], (double)blob.w[1][1], (double)blob.w[2][2]);
#endif
        return false;
    }

    *cal = blob;
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
    const char *url = "https://polycast5.com/blogs/tutorials/control-projects-using-the-ecompass";
    int n = lcd_draw_qr(qr_canvas, url, 100, &qr_buf);
    if (n != 0) {
        ESP_LOGE(TAG, "prompt_accel_espnow_qr lcd_draw_qr failed: %d", n);
    }
    
    gpio_screen_changed(); // Taps and holds from before this prompt don't count
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
    static float accel_up[3];          // Latest board-frame up vector, for tilt compensation
    static bool accel_up_valid = false; // Has an accel frame arrived this visit?

    if (!init) {
        // Seed the calibration from NVS once per boot and trust a valid stored calibration
        if (!ecompass_loaded) {
            ecompass_loaded = true;
            if (ecompass_nvs_load(&mag_cal)) {
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

        // Small "CAL" calibration hint text for bottom arrow
        lv_obj_t *cali_hint = lv_label_create(cont);
        lcd_format_label(cali_hint, "CAL", user_secondary_color, &lv_font_montserrat_14, LV_ALIGN_BOTTOM_MID, 0, 7);
        lv_obj_remove_flag(cali_hint, LV_OBJ_FLAG_SCROLLABLE);

        // Drop any stale readings left in the queues from a previous visit
        xQueueReset(xAccelReadingsQueue);
        xQueueReset(xMagReadingsQueue);

        last_refresh = 0; // Force an immediate trigger on the first frame
        arrow_heading = 0.0f;
        heading_ref = 0.0f;
        heading_init = false; // Re-capture the "straight ahead" reference on entry
        arrow_drawn = -1.0f;
        disp_x = disp_y = disp_z = 0.0f;
        accel_up_valid = false;
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
        accel_up_vector(&accel, accel_up);
        accel_up_valid = true;

        // Draws the X/Y/Z readout (Z = last heading) and hands back the tilt for the mag block below
        accel_apply_reading(&accel, accel_up, ball, ARROW_SIZE, val_lbl, disp_z, &disp_x, &disp_y);
    }

    // Tilt-compensated compass heading from the calibrated magnetometer; skipped in a pose the mode can't read
    mmc5603_reading_t mag;
    float raw_heading;
    if (ball && xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE &&
            ecompass_heading(&mag, accel_up_valid ? accel_up : NULL, &raw_heading)) {
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
            const int32_t up_deg = ecompass_view_up_deg();
            lv_image_set_rotation(ball, ((int32_t)lroundf(rel * 10.0f) + (up_deg - ARC_TOP_DEG) * 10 + 3600) % 3600);
            arrow_drawn = rel;

            // Grow the rim arc to match: from straight-up (0) clockwise through the turn
            if (heading_arc) {
                int32_t rdeg = (int32_t)lroundf(rel);
                if (rdeg > 359) rdeg = 359;
                lv_arc_set_angles(heading_arc, up_deg, up_deg + rdeg);
            }

            // Drift the North pip
            if (heading_npip) {
                // arrow_heading is kept in [0,360), so 360 - it is the north bearing CW from "up"
                float north_deg = fmodf(360.0f - arrow_heading, 360.0f);
                float a = ((float)up_deg + north_deg) * DEG2RAD; // -> LVGL screen angle (0 = right, +y down)
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
        arrow_drawn = -1.0f; // Redraw the arrow/arc/pip for the new mode's viewer-up
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

// Calibration samples (uT), one share per turn
#define ECOMPASS_CAL_TURNS    3   // Flat, upright, sideways
#define ECOMPASS_CAL_TURN_PTS 128 // Each turn's share of the buffer
#define ECOMPASS_CAL_PTS      (ECOMPASS_CAL_TURNS * ECOMPASS_CAL_TURN_PTS)
#define ECOMPASS_OUTLIER       0.2f // Radial error (fraction of the radius) past which a sample is a glitch
POLYCAST5_USE_PSRAM_BSS static float cal_pts[ECOMPASS_CAL_PTS][3];

// Squared distance between two 3-vectors
static inline float ecompass_dist2(const float a[3], const float b[3])
{
    return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
}

// Radial error of a sample on a calibration's ellipsoid, as a fraction of its radius (0 = on the surface)
static float ecompass_radial_err(const float p[3], const ecompass_cal_t *cal)
{
    float v2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = 0.0f;
        for (int k = 0; k < 3; k++) d += cal->w[i][k] * (p[k] - cal->c[k]);
        v2 += d * d;
    }
    return sqrtf(v2) - 1.0f;
}

// True if a prior fit (none when NULL) marks sample i as an outlier
static inline bool ecompass_fit_skip(int i, const ecompass_cal_t *prior)
{
    return prior && fabsf(ecompass_radial_err(cal_pts[i], prior)) > ECOMPASS_OUTLIER;
}

// Least-squares ellipsoid through the first n samples: centre = hard iron, shape = soft iron
// Fits x^2+y^2+z^2 + a(x^2-z^2) + b(y^2-z^2) + dx + ey + fz + g = 0 (axis weights sum to 3); coupled also fits
// 2pxy + 2qxz + 2syz, for soft iron that couples the axes. False if degenerate.
// With a prior fit, its outliers are left out, so a glitch can't drag the result.
static bool ecompass_fit(int n, bool coupled, const ecompass_cal_t *prior, ecompass_cal_t *out)
{
    // Centre and scale the samples to ~unit size so the float normal equations stay well conditioned
    float mean[3] = { 0.0f, 0.0f, 0.0f };
    int used = 0;
    for (int i = 0; i < n; i++) {
        if (ecompass_fit_skip(i, prior)) continue;
        for (int k = 0; k < 3; k++) mean[k] += cal_pts[i][k];
        used++;
    }
    if (used < 10) return false;
    for (int k = 0; k < 3; k++) mean[k] /= (float)used;

    float spread = 0.0f;
    for (int i = 0; i < n; i++) {
        if (ecompass_fit_skip(i, prior)) continue;
        for (int k = 0; k < 3; k++) {
            float d = cal_pts[i][k] - mean[k];
            spread += d * d;
        }
    }
    spread = sqrtf(spread / (float)used);
    if (spread < 1.0f) return false; // Barely moved

    // Normal equations: 6 or 9 unknowns (a, b, d, e, f, g[, p, q, s]) + right-hand side in column nu
    const int nu = coupled ? 9 : 6;
    float m[9][10] = { 0 };
    for (int i = 0; i < n; i++) {
        if (ecompass_fit_skip(i, prior)) continue;
        float x = (cal_pts[i][0] - mean[0]) / spread;
        float y = (cal_pts[i][1] - mean[1]) / spread;
        float z = (cal_pts[i][2] - mean[2]) / spread;
        float row[10] = { x * x - z * z, y * y - z * z, x, y, z, 1.0f, 2.0f * x * y, 2.0f * x * z, 2.0f * y * z, 0.0f };
        row[nu] = -(x * x + y * y + z * z);
        for (int j = 0; j < nu; j++) {
            for (int k = j; k <= nu; k++) m[j][k] += row[j] * row[k];
        }
    }
    for (int j = 1; j < nu; j++) {
        for (int k = 0; k < j; k++) m[j][k] = m[k][j]; // Mirror the symmetric half
    }

    // Gauss-Jordan elimination with partial pivoting
    for (int col = 0; col < nu; col++) {
        int piv = col;
        for (int j = col + 1; j < nu; j++) {
            if (fabsf(m[j][col]) > fabsf(m[piv][col])) piv = j;
        }
        if (fabsf(m[piv][col]) < 1e-6f * (float)used) return false; // Samples don't pin down an ellipsoid
        for (int k = 0; k <= nu; k++) {
            float t = m[col][k]; m[col][k] = m[piv][k]; m[piv][k] = t;
        }
        for (int j = 0; j < nu; j++) {
            if (j == col) continue;
            float s = m[j][col] / m[col][col];
            for (int k = col; k <= nu; k++) m[j][k] -= s * m[col][k];
        }
    }
    float sol[9] = { 0 };
    for (int j = 0; j < nu; j++) sol[j] = m[j][nu] / m[j][j];

    // Quadric x'Qx + l.x + g = 0: its eigenbasis gives the ellipsoid's axes
    float a[3][3] = {
        { 1.0f + sol[0], sol[6],        sol[7]                 },
        { sol[6],        1.0f + sol[1], sol[8]                 },
        { sol[7],        sol[8],        1.0f - sol[0] - sol[1] },
    };
    float v[3][3];
    ecompass_jacobi(a, v);

    // Centre in the eigenbasis (t), then the level k_sum of (x - c)'Q(x - c) = k_sum
    float t[3], k_sum = -sol[5];
    for (int e = 0; e < 3; e++) {
        if (!(a[e][e] > 0.0f)) return false; // Not an ellipsoid
        const float le = v[0][e] * sol[2] + v[1][e] * sol[3] + v[2][e] * sol[4];
        t[e] = -le / (2.0f * a[e][e]);
        k_sum += a[e][e] * t[e] * t[e];
    }
    if (!(k_sum > 0.0f)) return false;

    // Back in uT: c = mean + spread * V t, w = V sqrt(D / k_sum) V' / spread (built symmetric)
    for (int i = 0; i < 3; i++) {
        out->c[i] = mean[i] + spread * (v[i][0] * t[0] + v[i][1] * t[1] + v[i][2] * t[2]);
        for (int j = i; j < 3; j++) {
            float w = 0.0f;
            for (int e = 0; e < 3; e++) w += v[i][e] * sqrtf(a[e][e] / k_sum) * v[j][e];
            out->w[i][j] = out->w[j][i] = w / spread;
        }
    }
    return true;
}

// RMS radial error (fraction) of the first n samples on a fit, outliers left out and counted into *outliers
static float ecompass_fit_rms(int n, const ecompass_cal_t *cal, int *outliers)
{
    float err = 0.0f;
    int used = 0;
    *outliers = 0;
    for (int i = 0; i < n; i++) {
        float d = ecompass_radial_err(cal_pts[i], cal);
        if (fabsf(d) > ECOMPASS_OUTLIER) {
            (*outliers)++;
            continue;
        }
        err += d * d;
        used++;
    }
    return used ? sqrtf(err / (float)used) : 1.0f;
}

void lcd_ecompass_calibration_page(ui_btns_t *ui_btns, ui_menu_t *ui_menu, espnow_menu_t *espnow_menu)
{
    #define ECOMPASS_REFRESH_MS  25     // Pull fresh accel + mag samples at ~40 Hz while calibrating
    #define ECOMPASS_SECTORS_REQ 12     // Wedges (of 12) that count as a full turn
    #define ECOMPASS_CAL_STEP_MIN 1.0f  // Min uT between stored samples, so holding still doesn't skew the fit
    #define ECOMPASS_CAL_STEP_MAX 4.0f  // Spacing cap, so a strong field still gets enough samples per circle
    #define ECOMPASS_POSE_MIN    0.8f   // |up| along a turn's vertical axis from which its pose counts (within ~37 deg)
    #define ECOMPASS_STEADY_MAX  0.15f  // Max up-vector change over the last 3 reads (~9 deg) for them to steer the arrow +
                                        // wedges: a read tipping into the pose leaks the vertical field sideways
    #define ECOMPASS_RMS_MAX     0.10f  // Max RMS radial fit error: more means a distorted field (metal nearby)
    #define ECOMPASS_OUTLIER_PCT 5      // Max % of samples the fit may drop as glitches
    #define ECOMPASS_JUMP_MAX    150.0f // uT between consecutive reads the field can't move: a glitch
    #define ECOMPASS_COUPLED_GAIN 0.5f  // Coupled fit used when its RMS is below this share of the axis-aligned one
    #define ECOMPASS_SENSOR_MS   1000   // No accel/mag frame for this long: that sensor isn't responding

    // Each turn holds a different board axis vertical, so together they pin every axis' centre and scale
    static const struct {
        uint8_t vert;          // Board axis pointing up (or down) during the turn
        const char *prompt;    // Step prompt at the top
        const char *pose_hint; // Status while held in the wrong pose
    } turns[ECOMPASS_CAL_TURNS] = {
        { 2, "1/3: Lay flat, turn a circle", "Lay it flat" },
        { 0, "2/3: Upright, turn around", "Stand it on its long edge" },
        { 1, "3/3: Sideways, turn around", "Stand it on its short edge" },
    };

    // Statics
    static bool init = false;
    static lv_obj_t *cont = NULL;
    static lv_obj_t *title_lbl = NULL;
    static lv_obj_t *instr_lbl = NULL;
    static lv_obj_t *status_lbl = NULL;
    static lv_obj_t *arrow_img = NULL;   // Live direction arrow (shown while calibrating)
    static lv_obj_t *prog_bar = NULL;    // Coverage progress bar (shown while calibrating)
    static bool calibrating = false;
    static int cal_turn = 0;             // Index into turns[]
    static uint16_t visited_sectors = 0; // Bitmask of the 12x30deg wedges seen this turn
    static float lo[3], hi[3];           // This turn's per-axis extremes; their midpoint centres the arrow
    static int cal_n = 0;                // Samples in cal_pts
    static int turn_n0 = 0;              // cal_n when this turn started; SELECT mid-turn redoes the turn from here
    static float hist[3][6];             // Last 3 mag reads (uT) + the up vector each came with, newest first
    static int hist_n = 0;
    static float cal_prev[3];            // Previous read, for the glitch check
    static bool cal_have_prev = false;
    static float up[3];                  // Latest board-frame up vector (accel)
    static bool up_valid = false;
    static ecompass_cal_t fit;           // Fit saved on finish
    static bool fit_ready = false;       // All turns done and the fit is good enough to save
    static bool fit_uneven = false;      // All turns done but the fit is distorted: SELECT restarts the run
    static TickType_t last_refresh = 0;
    static TickType_t last_accel = 0, last_mag = 0; // When each sensor last delivered, to flag a dead one

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

        // Instruction (centre); moves to the top as the step prompt once a run starts
        instr_lbl = lv_label_create(cont);
        lv_label_set_long_mode(instr_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(instr_lbl, lv_pct(100));
        lv_obj_set_style_text_font(instr_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(instr_lbl, user_secondary_color, 0);
        lv_obj_set_style_text_align(instr_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(instr_lbl, "Away from metal, turn it a full circle flat, upright, then sideways.");
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

    // While a run is active: track each turn, spin the arrow, and fit once all turns are done
    const bool was_uneven = fit_uneven; // State on screen when SELECT was pressed, before this frame's fit
    if (calibrating) {
        const TickType_t now = xTaskGetTickCount();
        if (now - last_refresh >= pdMS_TO_TICKS(ECOMPASS_REFRESH_MS)) {
            last_refresh = now;
            xSemaphoreGive(xReadAccelSemaphore); // Req accel (pose)
            xSemaphoreGive(xReadMagSemaphore); // Req mag
        }

        accel_deg_t accel;
        if (xQueueReceive(xAccelReadingsQueue, &accel, 0) == pdTRUE) {
            accel_up_vector(&accel, up);
            up_valid = true;
            last_accel = now;
        }

        // When a reading is received (the pose must be known to use it)
        mmc5603_reading_t mag;
        if (up_valid && xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE) {
            last_mag = now;

            // Use the medoid of the last 3 reads (the one facing their widest gap): a lone spike never comes out
            memmove(hist[1], hist[0], 2 * sizeof(hist[0]));
            hist[0][0] = mag.x; hist[0][1] = mag.y; hist[0][2] = mag.z;
            memcpy(&hist[0][3], up, 3 * sizeof(float));
            if (hist_n < 3) hist_n++;
            float gap[3], tip = 0.0f; // gap[i]: field distance^2 between the two reads other than i; tip: max pose change^2
            for (int i = 0; i < 3; i++) {
                const float *p = hist[(i + 1) % 3], *q = hist[(i + 2) % 3];
                gap[i] = ecompass_dist2(p, q);
                tip = fmaxf(tip, ecompass_dist2(&p[3], &q[3]));
            }
            const int med = (gap[0] >= gap[1] && gap[0] >= gap[2]) ? 0 : (gap[1] >= gap[2]) ? 1 : 2;
            const float *s = hist[med];         // Field (uT)
            const float *s_up = &hist[med][3];  // Pose it was read in

            // Drops a glitch read and the one after it; nothing until the medoid has 3 reads
            bool glitch = hist_n < 3;
            if (!glitch) {
                float jump = 0.0f;
                for (int k = 0; k < 3; k++) {
                    jump += (s[k] - cal_prev[k]) * (s[k] - cal_prev[k]);
                    cal_prev[k] = s[k];
                }
                glitch = cal_have_prev && jump > ECOMPASS_JUMP_MAX * ECOMPASS_JUMP_MAX;
                cal_have_prev = true;
            }

            // A turn only counts reads held in its pose, so the turns are in three different planes
            // Only a steady pose steers the arrow + wedges: turning leaves the board-frame up vector still, tipping doesn't
            const int vert = turns[cal_turn].vert;
            const bool posed = fabsf(s_up[vert]) >= ECOMPASS_POSE_MIN;
            const bool tracked = posed && tip <= ECOMPASS_STEADY_MAX * ECOMPASS_STEADY_MAX;
            const bool done = fit_ready || fit_uneven;
            int covered = __builtin_popcount(visited_sectors);

            if (!glitch && posed && !done) {
                if (tracked) {
                    for (int k = 0; k < 3; k++) {
                        lo[k] = fminf(lo[k], s[k]);
                        hi[k] = fmaxf(hi[k], s[k]);
                    }
                }

                // The two level axes the turn sweeps (flat: X/Y)
                const int ax_a = (vert + 1) % 3;
                const int ax_b = (vert + 2) % 3;

                // Store once it has moved far enough from the last stored sample
                // Spacing follows this turn's radius, so a weak level field (high latitudes) still gets ~60 per circle
                // (before any tracked read, lo/hi are still unset and the spacing clamps to the minimum)
                const int cap = (cal_turn + 1) * ECOMPASS_CAL_TURN_PTS;
                if (cal_n < cap) {
                    float spacing = fmaxf(hi[ax_a] - lo[ax_a], hi[ax_b] - lo[ax_b]) * 0.05f;
                    spacing = fminf(fmaxf(spacing, ECOMPASS_CAL_STEP_MIN), ECOMPASS_CAL_STEP_MAX);
                    float step = 0.0f;
                    if (cal_n > 0) {
                        for (int k = 0; k < 3; k++) {
                            step += (s[k] - cal_pts[cal_n - 1][k]) * (s[k] - cal_pts[cal_n - 1][k]);
                        }
                    }
                    if (cal_n == 0 || step >= spacing * spacing) {
                        memcpy(cal_pts[cal_n++], s, sizeof(cal_pts[0]));
                    }
                }

                // Live direction from this turn's centre; only act when the vector is clearly off-centre
                float dx = s[ax_a] - (lo[ax_a] + hi[ax_a]) / 2.0f;
                float dy = s[ax_b] - (lo[ax_b] + hi[ax_b]) / 2.0f;
                if (tracked && dx * dx + dy * dy > 9.0f) { // > ~3 uT from centre -> valid
                    float ang = atan2f(dy, dx) / DEG2RAD;
                    if (ang < 0.0f) ang += 360.0f;
                    lv_image_set_rotation(arrow_img, (int32_t)lroundf(ang * 10.0f) % 3600); // Rotate arrow img

                    // Split the circle into 12 wedges of 30 degrees and tally which ones the user has covered
                    int sector = (int)(ang / 30.0f);
                    if (sector >= 0 && sector < 12) {
                        visited_sectors |= (uint16_t)(1u << sector);
                    }
                }

                covered = __builtin_popcount(visited_sectors);
                if (covered >= ECOMPASS_SECTORS_REQ) {
                    if (cal_turn < ECOMPASS_CAL_TURNS - 1) {
                        // Turn done: start the next pose with a fresh centre and wedge count
                        cal_turn++;
                        turn_n0 = cal_n;
                        visited_sectors = 0;
                        covered = 0;
                        for (int k = 0; k < 3; k++) {
                            lo[k] = 1e9f;
                            hi[k] = -1e9f;
                        }
                        lv_label_set_text(instr_lbl, turns[cal_turn].prompt);
                    } else {
                        // All turns done: fit the ellipsoid both ways, each refit without the glitches its first pass shows
                        // Coupled only when it fits far better (then the axis-aligned fit is wrong): its 3 extra terms
                        // are noisier on small high-dip circles
                        ecompass_cal_t pre = { 0 }, axis = { 0 }, coupled = { 0 };
                        int out_a = 0, out_c = 0;
                        const bool ok_a = ecompass_fit(cal_n, false, NULL, &pre) && ecompass_fit(cal_n, false, &pre, &axis);
                        const bool ok_c = ecompass_fit(cal_n, true, NULL, &pre) && ecompass_fit(cal_n, true, &pre, &coupled);
                        const float rms_a = ok_a ? ecompass_fit_rms(cal_n, &axis, &out_a) : 1.0f;
                        const float rms_c = ok_c ? ecompass_fit_rms(cal_n, &coupled, &out_c) : 1.0f;
                        const bool pass_a = ok_a && ecompass_cal_plausible(&axis) && rms_a <= ECOMPASS_RMS_MAX &&
                                out_a * 100 <= cal_n * ECOMPASS_OUTLIER_PCT;
                        const bool pass_c = ok_c && ecompass_cal_plausible(&coupled) && rms_c <= ECOMPASS_RMS_MAX &&
                                out_c * 100 <= cal_n * ECOMPASS_OUTLIER_PCT;
                        const bool use_c = !pass_a || rms_c < ECOMPASS_COUPLED_GAIN * rms_a;
                        fit = use_c ? coupled : axis;
                        fit_ready = use_c ? pass_c : pass_a;
                        fit_uneven = !fit_ready;
#ifdef POLYCAST5_DEBUG
                        ESP_LOGI(TAG, "ecompass fit n=%d rms axis %.3f (%d out) coupled %.3f (%d out) -> %s c=(%.1f, %.1f, %.1f) "
                                "w=(%.4f, %.4f, %.4f | %.4f, %.4f, %.4f)", cal_n, (double)rms_a, out_a, (double)rms_c, out_c,
                                !fit_ready ? "uneven" : use_c ? "coupled" : "axis",
                                (double)fit.c[0], (double)fit.c[1], (double)fit.c[2],
                                (double)fit.w[0][0], (double)fit.w[1][1], (double)fit.w[2][2],
                                (double)fit.w[0][1], (double)fit.w[0][2], (double)fit.w[1][2]);
#endif
                    }
                }
            }

            // Map and show percentage: each turn is an equal share of the bar
            int pct = (cal_turn * ECOMPASS_SECTORS_REQ + covered) * 100 / (ECOMPASS_CAL_TURNS * ECOMPASS_SECTORS_REQ);
            if (fit_ready || fit_uneven) pct = 100;
            lv_bar_set_value(prog_bar, pct, LV_ANIM_ON);
            if (fit_ready) {
                lv_label_set_text(status_lbl, "Ready - SELECT to finish");
            } else if (fit_uneven) {
                // Samples don't sit on one ellipsoid: metal nearby or a magnet moved during the run
                lv_label_set_text(status_lbl, "Near metal - SELECT: retry");
            } else if (fabsf(up[turns[cal_turn].vert]) < ECOMPASS_POSE_MIN) { // Live pose: a step just entered hints at once
                lv_label_set_text(status_lbl, turns[cal_turn].pose_hint);
            } else {
                char buf[40];
                snprintf(buf, sizeof(buf), "Keep turning... %d%%", pct);
                lv_label_set_text(status_lbl, buf);
            }
        }

        // A dead sensor would otherwise look like a run stuck at 0%
        if (!fit_ready && !fit_uneven) {
            if (now - last_accel > pdMS_TO_TICKS(ECOMPASS_SENSOR_MS)) {
                lv_label_set_text(status_lbl, "Tilt sensor not responding");
            } else if (now - last_mag > pdMS_TO_TICKS(ECOMPASS_SENSOR_MS)) {
                lv_label_set_text(status_lbl, "Compass not responding");
            }
        }
    }

    /* User input */
    if (ui_btns->select_btn) {
        if (!calibrating || (was_uneven && fit_uneven)) {
            // Start, or restart a distorted run: fresh sample set; the calibration in use stays until a fit is accepted
            cal_n = 0;
            turn_n0 = 0;
            cal_turn = 0;
            visited_sectors = 0;
            for (int k = 0; k < 3; k++) {
                lo[k] = 1e9f;
                hi[k] = -1e9f;
            }
            hist_n = 0;
            cal_have_prev = false;
            up_valid = false;
            fit_ready = fit_uneven = false;
            xQueueReset(xAccelReadingsQueue);
            xQueueReset(xMagReadingsQueue);

            calibrating = true;
            last_refresh = 0;
            last_accel = last_mag = xTaskGetTickCount();
            
            lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_HIDDEN); // Free the top for the step prompt
            lv_label_set_text(instr_lbl, turns[0].prompt);
            lv_obj_align(instr_lbl, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_remove_flag(arrow_img, LV_OBJ_FLAG_HIDDEN); // Reveal the live arrow + bar
            lv_obj_remove_flag(prog_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(prog_bar, 0, LV_ANIM_OFF);
            lv_label_set_text(status_lbl, "Keep turning... 0%");
        } else {
            // Finish: only accept a plausible fit through all turns
            if (fit_ready) {
                mag_cal = fit;
                cal_complete = true;
                ecompass_nvs_save(&mag_cal); // Save

                // Clean up
                lv_obj_delete(cont);
                cont = NULL; title_lbl = instr_lbl = status_lbl = arrow_img = prog_bar = NULL;
                calibrating = false; init = false;
                ui_menu->page = ESPNOW_ECOMPASS_PAGE; // Back to the compass
            } else if (fit_uneven) { // Turned uneven this frame: show it before a SELECT can drop the run
                lv_label_set_text(status_lbl, "Near metal - SELECT: retry");
            } else {
                // Mid-turn: redo this turn (a disturbed read can leave wedges unreachable); finished turns stay
                cal_n = turn_n0;
                visited_sectors = 0;
                for (int k = 0; k < 3; k++) {
                    lo[k] = 1e9f;
                    hi[k] = -1e9f;
                }
                lv_bar_set_value(prog_bar, cal_turn * 100 / ECOMPASS_CAL_TURNS, LV_ANIM_OFF);
                lv_label_set_text(status_lbl, "Turn restarted");
            }
        }
    } else if (ui_btns->left_btn) { // Cancel / back out (the calibration in use is untouched until a run finishes)
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
    static float accel_up[3];          // Latest board-frame up vector, for tilt compensation
    static bool accel_up_valid = false; // Has an accel frame arrived since entry / re-zero?

    if (!init) {
        // Seed the calibration from NVS once per boot (redundant)
        if (!ecompass_loaded) {
            ecompass_loaded = true;
            if (ecompass_nvs_load(&mag_cal)) {
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
        accel_up_valid = false;

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
        accel_up_vector(&accel, accel_up);
        accel_up_valid = true;

        // Draws the X/Y/Z readout (Z = last heading) and hands back the tilt for the mag block below
        accel_apply_reading(&accel, accel_up, ball, ARROW_SIZE, val_lbl, disp_z, &disp_x, &disp_y);
        fresh_sample = true;
    }

    // Tilt-compensated compass heading from the calibrated magnetometer; skipped in a pose the mode can't read
    mmc5603_reading_t mag;
    float raw_heading;
    if (ball && xQueueReceive(xMagReadingsQueue, &mag, 0) == pdTRUE &&
            ecompass_heading(&mag, accel_up_valid ? accel_up : NULL, &raw_heading)) {
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
            const int32_t up_deg = ecompass_view_up_deg();
            lv_image_set_rotation(ball, ((int32_t)lroundf(rel * 10.0f) + (up_deg - ARC_TOP_DEG) * 10 + 3600) % 3600);
            arrow_drawn = rel;

            // Grow the rim arc to match: from straight-up (0) clockwise through the turn
            if (heading_arc) {
                int32_t rdeg = (int32_t)lroundf(rel);
                if (rdeg > 359) rdeg = 359;
                lv_arc_set_angles(heading_arc, up_deg, up_deg + rdeg);
            }

            // Drift the North pip
            if (heading_npip) {
                // arrow_heading is kept in [0,360), so 360 - it is the north bearing CW from "up"
                float north_deg = fmodf(360.0f - arrow_heading, 360.0f);
                float a = ((float)up_deg + north_deg) * DEG2RAD; // -> LVGL screen angle (0 = right, +y down)
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
        arrow_drawn = -1.0f; // Redraw the arrow/arc/pip for the new mode's viewer-up
    } else if (ui_btns->down_btn == 1) { // Previous view mode (wraps)
        accel_mode = (accel_mode + MODE_COUNT - 1) % MODE_COUNT;
        lv_label_set_text(mode_lbl, accel_mode_name(accel_mode));
        arrow_drawn = -1.0f; // Redraw the arrow/arc/pip for the new mode's viewer-up
    } else if (ui_btns->select_btn) { // Re-zero: recapture "straight ahead" without restarting the stream
        // Drop any queued samples so the new reference comes from a fresh reading
        xQueueReset(xAccelReadingsQueue);
        xQueueReset(xMagReadingsQueue);

        last_refresh = 0; // Trigger a fresh sample request on the next frame
        arrow_heading = 0.0f;
        heading_ref = 0.0f;
        heading_init = false; // Next mag sample becomes the new "straight ahead"
        arrow_drawn = -1.0f;  // Force the next mag frame to redraw the arrow/arc/pip
        disp_x = disp_y = disp_z = 0.0f;
        accel_up_valid = false; // The new reference uses a fresh up vector too

        // Snap the visuals back to the zeroed pose until fresh samples arrive
        lv_image_set_rotation(ball, (ecompass_view_up_deg() - ARC_TOP_DEG + 360) * 10 % 3600);
        if (heading_arc) {
            lv_arc_set_angles(heading_arc, ARC_TOP_DEG, ARC_TOP_DEG); // Zero-length = nothing drawn
        }
        if (heading_npip) {
            lv_obj_add_flag(heading_npip, LV_OBJ_FLAG_HIDDEN); // Hidden until a heading exists again
        }
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