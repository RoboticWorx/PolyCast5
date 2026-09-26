#ifndef MIRROR_PRIV_H
#define MIRROR_PRIV_H

#include <stdint.h>
#include <stdbool.h>

#include "mirror.h"

// Shared between the capture and encode translation units only

// Colour the shadow and the stream hold instead of the real pixels while a credential
// screen is up. Filled at capture time, so a secret never reaches the shadow
#define MIRROR_REDACT_COLOR 0x2104 // Dark grey in RGB565

// What the tile stream carries under the canvas while the viewer paints thermal frames
// there. Constant, so the sensor noise LVGL flushes never reaches the diff
#define MIRROR_THERMAL_FILL 0x0000

// One bit per tile, as the dirty and reference sets use
#define MIRROR_TILE_WORDS ((MIRROR_TILE_CNT + 31) / 32)

/**
 * @brief The live screen, written only by the flush hook on lcd_task
 */
const uint16_t *mirror_shadow(void);

/**
 * @brief What the viewer is known to hold, quantized at the level it was sent with.
 *        Written only by the mirror task.
 */
uint16_t *mirror_ref(void);

/**
 * @brief Take ownership of a dirty tile, clearing its bit
 *
 * @return true if the tile was dirty and is now claimed
 */
bool mirror_dirty_claim(uint16_t tile);

/**
 * @brief Put a tile back in the dirty set, after a torn or dropped encode
 */
void mirror_dirty_restore(uint16_t tile);

/**
 * @brief Whether any tile is dirty
 */
bool mirror_dirty_any(void);

/**
 * @brief Whether the reference buffer holds what the viewer actually has for this tile
 *
 *        Cleared whenever that stops being true: a forced keyframe (a fresh viewer's
 *        canvas is blank), a quality change (the reference is quantized at the old
 *        level), or a send that failed after the reference had already advanced.
 *        An invalid tile is always re-encoded, whatever the pixels compare to.
 */
bool mirror_ref_is_valid(uint16_t tile);

/**
 * @brief Mark the reference good for a tile, after it has been encoded and updated
 */
void mirror_ref_mark_valid(uint16_t tile);

/**
 * @brief Drop a tile's reference, forcing it to be re-encoded next frame
 */
void mirror_ref_invalidate(uint16_t tile);

/**
 * @brief Repaint counter for a tile, bumped by every capture that touches it
 *
 *        Read before and after encoding a tile: a change means lcd_task repainted it
 *        mid-copy, so the encoded bytes are torn and must be discarded.
 */
uint8_t mirror_tile_gen(uint16_t tile);

/**
 * @brief Quantize, diff and encode every dirty tile into the frame arena
 *
 *        Quantizing BEFORE the comparison is what makes the thermal imager affordable:
 *        its IIR auto-range shifts every pixel by about one palette step per frame, so
 *        an exact diff would mark every tile dirty forever.
 *
 * @param [in] q Quality level, which sets the quantize mask and 2x downscale
 *
 * @return Number of tiles encoded; 0 means nothing changed and no frame is owed
 */
uint16_t mirror_encode_frame(mirror_quality_t q);

/**
 * @brief How many messages the encoded frame needs
 */
uint16_t mirror_encode_msg_count(void);

/**
 * @brief Serialize one message of the encoded frame
 *
 * @param [in]  index    Message index, 0 .. mirror_encode_msg_count() - 1
 * @param [in]  seq      Frame sequence number, shared by every message of the frame
 * @param [in]  keyframe Whether to flag this frame as a full repaint
 * @param [in]  q        The level the frame was encoded at
 * @param [out] out      Destination, at least MIRROR_MSG_MAX_BYTES
 * @param [in]  cap      sizeof(out)
 *
 * @return Bytes written, or 0 if the index or capacity is wrong
 */
size_t mirror_encode_build_msg(uint16_t index, uint16_t seq, bool keyframe,
        mirror_quality_t q, uint8_t *out, size_t cap);

/**
 * @brief Put every tile of a message back in the dirty set, after a failed send
 */
void mirror_encode_restore_msg(uint16_t index);

/**
 * @brief Hold the canvas part of these tiles at MIRROR_THERMAL_FILL while encoding
 *
 *        mirror_task only, which also runs the encoder
 *
 * @param [in] tiles  MIRROR_TILE_WORDS words, one bit per tile; NULL stops
 * @param [in] canvas The canvas area; ignored when tiles is NULL
 */
void mirror_encode_set_thermal(const uint32_t *tiles, const mirror_rect_t *canvas);

/**
 * @brief Build the MIRROR_MSG_THERMAL message the viewer is owed, if any
 *
 *        mirror_task only. Latest wins: frames published since the last call are dropped.
 *        While redacted the only message is the one ending the channel. Nothing is
 *        adopted until mirror_thermal_commit()
 *
 * @param [in]  keyframe The viewer is being repainted: resend it all, palette included
 * @param [in]  seq      Sequence number to stamp, shared with FRAME
 * @param [out] out      Destination, at least MIRROR_MSG_MAX_BYTES
 * @param [in]  cap      sizeof(out)
 *
 * @return Bytes written; 0 when nothing is owed
 */
size_t mirror_thermal_encode(bool keyframe, uint16_t seq, uint8_t *out, size_t cap);

/**
 * @brief The message last built reached the socket: adopt it
 *
 *        Tiles that changed hands are dirtied with their reference dropped, so the next
 *        frame resends them the way the viewer now paints them
 */
void mirror_thermal_commit(void);

/**
 * @brief Forget what any viewer was told, for a new session
 */
void mirror_thermal_reset(void);

#endif // MIRROR_PRIV_H
