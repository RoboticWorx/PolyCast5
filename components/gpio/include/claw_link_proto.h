#ifndef CLAW_LINK_PROTO_H
#define CLAW_LINK_PROTO_H

/**
 * Wire protocol between PolyCast5 (I2C master) and the PolyCast5-Claw hardware
 * expansion (I2C slave).
 *
 * This file is shared verbatim with the PolyCast5-Claw firmware. Keep both
 * copies byte-identical:
 *   PolyCast5      components/gpio/include/claw_link_proto.h
 *   PolyCast5-Claw application/edge_agent/components/host_i2c_link/include/claw_link_proto.h
 *
 * Transaction shape (matches the existing I2C terminal page): the master
 * writes one framed command, releases the bus, waits CLAW_LINK_TURNAROUND_MS,
 * then reads exactly CLAW_LINK_FRAME_LEN bytes.
 *
 * The slave builds its reply only once the read actually starts - the ESP32-C5
 * I2C slave stretches SCL on address match, so the master's read blocks until
 * the slave answers rather than returning stale data.
 */

#include <stdint.h>

// Slave address of the Claw expansion
// Picked from 0x08-0x0F, the sparsest legal window: 0x00-0x07 and 0x78-0x7F are
// reserved by the I2C spec, 0x08 is the SMBus host, 0x0B is the LC709203F fuel
// gauge, and 0x0C-0x0F cover the SMBus alert response address plus the
// AK8963/MLX90393 magnetometers. On-board devices are at 0x19, 0x20 and 0x30,
// and 0x2A is the generic external-hardware convention used by the I2C terminal
#define CLAW_LINK_I2C_ADDR 0x0A

/* Opcodes: byte 0 of every master -> slave write */
#define CLAW_LINK_OP_PING   0x01 // No payload: presence / liveness check
#define CLAW_LINK_OP_BEGIN  0x02 // [kind]: reset the assembly buffer
#define CLAW_LINK_OP_CHUNK  0x03 // [len][data...]: append to the assembly buffer
#define CLAW_LINK_OP_END    0x04 // No payload: dispatch the assembled payload by kind
#define CLAW_LINK_OP_STATUS 0x05 // [page]: stage the status frame for the next read
#define CLAW_LINK_OP_ABORT  0x06 // No payload: cancel the in-flight request

/* Payload kinds: argument to CLAW_LINK_OP_BEGIN */
#define CLAW_LINK_KIND_COMMAND 0x00 // Raw UTF-8 text -> agent
#define CLAW_LINK_KIND_WIFI    0x01 // [ssid_len][ssid][pass_len][pass]
#define CLAW_LINK_KIND_LLM     0x02 // [key_len][key][model_len][model][url_len][url]

// Max payload bytes carried by a single CLAW_LINK_OP_CHUNK write
#define CLAW_LINK_CHUNK_MAX 64

// Max size of a fully assembled inbound payload
#define CLAW_LINK_PAYLOAD_MAX 1024

/* Slave -> master status frame: always exactly CLAW_LINK_FRAME_LEN bytes.
 *
 * Sized to fit the ESP32-C5's 32-byte hardware I2C TX FIFO in a single
 * i2c_slave_write(). That driver only copies into the FIFO when the whole request
 * fits the free space (len < free_fifo_len), so anything 32 bytes or larger lands
 * in the software ring instead and the read then depends on the TX-watermark ISR
 * refilling the FIFO mid-transaction. Keeping a frame FIFO-resident removes that
 * dependency completely, at the cost of more pages for long text. */
#define CLAW_LINK_FRAME_LEN     31
#define CLAW_LINK_FRAME_HDR_LEN 10
#define CLAW_LINK_FRAME_TEXT_MAX (CLAW_LINK_FRAME_LEN - CLAW_LINK_FRAME_HDR_LEN) // 21

#define CLAW_LINK_MAGIC   0xC1
#define CLAW_LINK_VERSION 0x01

/* Status frame byte offsets */
#define CLAW_LINK_OFF_MAGIC    0
#define CLAW_LINK_OFF_VERSION  1
#define CLAW_LINK_OFF_STATE    2
#define CLAW_LINK_OFF_FLAGS    3
#define CLAW_LINK_OFF_SEQ      4 // Bumped whenever the display text changes
#define CLAW_LINK_OFF_PAGE     5 // Echoes the requested page
#define CLAW_LINK_OFF_TEXT_LEN 6 // Bytes of text in this frame (0..CLAW_LINK_FRAME_TEXT_MAX)
#define CLAW_LINK_OFF_RESERVED 7
#define CLAW_LINK_OFF_TOTAL_LO 8 // Total display text length, uint16 little-endian
#define CLAW_LINK_OFF_TOTAL_HI 9
#define CLAW_LINK_OFF_TEXT     10

/* Agent states reported in CLAW_LINK_OFF_STATE */
#define CLAW_LINK_STATE_IDLE      0 // Ready for a command
#define CLAW_LINK_STATE_RECEIVING 1 // Mid-transfer (BEGIN seen, END not yet)
#define CLAW_LINK_STATE_QUEUED    2 // Handed to the event router
#define CLAW_LINK_STATE_RUNNING   3 // Agent working; display text is a live stage update
#define CLAW_LINK_STATE_DONE      4 // Display text is the final answer
#define CLAW_LINK_STATE_ERROR     5 // Display text is an error message

/* Bit flags reported in CLAW_LINK_OFF_FLAGS */
#define CLAW_LINK_FLAG_WIFI_CONNECTED (1U << 0) // Claw has joined the host's network
#define CLAW_LINK_FLAG_LLM_CONFIGURED (1U << 1) // Claw has a usable LLM config
#define CLAW_LINK_FLAG_BUSY           (1U << 2) // A request is in flight

// Max display text the master will assemble across pages. Each page costs a round trip,
// so this is deliberately sized for what the host's small screen can usefully show
// rather than for the longest answer the agent could produce
#define CLAW_LINK_TEXT_MAX 256

// How long the master waits after a write before starting the following read.
// Ordering is already guaranteed on the slave side (received bytes and read
// requests share one queue), so this is margin rather than a correctness
// requirement: it just lets the slave digest the STATUS opcode before its read
// arrives, instead of stretching the clock while it catches up
#define CLAW_LINK_TURNAROUND_MS 20

#endif // CLAW_LINK_PROTO_H
