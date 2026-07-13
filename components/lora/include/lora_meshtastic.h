#ifndef LORA_MESHTASTIC_H
#define LORA_MESHTASTIC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sx126x.h"
#include "lora_pcp.h" // lora_region_t (US/EU frequency slot)

// ───────────────────────────────────────────────────────────────────────────
// Meshtastic interop — default public LongFast channel, US 902–928 MHz band.
// All constants verified byte-for-byte against meshtastic/firmware (master):
//   PHY      MeshRadio.h::modemPresetToParams  → LONG_FAST = BW 250k / SF11 / CR 4/5
//   freq     RadioInterface.cpp                → slot 19 → 906.875 MHz
//   sync     RadioLibInterface.h setSyncWord(0x2b) → SX126x reg 0x24B4
//   channel  Channels.cpp xorHash("LongFast") ^ xorHash(defaultPSK) = 0x08
//   crypto   CryptoEngine.cpp AES-128-CTR, nonce = packetId(8 LE)|from(4 LE)|0..0
// ───────────────────────────────────────────────────────────────────────────

#define MESHTASTIC_LORA_FREQ_HZ    906875000UL // LongFast US default slot (channel_num 19)
#define MESHTASTIC_LORA_FREQ_EU_HZ 869525000UL // LongFast EU_868 slot (869.4-869.65 MHz band, only slot)
#define MESHTASTIC_SYNC_WORD       0x2B        // -> SX126x sync reg 0x24B4 (matches RadioLib)
#define MESHTASTIC_PREAMBLE_SYMB   16          // symbols (all presets)
#define MESHTASTIC_CHANNEL_HASH    0x08        // PacketHeader.channel for the LongFast channel
#define MESHTASTIC_MAX_PACKET_LEN  255         // LoRa PHY max: 16-byte header + payload

// PortNums (meshtastic/portnums.proto)
#define MESHTASTIC_PORT_TEXT       1
#define MESHTASTIC_PORT_POSITION   3
#define MESHTASTIC_PORT_NODEINFO   4

// Recent-message store (shared with the web portal)
// The portal only needs the text conversation, so we keep the last MESHTASTIC_MSG_LOG_CAP text messages in a small ring buffer.
#define MESHTASTIC_MSG_LOG_CAP  16  // Ring buffer depth
#define MESHTASTIC_RX_TEXT_MAX  200 // Stored/sent text is truncated to this many bytes

/**
 * @brief One stored Meshtastic text message (inbound or our own outbound)
 */
typedef struct {
    uint32_t seq;       // Monotonically increasing id (0 = unused); newest = largest
    uint32_t id;        // Meshtastic packet id (used to match a relayed self-echo as an ACK)
    uint32_t from_node; // Sender node number (our node number for outbound)
    bool     outbound;  // True if we sent it, false if received
    bool     acked;     // Outbound only: heard our own packet relayed back = delivered
    bool     failed;    // Outbound only: the radio never actually put it on air
    int8_t   rssi;      // Packet RSSI in dBm (0 for outbound)
    int8_t   snr;       // Packet SNR in dB (0 for outbound)
    uint8_t  hops;      // Hops taken to reach us (0 = heard directly); inbound only
    char     text[MESHTASTIC_RX_TEXT_MAX + 1];
} lora_meshtastic_msg_t;

// Nodes-heard roster (shared with the web portal)
// Every packet we hear updates the sender's signal + last-heard; NodeInfo packets fill in the names
#define MESHTASTIC_NODE_MAX        32 // Roster capacity (oldest evicted when full)
#define MESHTASTIC_NODE_LONG_MAX   20 // Stored long_name truncated to this many bytes
#define MESHTASTIC_NODE_SHORT_MAX  8  // Stored short_name truncated to this many bytes

/**
 * @brief One node we've heard on the mesh (snapshot for the web portal)
 */
typedef struct {
    uint32_t node_num;   // Sender node number
    char     long_name[MESHTASTIC_NODE_LONG_MAX + 1];   // "" until a NodeInfo arrives
    char     short_name[MESHTASTIC_NODE_SHORT_MAX + 1]; // "" until a NodeInfo arrives
    int8_t   rssi;       // Best (strongest) RSSI seen, dBm
    int8_t   snr;        // Best (highest) SNR seen, dB
    uint8_t  hops;       // Hops of the most recent packet (0 = heard directly)
    uint32_t age_s;      // Seconds since last heard
} lora_meshtastic_node_t;

/**
 * @brief Runtime Meshtastic-mode flag.
 *
 * Chooses what the LoRa radio does. lora_task sets this at startup from the
 * persisted Meshtastic toggle (lora_meshtastic_portal_enabled_load_nvs()); the
 * LCD toggle saves the flag and reboots, so a change takes effect on next boot.
 *   true  -> Meshtastic mode enabled for LoRa radio
 *   false -> Normal PolyCast LoRa (PCP)
 */
extern volatile bool g_meshtastic_mode;

/**
 * @brief Fill SX126x modem/packet params + RF frequency + sync word for
 *        Meshtastic LongFast. The RF frequency follows @p region (US slot 19
 *        906.875 MHz / EU_868 slot 869.525 MHz); the modem preset and sync word
 *        are region-independent. Called by lora_task during radio bring-up.
 */
void lora_meshtastic_get_radio_params(sx126x_mod_params_lora_t *mod,
                                      sx126x_pkt_params_lora_t *pkt,
                                      uint32_t *freq_hz, uint8_t *sync_word,
                                      lora_region_t region);

/**
 * @brief Derive our node number / id string from the device MAC. Idempotent.
 */
void lora_meshtastic_init(void);

/**
 * @brief Meshtastic main loop: continuous RX, boot NodeInfo announce, periodic
 *        re-announce (and optional test TX). Never returns.
 */
void lora_meshtastic_run(void);

/**
 * @brief Re-arm continuous RX after the radio was idled (e.g. on return from
 *        light sleep). Clears stale TX-busy state; safe no-op if Meshtastic
 *        mode was never initialized or no portal session is active. Serializes
 *        via the radio mutex.
 */
void lora_meshtastic_resume_rx(void);

/**
 * @brief Begin a listening session: put the radio into continuous RX and queue a
 *        NodeInfo announce. Called when the Meshtastic portal page opens. Until
 *        this runs the radio stays in standby, so it doesn't drain the battery
 *        listening when the user isn't viewing/sending. No-op outside Meshtastic
 *        mode. Serializes via the radio mutex.
 */
void lora_meshtastic_listen_start(void);

/**
 * @brief End a listening session: idle the radio to low-power standby and drop
 *        any un-sent queued text. Called when the Meshtastic portal page closes.
 *        Serializes via the radio mutex.
 */
void lora_meshtastic_listen_stop(void);

/**
 * @brief Handle a LoRa DIO1 IRQ while in Meshtastic mode. Called from the LoRa
 *        event-handler task.
 *
 * @param [in] irq_flags Raw SX126X_IRQ_* flags read from the radio.
 */
void lora_meshtastic_handle_irq(uint16_t irq_flags);

/**
 * @brief Decrypt + decode + print a raw received Meshtastic frame.
 *
 * @param [in] buf  Raw on-air bytes (16-byte header + encrypted payload)
 * @param [in] len  Frame length in bytes
 * @param [in] rssi Packet RSSI in dBm
 * @param [in] snr  Packet SNR in dB
 */
void lora_meshtastic_process_rx(const uint8_t *buf, size_t len, int8_t rssi, int8_t snr);

/**
 * @brief Broadcast our NodeInfo (User) so we appear in other nodes' lists.
 *
 * @returns true if the transmission was started
 */
bool lora_meshtastic_send_nodeinfo(void);

/**
 * @brief Our 32-bit Meshtastic node number (0 until lora_meshtastic_init()).
 */
uint32_t lora_meshtastic_node_num(void);

/**
 * @brief Our Meshtastic node id string, e.g. "!a1b2c3d4" (never NULL).
 */
const char *lora_meshtastic_node_id(void);

/**
 * @brief Queue a text message for the mesh TX task to broadcast.
 *
 * Thread-safe; called from the web portal (HTTP server task). The actual
 * transmit is performed by lora_meshtastic_run so all TX stays single-producer.
 * Text longer than MESHTASTIC_RX_TEXT_MAX is truncated.
 *
 * @param [in] text NUL-terminated message
 * 
 * @returns true if queued, false if not in Meshtastic mode, empty, or queue full
 */
bool lora_meshtastic_enqueue_text(const char *text);

/**
 * @brief Copy stored text messages newer than @p since into @p out.
 *
 * Messages are copied oldest-first. If more than @p max_out are newer than
 * @p since, only the most recent @p max_out are returned (older ones are gone
 * from the ring). Thread-safe.
 *
 * @param [in]  since      Return only messages with seq > since (0 for all)
 * @param [out] out        Caller buffer for up to @p max_out messages
 * @param [in]  max_out    Capacity of @p out in elements
 * @param [out] newest_seq If non-NULL, receives the newest stored seq (0 if none)
 * 
 * @returns number of messages copied into @p out
 */
size_t lora_meshtastic_get_msgs_since(uint32_t since, lora_meshtastic_msg_t *out,
                                      size_t max_out, uint32_t *newest_seq);

/**
 * @brief Copy the nodes-heard roster into @p out (up to @p max_out). Each entry's
 *        age_s is computed at call time. Thread-safe.
 *
 * @returns number of nodes copied
 */
size_t lora_meshtastic_get_nodes(lora_meshtastic_node_t *out, size_t max_out);

/**
 * @brief Number of nodes currently in the roster. Thread-safe.
 */
size_t lora_meshtastic_node_count(void);

#endif // LORA_MESHTASTIC_H
