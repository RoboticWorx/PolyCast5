#ifndef LORA_MESHTASTIC_H
#define LORA_MESHTASTIC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sx126x.h"

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
#define MESHTASTIC_SYNC_WORD       0x2B        // → SX126x sync reg 0x24B4 (matches RadioLib)
#define MESHTASTIC_PREAMBLE_SYMB   16          // symbols (all presets)
#define MESHTASTIC_CHANNEL_HASH    0x08        // PacketHeader.channel for the LongFast channel
#define MESHTASTIC_MAX_PACKET_LEN  255         // LoRa PHY max: 16-byte header + payload

// PortNums (meshtastic/portnums.proto)
#define MESHTASTIC_PORT_TEXT       1
#define MESHTASTIC_PORT_POSITION   3
#define MESHTASTIC_PORT_NODEINFO   4

/**
 * @brief Runtime Meshtastic-mode flag.
 *
 * Flip this to choose what the LoRa radio does at boot. Initialized from
 * POLYCAST5_MESHTASTIC_MODE (see polycast5_macros.h). Read once when lora_task
 * starts; a later LCD toggle will re-init the radio (phase 2).
 *   true  → join the Meshtastic mesh (phase 1: print RX to terminal, TX text)
 *   false → normal PolyCast LoRa (PCP outlet control)
 */
extern volatile bool g_meshtastic_mode;

/**
 * @brief Fill SX126x modem/packet params + RF frequency + sync word for
 *        Meshtastic LongFast (US). Called by lora_task during radio bring-up.
 */
void lora_meshtastic_get_radio_params(sx126x_mod_params_lora_t *mod,
                                      sx126x_pkt_params_lora_t *pkt,
                                      uint32_t *freq_hz, uint8_t *sync_word);

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
 *        mode was never initialized. Serializes via the radio mutex.
 */
void lora_meshtastic_resume_rx(void);

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
 * @brief Encrypt and broadcast a UTF-8 text message on the default channel.
 *
 * @param [in] text NUL-terminated message
 * @returns true if the transmission was started
 */
bool lora_meshtastic_send_text(const char *text);

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

#endif // LORA_MESHTASTIC_H
