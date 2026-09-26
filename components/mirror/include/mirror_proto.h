#ifndef MIRROR_PROTO_H
#define MIRROR_PROTO_H

#include <stdint.h>

// Screen Mirror wire format. Every multi-byte field is little-endian, which is native
// on the C5 and on every browser we care about, so neither side ever byte-swaps.
// RGB565 pixels are little-endian too: the big-endian swap the panel needs happens
// later, inside spi_master_write_colors(), and the mirror never pays for it.

#define MIRROR_PROTO_VER 1

// Device -> browser
#define MIRROR_MSG_FRAME     0x01
// 0x02 reserved (was MIRROR_MSG_HELLO); do not reuse
#define MIRROR_MSG_STATUS    0x03
#define MIRROR_MSG_TEXTSTATE 0x04
#define MIRROR_MSG_BYE       0x05
#define MIRROR_MSG_THERMAL   0x06

// Browser -> device
#define MIRROR_MSG_BTN          0x10
#define MIRROR_MSG_ACK          0x11
#define MIRROR_MSG_TEXT         0x12
#define MIRROR_MSG_KEYFRAME_REQ 0x13
#define MIRROR_MSG_QUALITY      0x14
#define MIRROR_MSG_TEXTKEY      0x15
#define MIRROR_MSG_END_SESSION  0x16

// MIRROR_MSG_STATUS payload: [0x03, state]. Sent after every attach, and LIVE once the relay
// confirms its lock to the approved viewer
#define MIRROR_STATUS_PENDING 0 // Attached, waiting for approval on the device; no frames yet
#define MIRROR_STATUS_LIVE    1 // Approved; frames follow

// FRAME header flags
#define MIRROR_FLAG_KEYFRAME   0x01
#define MIRROR_FLAG_HALF_SCALE 0x02
#define MIRROR_FLAG_LAST_MSG   0x04 // Last message of this frame; the viewer ACKs on it
#define MIRROR_FLAG_REDACTED   0x08 // A credential screen is up; pixels are a placeholder
#define MIRROR_FLAG_LEVEL_SHIFT 4   // Quality level in the high nibble

// Tile encodings
#define MIRROR_ENC_RAW    0 // Pixels verbatim
#define MIRROR_ENC_SOLID  1 // One colour
#define MIRROR_ENC_RLE_H  2 // Row-major runs: menus, text, thermal gradients
#define MIRROR_ENC_PAL4   3 // <=16 colours as a palette plus 4bpp indices: antialiased text
#define MIRROR_ENC_RLE_V  4 // Column-major runs: DOOM's raycast columns are vertically flat

// FRAME header, then tile_count tile records
//   u8  type, u8 flags, u16 seq, u8 tile_w, u8 tile_h, u8 cols, u8 rows, u16 tile_count
// Tile record:
//   u16 tile_index, u8 enc, u16 payload_len, u8 payload[payload_len]
#define MIRROR_FRAME_HDR_BYTES 10
#define MIRROR_TILE_HDR_BYTES 5

// Tiles per message, bounding one message at 10 + 8 * (5 + 480) = 3,890 B. Sized to stay
// under 4096 so a message is one TLS record (MBEDTLS_SSL_OUT_CONTENT_LEN) and one
// websocket client write (its buffer_size chunks anything larger, and each chunk gets the
// full send timeout)
#define MIRROR_TILES_PER_MSG 8
#define MIRROR_MSG_MAX_BYTES (MIRROR_FRAME_HDR_BYTES + \
        MIRROR_TILES_PER_MSG * (MIRROR_TILE_HDR_BYTES + MIRROR_TILE_PX * 2))

// THERMAL: the thermal page's raw sensor frame. The browser upscales and colours it with
// the device's own integer math (lcd_ir_exp_render.c, ported in scripts/relay/decode.js)
// and paints it over the canvas part of every tile set in tiles[]; the device holds those
// parts at a constant in the tile stream. Shares the FRAME sequence and is ACKed the same way
//   u8  type, u8 flags, u16 seq
// then, only with MIRROR_THERMAL_ON:
//   u8  x, u8 y, u8 w, u8 h                   canvas on screen
//   u8  cols, u8 rows                         source frame
//   i32 lo, i32 hi                            samples drawn as palette[0] and palette[255]
//   u16 cross_fg, u16 cross_bg                crosshair core and edge, RGB565
//   u8  tiles[MIRROR_THERMAL_TILE_BYTES]      bit t, LSB first: tile t shows this frame
//   u16 palette[256]                          only with MIRROR_THERMAL_PALETTE
//   i16 px[cols * rows]                       row-major, before any flip
// Without ON the channel has ended: stop painting, the tiles it covered are being resent.
// The palette comes on every OFF to ON and every keyframe; otherwise keep the last one
#define MIRROR_THERMAL_ON        0x01
#define MIRROR_THERMAL_PALETTE   0x02
#define MIRROR_THERMAL_FLIP_H    0x04
#define MIRROR_THERMAL_FLIP_V    0x08
#define MIRROR_THERMAL_CROSSHAIR 0x10

#define MIRROR_THERMAL_OFF_BYTES  4
#define MIRROR_THERMAL_TILE_BYTES ((MIRROR_TILE_CNT + 7) / 8)
#define MIRROR_THERMAL_HDR_BYTES  (22 + MIRROR_THERMAL_TILE_BYTES)

// Button ids, deliberately gpio_task.c's array order so nothing has to translate
#define MIRROR_BTN_SELECT 0
#define MIRROR_BTN_HOME   1
#define MIRROR_BTN_UP     2
#define MIRROR_BTN_DOWN   3
#define MIRROR_BTN_LEFT   4
#define MIRROR_BTN_RIGHT  5
#define MIRROR_BTN_POWER  6 // Reserved and rejected: see btn_pin()/handle_input() in mirror_task.c

// MIRROR_MSG_BTN payload: [0x10, id, action]. A click released within 250 ms is one TAP;
// a longer press is DOWN at 250 ms, re-asserted every 400 ms (GPIO_REMOTE_HOLD_MAX_MS
// releases it otherwise), then UP
#define MIRROR_BTN_ACTION_UP   0
#define MIRROR_BTN_ACTION_DOWN 1
#define MIRROR_BTN_ACTION_TAP  2

// Remote text control keys
#define MIRROR_TEXTKEY_BACKSPACE 0
#define MIRROR_TEXTKEY_SUBMIT    1
#define MIRROR_TEXTKEY_CANCEL    2

// BYE reasons
#define MIRROR_BYE_USER      0
#define MIRROR_BYE_SLEEP     1
#define MIRROR_BYE_LOW_BATT  2
#define MIRROR_BYE_WIFI_LOST 3
// 4 reserved (was MIRROR_BYE_PORTAL); do not reuse
#define MIRROR_BYE_TIMEOUT   5
#define MIRROR_BYE_DENIED    6 // The user refused a pending viewer on the device

#endif // MIRROR_PROTO_H
