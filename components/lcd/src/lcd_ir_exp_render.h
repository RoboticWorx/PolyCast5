#ifndef LCD_IR_EXP_RENDER_H
#define LCD_IR_EXP_RENDER_H

#include <stdint.h>
#include <stdbool.h>

// The thermal page's render math, free of LVGL and ESP-IDF so the host tests can hold the browser's port of it
// (scripts/relay/decode.js) to pixel equality. Integer only: change one side and the other must follow.

// Four source samples and their Q8 weights for one output pixel
typedef struct {
    int16_t idx[4];
    int16_t w[4];
} irx_tap_t;

// One upscale: the tap tables, the scratch row buffer and the sizes they were built for
typedef struct {
    const irx_tap_t *tap_x; // dst_w entries
    const irx_tap_t *tap_y; // dst_h entries
    int16_t *mid;           // src_h x dst_w intermediate
    int src_w;
    int src_h;
    int dst_w;
    int dst_h;
} irx_scaler_t;

/**
 * @brief Build one axis of Catmull-Rom taps
 *
 * @param [out] taps  dst_n entries
 * @param [in]  dst_n Output pixels on this axis
 * @param [in]  src_n Source samples on this axis
 * @param [in]  flip  Mirror the axis
 */
void irx_render_taps(irx_tap_t *taps, int dst_n, int src_n, bool flip);

/**
 * @brief Upscale one frame and map it through the palette
 *
 * @param [in]  s       Taps and scratch for this frame's size
 * @param [in]  src     src_w x src_h samples, row-major
 * @param [in]  lo      Sample value drawn as palette[0]
 * @param [in]  hi      Sample value drawn as palette[255]
 * @param [in]  palette 256 RGB565 entries
 * @param [out] out     dst_h rows of dst_w pixels
 * @param [in]  stride  Pixels between rows of out
 */
void irx_render_frame(const irx_scaler_t *s, const int16_t *src, int32_t lo, int32_t hi,
        const uint16_t *palette, uint16_t *out, int stride);

/**
 * @brief Draw the centre reticle over a rendered frame
 */
void irx_render_crosshair(uint16_t *out, int stride, int w, int h, uint16_t fg, uint16_t bg);

#endif // LCD_IR_EXP_RENDER_H
