// Custom ESP-NOW actually secure PolyCast5 protocol!
// ┌────────────────────────────────────────────────────────────────────────┐
// │           ESP-NOW Auth — AES-128-CCM, associated-data header           │
// ├────────────────────────────────────────────────────────────────────────┤
// │                    Command Frame (18 bytes on air)                     │
// ├──────────────────────────────┬─────────────┬───────────────────────────┤
// │     Associated Data (9B)     │ Ciphertext  │         MIC (8B)          │
// │          cleartext           │    (1B)     │         Auth Tag          │
// ├──────────────┬───────────────┼─────────────┼───────────────────────────┤
// │    Magic     │    Counter    │   Command   │                           │
// │      5B      │      4B       │     1B      │                           │
// │   "PC5: "    │   uint32 BE   │    uint8    │                           │
// └──────────────┴───────────────┴─────────────┴───────────────────────────┘

#ifndef ESPNOW_AUTH_H
#define ESPNOW_AUTH_H

#include <stdbool.h>
#include <stdint.h>

#include "espnow_utils.h"

/*
 * Authenticated ESP-NOW command frames.
 *
 * ESP-NOW's own link-layer encryption cannot be relied on for authenticity. Espressif's
 * reference CCMP decrypt sets the CCM tag length to zero for ESP-NOW frames, so the MIC
 * that is transmitted is never verified, and on ESP32-C5 the receive path hands the frame
 * to the Wi-Fi MAC hardware where the behaviour cannot be audited at all. On top of that
 * there is no replay check anywhere in ESP-NOW: a recorded frame rebroadcast verbatim is
 * accepted. Link-layer encryption is still worth enabling for confidentiality, but the
 * decision to act on a command has to rest on something verifiable, so authenticity and
 * freshness are handled here, above the link layer, the same way lora_pcp.c does it.
 *
 * Wire format, sent in place of the plain "PC5: <cmd>" text whenever the peer has an LMK:
 *
 *   offset  size  field
 *   0       5     ESPNOW_MAGIC ("PC5: "), cleartext
 *   5       4     counter, big-endian uint32, cleartext
 *   9       1     command byte, AES-CCM ciphertext
 *   10      8     AES-CCM MIC over the command, with bytes 0..8 as associated data
 *
 * The header is cleartext but authenticated as associated data, so the counter cannot be
 * edited in flight. The peer accepts a frame only if the MIC verifies
 * and the counter is strictly greater than the last one it accepted.
 *
 * The CCM key is the peer's 16-byte LMK, used directly. ESP-NOW never uses the LMK raw
 * itself - its own CCMP key is the LMK encrypted under the PMK - and the two nonce spaces
 * cannot collide, since a CCMP nonce carries the source MAC where ours carries zeros.
 *
 * The nonce is 13 bytes: nine zeros followed by the big-endian counter. The counter is
 * persisted and only ever increases, so a (key, nonce) pair is never reused.
 */

#define ESPNOW_AUTH_CTR_LEN   4
#define ESPNOW_AUTH_MIC_LEN   8
#define ESPNOW_AUTH_NONCE_LEN 13

// Cleartext header that is authenticated as associated data: magic | counter
#define ESPNOW_AUTH_HDR_LEN (ESPNOW_MAGIC_LEN + ESPNOW_AUTH_CTR_LEN)

// Full frame as it goes on the air
#define ESPNOW_AUTH_FRAME_LEN (ESPNOW_AUTH_HDR_LEN + 1 + ESPNOW_AUTH_MIC_LEN)

/**
 * @brief Load the persisted send counter and reserve a fresh block of values
 *
 * Call once at start-up, before the first espnow_auth_build_frame(). Values are reserved
 * ahead of use so an unexpected reset can only skip counter values, never repeat one.
 */
void espnow_auth_load_counter_nvs(void);

/**
 * @brief Build an authenticated command frame for a peer
 *
 * Consumes one counter value on success.
 *
 * @param [in] lmk 16-byte local master key shared with the peer
 * @param [in] cmd Command byte to send
 * @param [out] frame Destination buffer of at least ESPNOW_AUTH_FRAME_LEN bytes
 *
 * @return true if the frame was built
 */
bool espnow_auth_build_frame(const uint8_t *lmk, uint8_t cmd, uint8_t *frame);

#endif // ESPNOW_AUTH_H
