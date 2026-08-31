#ifndef CTAP2_H
#define CTAP2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CTAP2 (FIDO2) second-factor authenticator.
 *
 * Shares the BLE transport, framing, key wrapping and attestation material with
 * the CTAP1/U2F path in u2f_core.c; only the payload inside a MSG frame differs,
 * being CBOR here and an ISO 7816 APDU there. Which one a request is routed to
 * comes from the revision the client selects on fidoServiceRevisionBitfield.
 *
 * Scope is deliberately second factor only: no discoverable credentials (so
 * credential storage stays O(1) through key wrapping) and no clientPIN. That is
 * enough for Windows to drive the device, which refuses a U2F-only BLE
 * authenticator outright. */

/* Commands, in the first byte of a request */
#define CTAP2_CMD_MAKE_CREDENTIAL 0x01
#define CTAP2_CMD_GET_ASSERTION   0x02
#define CTAP2_CMD_GET_INFO        0x04
#define CTAP2_CMD_CLIENT_PIN      0x06
#define CTAP2_CMD_RESET           0x07
#define CTAP2_CMD_GET_NEXT_ASSERT 0x08

/* Status byte, in the first byte of a response */
#define CTAP2_OK                        0x00
#define CTAP1_ERR_INVALID_COMMAND       0x01
#define CTAP1_ERR_INVALID_PARAMETER     0x02
#define CTAP1_ERR_INVALID_LENGTH        0x03
#define CTAP2_ERR_CBOR_UNEXPECTED_TYPE  0x11
#define CTAP2_ERR_INVALID_CBOR          0x12
#define CTAP2_ERR_MISSING_PARAMETER     0x14
#define CTAP2_ERR_CREDENTIAL_EXCLUDED   0x19
#define CTAP2_ERR_UNSUPPORTED_ALGORITHM 0x26
#define CTAP2_ERR_OPERATION_DENIED      0x27
#define CTAP2_ERR_UNSUPPORTED_OPTION    0x2B
#define CTAP2_ERR_INVALID_OPTION        0x2C
#define CTAP2_ERR_NO_CREDENTIALS        0x2E
#define CTAP2_ERR_PIN_AUTH_INVALID      0x33
#define CTAP2_ERR_PIN_NOT_SET           0x35
#define CTAP1_ERR_OTHER                 0x7F

/* authenticatorData flags */
#define CTAP2_FLAG_UP 0x01 // User presence verified
#define CTAP2_FLAG_UV 0x04 // User verified (PIN or biometric)
#define CTAP2_FLAG_AT 0x40 // Attested credential data is present

#define CTAP2_AAGUID_LEN 16

/* Model identifier, shared by every unit (u2f_attest.c) */
extern const uint8_t ctap2_aaguid[CTAP2_AAGUID_LEN];

/**
 * @brief Whether this request has to wait on the user-presence test
 *
 * @param [in] req Request bytes, command byte first
 * @param [in] req_len Length of req
 */
bool ctap2_core_needs_presence(const uint8_t *req, size_t req_len);

/**
 * @brief Process a CTAP2 request and build the response, status byte included
 *
 * @param [in] req Request bytes, command byte first
 * @param [in] req_len Length of req
 * @param [out] rsp Response buffer
 * @param [in] rsp_cap Capacity of rsp
 * @param [in] presence True if the user approved the presence test
 *
 * @return Number of bytes written to rsp (always >= 1)
 */
size_t ctap2_core_handle(const uint8_t *req, size_t req_len, uint8_t *rsp, size_t rsp_cap,
        bool presence);

#endif // CTAP2_H
