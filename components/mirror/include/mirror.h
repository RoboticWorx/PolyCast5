#ifndef MIRROR_H
#define MIRROR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Panel geometry. Asserted against HOR_RES/VER_RES where the capture hook is installed
#define MIRROR_SCR_W 240
#define MIRROR_SCR_H 135

// Tile grid: 240/16 and 135/15 both divide exactly, so there are no partial tiles
#define MIRROR_TILE_W 16
#define MIRROR_TILE_H 15
#define MIRROR_GRID_COLS (MIRROR_SCR_W / MIRROR_TILE_W) // 15
#define MIRROR_GRID_ROWS (MIRROR_SCR_H / MIRROR_TILE_H) // 9
#define MIRROR_TILE_CNT (MIRROR_GRID_COLS * MIRROR_GRID_ROWS) // 135
#define MIRROR_TILE_PX (MIRROR_TILE_W * MIRROR_TILE_H) // 240

// Quality ladder. Higher levels quantize harder and, at level 3, halve the resolution
typedef enum {
    MIRROR_Q_EXACT = 0, // RGB565 untouched
    MIRROR_Q_G5,        // Green dropped to 5 bits, visually lossless
    MIRROR_Q_444,       // RGB444 in 565, slight banding on gradients
    MIRROR_Q_444_HALF,  // RGB444 at 2x downscale, for busy screens
    MIRROR_Q_COUNT
} mirror_quality_t;

typedef enum {
    MIRROR_OFF = 0,
    MIRROR_STARTING, // Wi-Fi up, relay connecting
    MIRROR_WAITING,  // Relay up, pairing code shown, no viewer yet
    MIRROR_PENDING,  // Viewer attached, waiting for the user to approve it on the device
    MIRROR_LIVE,     // Viewer approved, streaming
    MIRROR_ERROR,
} mirror_state_t;

// Read from anywhere; written only by the mirror task
extern volatile mirror_state_t mirror_state;

// Pairing code the relay issued, NUL-terminated and empty until MIRROR_WAITING
extern char mirror_code[12];

// The relay's 3-char viewer check code, shown on the device so the user can confirm the
// attached viewer before approving. Fresh on every attach until an approval locks the
// relay. NUL-terminated, empty when no viewer is attached
extern char mirror_check[4];

// Last error shown on the pairing page, empty when there is none
extern char mirror_error[40];

/**
 * @brief Whether the flush hook should be capturing
 *
 *        Deliberately trivial: this runs on every LVGL flush, so when mirroring is off
 *        the whole feature costs one load and one branch.
 */
static inline bool mirror_is_active(void)
{
    // ERROR counts as stopped. Nothing clears it until the user acts, so treating it as
    // running would leave the capture hook and the indicator going for the rest of the boot
    return (mirror_state == MIRROR_STARTING ||
            mirror_state == MIRROR_WAITING ||
            mirror_state == MIRROR_PENDING ||
            mirror_state == MIRROR_LIVE);
}

/**
 * @brief Copy one flushed region into the shadow framebuffer and mark its tiles dirty
 *
 *        Called from the LVGL flush callback on lcd_task. Coordinates are in logical
 *        240x135 space; px is native little-endian RGB565, width*height pixels, with no
 *        row padding. Does nothing but memcpy and bit-sets: no allocation, no logging,
 *        no LVGL calls.
 */
void mirror_capture(int16_t x1, int16_t y1, int16_t x2, int16_t y2, const uint16_t *px);

/**
 * @brief Mark every tile dirty, so the next encode emits a full frame
 *
 *        Needed on session start (the shadow is only as complete as what LVGL has
 *        repainted since boot) and whenever the quality level changes, since the
 *        reference buffer holds values quantized at the old level.
 */
void mirror_force_keyframe(void);

/**
 * @brief Start a mirror session: connect to the relay and ask for a pairing code
 *
 *        Requires an associated Wi-Fi STA. Safe to call when already running.
 */
esp_err_t mirror_start(void);

/**
 * @brief Tear the session down and release anything a remote viewer was holding
 *
 * @param [in] reason One of MIRROR_BYE_*, sent to the viewer so it can explain itself
 */
void mirror_stop(uint8_t reason);

/**
 * @brief Whether an approved viewer is attached and being streamed to
 */
bool mirror_has_viewer(void);

/**
 * @brief Approve the attached viewer, from lcd_task when the user presses SELECT on the
 *        mirror page during MIRROR_PENDING
 *
 *        Ignored unless check is still mirror_check, so a viewer that replaced the one on
 *        screen is never approved unseen. The mirror task then asks the relay to lock the
 *        session to that viewer and streams nothing until the relay confirms: from then on
 *        only its resume token re-attaches, carrying the same check, and goes straight to
 *        LIVE. Until the lock lands every attach is a new viewer with a fresh check, and
 *        any other check voids the approval. Approval otherwise lasts until the relay
 *        connection drops (a new connection means a new code) or the session ends.
 *
 * @param [in] check The check code the page showed when SELECT was pressed
 */
void mirror_viewer_approve(const char *check);

/**
 * @brief Whether the relay has confirmed its lock to the approved viewer
 *
 *        Stays set while that viewer is away, when the pairing code admits no one else.
 *        Cleared with the approval.
 */
bool mirror_relay_locked(void);

/**
 * @brief Take a pending "sleep the device" request from the viewer, clearing it
 *
 *        The mirror cannot sleep the device itself: that is the LCD task's job and it
 *        owns LVGL. The LCD task polls this and raises a power press, so the request
 *        walks exactly the same path as the physical button.
 *
 * @return true if a request was pending
 */
bool mirror_take_sleep_request(void);

// Thermal channel. The thermal page hands over the raw sensor frame it rendered and the
// browser upscales it with the same integer math, so sensor noise no longer resends the
// canvas tile by tile. Tiles an object drawn over the canvas touches stay on the tile path
#define MIRROR_THERMAL_MAX_PX    (32 * 24) // Largest source frame
#define MIRROR_THERMAL_MAX_HOLES 4 // Overlays tracked at once; the caller merges the rest

// Inclusive screen rectangle, as lv_area_t
typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
} mirror_rect_t;

// One rendered thermal frame, with everything the canvas was drawn from
typedef struct {
    const int16_t *px;       // cols x rows samples, row-major, before any flip
    uint8_t cols;
    uint8_t rows;
    int32_t lo;              // Sample drawn as palette[0]
    int32_t hi;              // Sample drawn as palette[255]
    const uint16_t *palette; // 256 RGB565 entries
    bool flip_h;
    bool flip_v;
    bool crosshair;
    uint16_t cross_fg;       // Crosshair core, RGB565
    uint16_t cross_bg;       // Crosshair edge, RGB565
} mirror_thermal_frame_t;

/**
 * @brief Publish the frame the thermal canvas was just rendered from
 *
 *        lcd_task only. A copy into a seqlocked slot: no allocation, no blocking, no
 *        LVGL. mirror_task sends the latest and drops any it missed.
 */
void mirror_thermal_frame(const mirror_thermal_frame_t *f);

/**
 * @brief Say whether the thermal canvas shows a published frame, where it is, and what
 *        is drawn over it
 *
 *        lcd_task only. Unchanged arguments cost a compare, so call it every tick.
 *
 * @param [in] on      Canvas visible with a published frame on it
 * @param [in] canvas  Canvas area on screen
 * @param [in] holes   Areas of objects drawn over the canvas
 * @param [in] n_holes Up to MIRROR_THERMAL_MAX_HOLES; more turns the channel off
 */
void mirror_thermal_view(bool on, const mirror_rect_t *canvas, const mirror_rect_t *holes,
        size_t n_holes);

/**
 * @brief End the thermal channel and forget its frame, when the page goes away
 *
 *        lcd_task only. Every tile the channel covered is resent through the tile path.
 */
void mirror_thermal_stop(void);

// Remote text entry, registered by the LCD component at init. Keeping this a sink rather
// than a direct call is what stops the mirror and lcd components depending on each other
typedef struct {
    size_t (*type)(const char *ascii, size_t len);
    void (*key)(int key); // An lcd_ti_remote_key_t value
    bool (*state)(bool *out_sensitive, char *out_buf, size_t out_size);
} mirror_text_sink_t;

/**
 * @brief Register the on-device text entry the remote keyboard types into
 *
 * @param [in] sink Borrowed, must outlive the session; NULL disables remote typing
 */
void mirror_set_text_sink(const mirror_text_sink_t *sink);

#endif // MIRROR_H
