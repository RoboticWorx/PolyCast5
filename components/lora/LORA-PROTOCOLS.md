# PolyCast5 LoRa Protocols

The PolyCast5 radio (SX1262) speaks **two mutually exclusive LoRa protocols**, chosen
at boot. This document is the reference for both:

1. **PCP** - the *Poly Cipher Protocol*, PolyCast5's own private protocol for
   controlling PolyPlug outlets.
2. **Meshtastic** - the public, open-source mesh-messaging protocol. This section
   documents the **base-truth spec** that any Meshtastic-compatible device must
   match on the air, independent of PolyCast5.

---

## TL;DR - PCP vs. Meshtastic

|                     | **PCP** (Poly Cipher Protocol)                     | **Meshtastic** (LongFast public channel)             |
|---------------------|----------------------------------------------------|------------------------------------------------------|
| **What it is**      | PolyCast5's private point-to-point control link    | Open-source long-range mesh text protocol            |
| **Talks to**        | PolyPlug smart outlets (paired to this device)     | Any Meshtastic node/phone in range                   |
| **Goal**            | Send a command, get a confirmed ACK back           | Broadcast text/telemetry across a multi-hop mesh     |
| **Frequency**       | Per-region band center, user-selectable (§1.2.1)   | Per-region LongFast slot (§1.2.1)                    |
| **PHY**             | SF7–SF12 (user-selectable) · BW 125 kHz · CR 4/5   | SF11 · BW 250 kHz · CR 4/5 (the LongFast preset)     |
| **Sync word**       | `0x62` (private)                                    | `0x2B` (the Meshtastic network-wide value)           |
| **Crypto**          | AES-128-**CCM** (authenticated, per-pair key)      | AES-128-**CTR** (channel PSK, no per-packet auth)    |
| **Framing**         | Fixed binary structs                               | 16-byte header + protobuf payload                    |
| **Addressing**      | Per-plug AES key (the `Index` byte is the command) | 32-bit node numbers, broadcast or unicast            |
| **Delivery**        | Explicit ACK packet with matching msg-id + retries | Implicit ACK (hearing your packet rebroadcast)       |
| **Mesh/relaying**   | None - direct link only                            | Flood routing with a hop limit                       |

**Why two protocols?** PCP is optimized for a *reliable, authenticated, private*
command channel to hardware you own. Meshtastic is optimized for *interoperable,
long-range, best-effort* messaging with strangers' nodes. They share nothing on
the air - different frequency, sync word, modulation, and crypto - so a PolyPlug
never hears a mesh packet and vice versa.

### How PolyCast5 switches between them

The mode is a persisted NVS flag (`g_meshtastic_mode`), read once at boot in
[`lora_task`](src/lora_task.c). The **entire PHY** (frequency, sync word, SF/BW/CR,
preamble, packet params) is selected from that one flag *before* the radio is
brought up, and the LoRa event-handler task routes every DIO1 interrupt to the
matching handler. Toggling the mode from the LCD writes the flag and **reboots**,
so the switch is atomic - there is never a moment where PCP logic touches a radio
configured for Meshtastic or vice versa. While Meshtastic mode is on, every PCP
entry point (PolyPlug control, pairing, hotkeys) is disabled in the UI.

---

## 1. PCP - Poly Cipher Protocol

PolyCast5's private protocol for commanding **PolyPlugs** (LoRa smart outlets).
It is a one-to-many control link: the PolyCast5 is the controller, each PolyPlug
is a receiver addressed by an `index`.

### 1.1 Purpose & model

- The controller sends an **encrypted command** ("turn plug 2 on", or run a named
  instruction) and waits for an **encrypted ACK** carrying the same message id.
- If no valid ACK arrives within the RX window, the command is **retried** (up to
  `MAX_RETRIES`), then abandoned.
- Every command carries a **monotonic message id** (persisted to NVS) so a receiver
  can reject replays.

### 1.2 Radio / PHY parameters

| Parameter        | Value                                         |
|------------------|-----------------------------------------------|
| Frequency        | **Per-region band center** (see §1.2.1)        |
| Sync word        | **0x62** (private, not the Meshtastic value)   |
| Spreading factor | **SF7** default, user-selectable **SF7–SF12**  |
| Bandwidth        | **125 kHz**                                    |
| Coding rate      | **4/5**                                         |
| Preamble         | **12 symbols**                                 |
| Header           | Explicit                                       |
| CRC              | On                                             |
| IQ               | Normal                                         |
| LDRO             | On automatically for SF11/SF12 (`SF > 10`)     |
| TX power         | 22 dBm                                          |

The spreading factor is stored in NVS (`lora_cfg/sf`) and applied at boot; changing
it reboots the device and requires re-pairing plugs (they must use the same SF).

The **region** is likewise stored in NVS (`lora_cfg/region`) and applied at boot. It selects
the PCP carrier, the Meshtastic LongFast slot, *and* the SX1262 image-calibration band (see
§1.2.1) so image rejection matches the operating band. Changing region reboots the device and
requires re-pairing plugs — the region byte rides the ESP-NOW key-sync frame alongside the SF
byte, so plugs switch to the same RF band. The region enum is **append-only** so the byte stays
wire-compatible with existing PolyPlugs.

### 1.2.1 Region table

One row per `lora_region_t` (the code equivalent lives in
[`lora_pcp.c`](src/lora_pcp.c) as `region_params[]`). Band definitions mirror
meshtastic/firmware `RadioInterface.cpp`; every band sits inside the M620720
antenna's 863-928 MHz range (CN 470 and EU 433 are outside it and therefore
unsupported). The PCP carrier is the band center; the LongFast slot derivation
is in §2.2 (`n` = channel count, `slot` = `djb2("LongFast") % n`).

| Region | Legal band (MHz)              | PCP carrier | LongFast slot | LongFast freq | Image-cal band |
|--------|-------------------------------|-------------|---------------|---------------|----------------|
| US     | 902-928                       | 915.0 MHz   | n=104, 19     | 906.875 MHz   | 902-928 MHz    |
| EU     | 869.4-869.65 sub-band         | 869.5 MHz   | n=1, 0        | 869.525 MHz   | 863-870 MHz    |
| ANZ    | 915-928                       | 921.5 MHz   | n=52, 19      | 919.875 MHz   | 902-928 MHz    |
| IN     | 865-867                       | 866.0 MHz   | n=8, 3        | 865.875 MHz   | 863-870 MHz    |
| KR     | 920-923                       | 921.5 MHz   | n=12, 11      | 922.875 MHz   | 902-928 MHz    |
| JP     | 920.5-923.5                   | 922.0 MHz   | n=12, 11      | 923.375 MHz   | 902-928 MHz    |
| TW     | 920-925                       | 922.5 MHz   | n=20, 15      | 923.875 MHz   | 902-928 MHz    |
| RU     | 868.7-869.2                   | 868.95 MHz  | n=2, 1        | 869.075 MHz   | 863-870 MHz    |
| TH     | 920-925                       | 922.5 MHz   | n=20, 15      | 923.875 MHz   | 902-928 MHz    |
| SG     | 917-925 (Meshtastic `SG_923`) | 921.0 MHz   | n=32, 3       | 917.875 MHz   | 902-928 MHz    |
| MY     | 919-924 (Meshtastic `MY_919`) | 921.5 MHz   | n=20, 15      | 922.875 MHz   | 902-928 MHz    |

TW and TH share the same 920-925 MHz band, so their identical frequencies are
intentional — they are distinct regulatory labels, not a copy-paste error.

The EU carrier sits in the **869.4-869.65 MHz** high-power sub-band (ETSI EN 300 220 allows
500 mW / 27 dBm at 10% duty), so PCP's 22 dBm TX stays within the EU limit — the 25 mW-limited
868.0-868.6 MHz sub-band would not.

> **TX power disclaimer:** TX power is fixed at **22 dBm** in every region. Several
> regions legally allow less (e.g. Japan under ARIB STD-T108, Korea, Russia, and the
> EU outside the 869.4-869.65 sub-band) and/or impose duty-cycle limits. Selecting
> the region that matches your location and complying with local power and duty-cycle
> rules is the user's responsibility.

### 1.3 Security model

- **Cipher:** AES-128-CCM (via the PSA crypto API) with a **4-byte MIC** (shortened
  authentication tag). CCM gives both confidentiality *and* authentication - a
  tampered or wrong-key packet fails to decrypt and is dropped.
- **Key:** a random **16-byte** key generated on the PolyCast5. During pairing it is
  distributed to each PolyPlug **over ESP-NOW** (not over LoRa), so the LoRa link
  never carries the key.
- **Nonce:** a fresh **random 13-byte** nonce per transmission, prepended in the
  clear (CCM needs the nonce to decrypt; it is not secret).
- **Replay protection:** the `msg_id` counter is monotonic and persisted, so a
  receiver rejects any id it has already seen.

### 1.4 Packet formats

All multi-byte integers are the on-wire layout of the packed C structs
([`lora_pcp.h`](include/lora_pcp.h)). Ciphertext length equals plaintext length
(CCM is a stream construction - no block padding).

**Command packet - 55 bytes on air**

```
┌───────────────┬──────────────────────────────────────────────┬────────────┐
│  Nonce (13B)  │            AES-CCM Ciphertext (38B)          │  MIC (4B)  │
│   random      ├──────┬──────────┬───────┬────────────────────┤  auth tag  │
│               │ Type │  Msg ID  │ Index │       Instr        │            │
│               │  1B  │  4B u32  │ 1B u8 │      char[32]      │            │
│               │ 0x01 │          │       │                    │            │
└───────────────┴──────┴──────────┴───────┴────────────────────┴────────────┘
```

| Field   | Size | Meaning                                             |
|---------|------|-----------------------------------------------------|
| Type    | 1 B  | `0x01` = COMMAND                                     |
| Msg ID  | 4 B  | uint32, monotonic; the ACK must echo this            |
| Index   | 1 B  | which command: 0 toggle, 1 loop, 2 plan, 3 away, 4 GPIO |
| Instr   | 32 B | NUL-terminated instruction string. A toggle (index 0) carries `LORA_PCP_INSTR_TOGGLE` (`"0s"`) when the remote wants the relay level back; `"0"` is the filler for any other command with no arguments |

**ACK packet - 23 bytes on air**

```
┌───────────────┬──────────────────────────────────────────────┬────────────┐
│  Nonce (13B)  │             AES-CCM Ciphertext (6B)          │  MIC (4B)  │
│   random      ├──────┬────────────────────────────┬──────────┤  auth tag  │
│               │ Type │           Msg ID           │  State   │            │
│               │  1B  │           4B u32           │  1B u8   │            │
│               │ 0x02 │                            │          │            │
└───────────────┴──────┴────────────────────────────┴──────────┴────────────┘
```

| Field  | Size | Meaning                                                               |
|--------|------|-----------------------------------------------------------------------|
| Type   | 1 B  | `0x02` = ACK                                                          |
| Msg ID | 4 B  | uint32; echoes the command's id                                       |
| State  | 1 B  | relay level after a toggle: `0x00` off, `0x01` on, `0xFF` not reported |

**Asking for the relay level.** Every ACK is this one 23-byte form; what varies is
whether the State byte carries a real level or `0xFF`:

- A remote that wants the relay level sends `LORA_PCP_INSTR_TOGGLE` (`"0s"`) as
  the instruction of a toggle command (index 0). `lora_task` injects it at
  dispatch, and only for a command whose outcome will actually be displayed - a
  hotkey's result is discarded, so it does not ask and the plug does not hold its
  ACK waiting for the relay.
- A PolyPlug reports a real level **only** for a toggle carrying that marker.
  Every other command, and any toggle sent without it (a hotkey), ACKs with
  `0xFF`, which the remote shows as the plain tick.

The plug reports the level the relay actually reached: it waits up to 500 ms for
`gpio_task` to apply the toggle and publish the resulting level, matched by
message id so a level left over from an earlier command cannot be misreported. If
that wait expires the ACK still goes out, carrying `0xFF`.

### 1.5 Message flow

```
LCD/UI ──► xLoraSendEncQueue ──► lora_task
                                    │  set key, assign msg_id, build struct
                                    ▼
                          encrypt (AES-CCM) + transmit
                                    │
                                    ▼
                             enter RX (2 s timeout)
                                    │
                 ┌──────────────────┼───────────────────┐
                 ▼                  ▼                   ▼
          ACK, id matches     ACK id wrong /      RX timeout /
          → delivered         CRC fail            header/CRC error
                              → retry (≤ 2)       → retry (≤ 2)
                                                   then give up
```

**What the UI shows.** The outcome is published on `xLoraReceiptQueue` and
rendered by the LoRa pages:

| Outcome                | Toggle (index 0) | Timer / plan / away / GPIO |
|------------------------|------------------|----------------------------|
| ACK, relay ended on    | `1`              | -                          |
| ACK, relay ended off   | `0`              | -                          |
| ACK, no level reported | ✓                | ✓                          |
| Every retry exhausted  | ✗                | nothing                    |

Only commands raised from a LoRa page publish an outcome. A hotkey fired from the
homescreen is tagged `ui_origin = false` and stays silent, so its result can never
be painted onto whichever page the user opens next. An outcome is also withheld
when a newer command is already queued, so what is shown always belongs to the
most recent send.

The `waiting_for_ack` / `need_to_retry` state machine lives in
[`lora_task.c`](src/lora_task.c); encryption, key handling, and ACK validation are
in [`lora_pcp.c`](src/lora_pcp.c).

### 1.6 Constants

| Name                     | Value | Meaning                        |
|--------------------------|-------|--------------------------------|
| `LORA_PCP_NONCE_LENGTH`  | 13    | CCM nonce bytes                |
| `LORA_PCP_MIC_LENGTH`    | 4     | CCM auth tag bytes             |
| `LORA_PCP_ENC_KEY_LEN`   | 16    | AES-128 key bytes              |
| `LORA_PCP_INSTR_MAX_LEN` | 32    | instruction string capacity    |
| `LORA_PCP_COMMAND`       | 0x01  | command type byte              |
| `LORA_PCP_ACK`           | 0x02  | ACK type byte                  |
| `LORA_PCP_ACK_STATE_OFF` | 0x00  | ACK state: relay ended off     |
| `LORA_PCP_ACK_STATE_ON`  | 0x01  | ACK state: relay ended on      |
| `LORA_PCP_ACK_STATE_NONE`| 0xFF  | ACK state: no level reported   |
| `LORA_PCP_INSTR_TOGGLE`  | `"0s"`| toggle instr asking for state  |
| `MAX_RETRIES`            | 2     | retransmits before giving up   |

---

## 2. Meshtastic - the base-truth spec

This section documents **Meshtastic itself**, not PolyCast5. Every value below is
the ground truth a device must reproduce byte-for-byte to interoperate on the
default **LongFast** public channel in the **US 902–928 MHz** band. Sources are the
[meshtastic/firmware](https://github.com/meshtastic/firmware) and
[meshtastic/protobufs](https://github.com/meshtastic/protobufs) repositories.

> **What "compatible" means:** two Meshtastic devices interoperate only if they
> agree on (a) the RF channel and modulation, (b) the sync word, (c) the channel
> hash byte, (d) the AES key and nonce construction, and (e) the packet header +
> protobuf framing. Miss any one and your packets are invisible or undecryptable
> to everyone else.

### 2.1 Radio / PHY - the LongFast preset

| Parameter        | Value        | Notes                                                       |
|------------------|--------------|-------------------------------------------------------------|
| Modem preset     | **LONG_FAST** | The out-of-the-box default for all Meshtastic devices       |
| Spreading factor | **SF11**     |                                                             |
| Bandwidth        | **250 kHz**  |                                                             |
| Coding rate      | **4/5**      |                                                             |
| Preamble         | **16 symbols** | Meshtastic uses 16, not the LoRa default of 8             |
| Sync word        | **0x2B**     | Network-wide "Meshtastic" value; maps to SX126x reg `0x24B4` |
| Header           | Explicit     |                                                             |
| CRC              | On           |                                                             |
| IQ               | Normal       |                                                             |
| LDRO             | **Off**      | Symbol time `2^11/250 = 8.192 ms` < 16 ms threshold         |
| Max payload      | 255 bytes    | LoRa PHY limit (Semtech datasheet)                          |

### 2.2 Frequency & channel hash (two *different* hashes)

Meshtastic derives two things from the channel, using **two distinct hash functions** -
a common point of confusion:

**(a) The RF frequency slot** - from a 32-bit **djb2** hash of the channel *name*:

```
numChannels = floor((freqEnd − freqStart) / (spacing + bandwidth_MHz))
            = floor((928.0 − 902.0) / (0 + 0.25))         = 104   (US, BW 250)

channel_num = djb2("LongFast") % numChannels
            = 130429955 % 104                             = 19

freq = freqStart + bandwidth/2 + channel_num · bandwidth
     = 902.0 + 0.125 + 19 · 0.25                          = 906.875 MHz
```

> `spacing` is Meshtastic's per-region inter-channel guard; it is **0 for the US
> region** (and every current region row), so it drops out here — but the real
> firmware formula includes it.
>
> **Region selection:** the above is the worked **US** example (906.875 MHz). The
> same formula (with spacing 0) yields the LongFast slot for every supported region —
> the per-region band edges, channel counts, and resulting slots are tabulated in
> §1.2.1. E.g. the EU_868 band (869.4-869.65 MHz) is only one BW-250 channel wide,
> so `channel_num` collapses to 0: `869.4 + 0.125 = 869.525 MHz`. The modem preset
> (SF11/BW250/CR4-5) and sync word are region-independent; only the frequency slot
> moves.

**(b) The `channel` byte in the packet header** - an 8-bit **XOR hash** of the
channel name XORed with the XOR hash of the PSK. This lets a receiver quickly
skip packets not on a channel it holds a key for:

```
xorHash(s) = s[0] ^ s[1] ^ … ^ s[n-1]

xorHash("LongFast")   = 0x0A
xorHash(default PSK)  = 0x02
channel byte          = 0x0A ^ 0x02 = 0x08
```

So on the LongFast US channel, packets are sent at **906.875 MHz** with header
channel byte **`0x08`**.

### 2.3 Encryption - AES-128-CTR

Meshtastic encrypts only the payload (the header travels in the clear). The
default channel uses a well-known 16-byte PSK, so "encryption" on the public
channel is really just obfuscation - anyone can decrypt it.

- **Cipher:** AES-128 in **CTR** mode. CTR is symmetric (encrypt == decrypt), and
  provides **no authentication** - there is no MIC/tag. A receiver's only integrity
  check is "did the payload parse as a valid protobuf?"
- **Default LongFast PSK** (what the 1-byte PSK value `0x01` expands to):
  ```
  base64:  1PG7OiApB1nwvP+rz05pAQ==
  bytes:   d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01
  ```
- **Nonce (16-byte CTR initial counter block):**
  ```
  bytes 0–7   packetId  as little-endian uint64  (a 32-bit id → bytes 4–7 = 0)
  bytes 8–11  fromNode  as little-endian uint32
  bytes 12–15 block counter, starts at 0
  ```
  Because the id and sender go into the nonce, the same plaintext from different
  senders (or with different ids) produces different ciphertext - and a receiver
  must know `from` and `id` (both in the cleartext header) to build the nonce and
  decrypt.

### 2.4 Packet header - 16 bytes, little-endian

Every on-air frame is a 16-byte header followed by the encrypted payload:

```
┌──────────┬──────────┬──────────┬────────┬─────────┬──────────┬────────────┐
│  to (4)  │ from (4) │  id (4)  │ flags  │ channel │ next_hop │ relay_node │
│   LE     │    LE    │    LE    │  (1)   │   (1)   │   (1)    │    (1)     │
└──────────┴──────────┴──────────┴────────┴─────────┴──────────┴────────────┘
   0..3       4..7       8..11      12        13        14          15
```

| Field        | Meaning                                                                 |
|--------------|-------------------------------------------------------------------------|
| `to`         | destination node number; `0xFFFFFFFF` = broadcast                        |
| `from`       | sender node number                                                       |
| `id`         | 32-bit packet id (nonzero); used for dedup and implicit ACK              |
| `flags`      | hop limit / want-ack / via-mqtt / hop start (see below)                  |
| `channel`    | the 8-bit channel hash (`0x08` for LongFast) - see §2.2                   |
| `next_hop`   | 0 = no preference (last byte of the intended next-hop node, else)        |
| `relay_node` | last byte of the relaying node's number (see §2.7)                        |

**Flags byte layout:**

```
 bit  7   6   5   4        3        2   1   0
     └── hop_start ──┘  via_mqtt  want_ack └ hop_limit ┘
```

| Bits | Field       | Meaning                                              |
|------|-------------|------------------------------------------------------|
| 0–2  | `hop_limit` | remaining hops; decremented by each relayer          |
| 3    | `want_ack`  | sender requests an ACK                                |
| 4    | `via_mqtt`  | packet has traversed an MQTT gateway                  |
| 5–7  | `hop_start` | original hop limit at the source (for hop counting)  |

A receiver computes **hops taken** as `hop_start − hop_limit` (0 = heard directly).
A typical LongFast broadcast sets `hop_start = hop_limit = 3` → flags `0x63`.

### 2.5 The Data protobuf & PortNums

After decryption the payload is a **`Data`** protobuf message. Meshtastic uses no
length prefix - the whole decrypted payload *is* the message.

`Data` fields (`meshtastic/mesh.proto`):

| # | Field             | Wire type        | Notes                                   |
|---|-------------------|------------------|-----------------------------------------|
| 1 | `portnum`         | varint (PortNum) | which app the payload belongs to        |
| 2 | `payload`         | bytes            | the app payload                          |
| 3 | `want_response`   | bool             |                                          |
| 4 | `dest`            | fixed32          |                                          |
| 5 | `source`          | fixed32          |                                          |
| 6 | `request_id`      | fixed32          |                                          |
| 7 | `reply_id`        | fixed32          |                                          |
| 8 | `emoji`           | fixed32          |                                          |
| 9 | `bitfield`        | varint           |                                          |
| 10| `xeddsa_signature`| bytes            |                                          |

**Common PortNums** (`meshtastic/portnums.proto`):

| Value | PortNum              | Payload                                          |
|-------|----------------------|--------------------------------------------------|
| 1     | `TEXT_MESSAGE_APP`   | raw UTF-8 text (the payload *is* the string)     |
| 3     | `POSITION_APP`       | `Position` protobuf (lat/lon ×1e7, alt, time)    |
| 4     | `NODEINFO_APP`       | `User` protobuf (see §2.6)                        |
| 5     | `ROUTING_APP`        | `Routing` protobuf (ACKs / traceroute)           |

### 2.6 The User (NodeInfo) message

Broadcast on `NODEINFO_APP` so other nodes can display a name for you.

`User` fields (`meshtastic/mesh.proto`):

| # | Field             | Type                       | Notes                                            |
|---|-------------------|----------------------------|--------------------------------------------------|
| 1 | `id`              | string                     | node id string, e.g. `"!a1b2c3d4"`               |
| 2 | `long_name`       | string                     | display name                                     |
| 3 | `short_name`      | string                     | ≤ 4 chars by convention                          |
| 4 | `macaddr`         | bytes                      | **deprecated**                                   |
| 5 | `hw_model`        | enum `HardwareModel`       | device model                                     |
| 6 | `is_licensed`     | bool                       | ham operator; licensed nodes drop unlicensed NodeInfo |
| 7 | `role`            | enum `Config.DeviceConfig.Role` | the node's mesh role (below)                |
| 8 | `public_key`      | bytes                      | for PKC-encrypted direct messages                |
| 9 | `is_unmessagable` | bool (optional)            | node cannot be messaged                          |

**Role enum** (`meshtastic/config.proto`, `DeviceConfig.Role`):

| Value | Role          | Meaning                                                   |
|-------|---------------|-----------------------------------------------------------|
| 0     | `CLIENT`      | default; participates normally in flood routing            |
| 1     | `CLIENT_MUTE` | **does not forward** other nodes' packets                  |
| 2     | `ROUTER`      | infrastructure relay, always rebroadcasts                  |
| …     | (others)      | ROUTER_CLIENT, REPEATER, TRACKER, SENSOR, etc.             |

A leaf device that never rebroadcasts should advertise `role = CLIENT_MUTE (1)`, so
peers don't assume it participates in routing.

### 2.7 Routing rules a compatible device must honor

- **Node identity:** `nodeNum` is the **low 4 bytes of the Wi-Fi MAC, big-endian**.
  The id string is `"!" + lowercase 8-hex-digit nodeNum` (e.g. `!a1b2c3d4`).
- **Broadcast address:** `0xFFFFFFFF`.
- **Flood routing:** a relayer decrements `hop_limit`; when it reaches 0 the packet
  is not forwarded. `hop_start` is left unchanged so hop count is recoverable.
- **Duplicate suppression:** dedup key is the **pair `(from, id)`**, *not* `id`
  alone - two different senders may reuse a 32-bit id. Meshtastic keeps a small
  recent-packet history keyed on both.
- **Implicit ACK for broadcasts:** there is no explicit ACK for a broadcast. Hearing
  a neighbor **rebroadcast your own packet** (same `from` == you, same `id`) is the
  implicit "delivered" signal.
- **`relay_node`:** set to the **last byte of the relaying node's number**, with the
  special case that a last byte of `0x00` is sent as `0xFF` (0 means "unset").
  Modern firmware sets this on originated packets too.

### 2.8 Minimal protobuf primer

Meshtastic messages are standard protobuf-3 wire format. Only two wire types matter
for the messages above:

```
Each field starts with a tag varint:  tag = (field_number << 3) | wire_type

wire_type 0 = varint            (portnum, role, bools, hop counts)
wire_type 2 = length-delimited  (a length varint, then that many bytes:
                                 strings, sub-messages, byte payloads)
wire_type 5 = 32-bit fixed      (fixed32 - e.g. Position lat/lon)
wire_type 1 = 64-bit fixed
```

Example tags you will actually emit/parse:

| Bytes  | Meaning                                              |
|--------|------------------------------------------------------|
| `0x08` | field 1, varint  → `Data.portnum`                    |
| `0x12` | field 2, len     → `Data.payload` (and `User.long_name`) |
| `0x0A` | field 1, len     → `User.id`                         |
| `0x1A` | field 3, len     → `User.short_name`                 |
| `0x38` | field 7, varint  → `User.role`                       |

Unknown fields must be **skipped** by wire type, never treated as an error - that is
how the format stays forward-compatible.

### 2.9 Compatibility checklist

A device is LongFast Meshtastic-compatible **only if all of these match**:

- [ ] RF: the region's LongFast slot (§1.2.1), SF11, BW 250 kHz, CR 4/5, preamble 16, CRC on, IQ normal, LDRO off
- [ ] Sync word `0x2B`
- [ ] Header channel byte `0x08`
- [ ] AES-128-CTR with the default PSK and the `id|from|0` nonce
- [ ] 16-byte little-endian header with the correct flags bit layout
- [ ] `Data` protobuf with a valid PortNum, payload as bytes
- [ ] `nodeNum` = low 4 MAC bytes (big-endian); id string `!%08x`
- [ ] Dedup on `(from, id)`; broadcast to `0xFFFFFFFF`; honor `hop_limit`

---

## Appendix - PHY side-by-side

| Parameter    | PCP            | Meshtastic LongFast   |
|--------------|----------------|-----------------------|
| Frequency    | Per-region     | Per-region (§1.2.1)   |
| Sync word    | 0x62           | 0x2B                  |
| SF           | 7 (7–12)       | 11                    |
| Bandwidth    | 125 kHz        | 250 kHz               |
| Coding rate  | 4/5            | 4/5                   |
| Preamble     | 12 symbols     | 16 symbols            |
| Header       | Explicit       | Explicit              |
| CRC          | On             | On                    |
| IQ           | Normal         | Normal                |
| Crypto       | AES-128-CCM    | AES-128-CTR           |
| Auth tag     | 4-byte MIC     | none                  |

## Source files

| File                                                   | Responsibility                              |
|--------------------------------------------------------|---------------------------------------------|
| [`lora_task.c`](src/lora_task.c)                       | Radio bring-up, mode select, IRQ dispatch   |
| [`lora_pcp.c`](src/lora_pcp.c) / [`.h`](include/lora_pcp.h) | PCP framing, AES-CCM, ACK/retry state  |
| [`lora_radio.c`](src/lora_radio.c)                     | PCP low-level TX / RX-mode helpers          |
| [`lora_meshtastic.c`](src/lora_meshtastic.c) / [`.h`](include/lora_meshtastic.h) | Meshtastic framing, AES-CTR, protobuf, RX/TX loop |
| [`lora_meshtastic_portal.c`](src/lora_meshtastic_portal.c) | Wi-Fi SoftAP + HTTP portal for messaging |
| [`lora_meshtastic_portal_html.c`](src/lora_meshtastic_portal_html.c) | The portal's single-page web UI    |
