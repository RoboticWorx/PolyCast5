/*
 * PolyPlug ESP-NOW Receive Example (Encrypted + Authenticated)
 * https://polycast5.com
 *
 * Receives commands sent from PolyCast5 over ESP-NOW with LMK
 * (Local Master Key) encryption enabled, plus an integrity/replay check.
 *
 * You must set the local_master_key and polycast5_mac below to
 * match the values shown on your PolyCast5.
 *
 * This sketch checks the command itself, regular ESP-NOW encryption hides
 * your commands from eavesdroppers, but it will not tell you who
 * sent a frame or whether you have seen it before (not secure) which
 * this fixes. This is also why there is a bit more code.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ccm.h>
#include <string.h>

#define POLYCAST5_MAGIC "PC5: " // Data prefix to identify PolyCast5 data specifically
#define MAGIC_LEN 5 // Length of the prefix, excluding the NUL terminator
#define LMK_SIZE 16 // Local Master Key size for ESP-NOW encryption in bytes
#define MAC_SIZE 6 // PolyCast5 MAC address size in bytes

#define CTR_LEN 4 // Counter width in bytes
#define MIC_LEN 8 // Authentication tag length in bytes
#define NONCE_LEN 13 // AES-CCM nonce length in bytes
#define HDR_LEN (MAGIC_LEN + CTR_LEN) // Cleartext, authenticated as associated data
#define FRAME_LEN (HDR_LEN + 1 + MIC_LEN) // Whole frame as it arrives

// 16-byte local master key for encryption
// !CHANGE THIS TO THE GENERATED KEY SHOWN ON POLYCAST5
static const uint8_t local_master_key[LMK_SIZE] = {
  0xEB, 0xFA, 0xE9, 0xBE, 0xCA, 0xC4, 0x65, 0xB0,
  0xC7, 0x5A, 0xE3, 0x59, 0xD3, 0x8B, 0x4D, 0x2B
};

// MAC address of the sender
// !CHANGE THIS TO THE DEVICE MAC SHOWN ON POLYCAST5
static const uint8_t polycast5_mac[MAC_SIZE] = {0xD0, 0xCF, 0x13, 0xE0, 0xA7, 0x2C};

static bool keys_ready = false; // False until setup succeeds; nothing is accepted before then

// If replay counter cannot be stored: false refuses every command, true keeps working without it
// (replay counter prevents someone from capturing the raw signal and replaying it to bypass encryption)
#define ALLOW_WITHOUT_REPLAY_STORAGE false

#define NVS_NS "pc5" // Namespace holding the pairing ID and the replay counter
#define NVS_KEY_PAIR "pair"
#define NVS_KEY_CTR "ctr"

static bool storage_ok = false; // False if the replay counter could not be established
static volatile uint32_t last_counter; // Highest counter accepted so far, may not be stored yet
static uint32_t persisted_counter; // Highest counter actually written to flash

// A verified command and the counter it arrived with, handed from the callback to loop()
typedef struct {
  uint8_t cmd;
  uint32_t counter;
} pending_cmd_t;

static QueueHandle_t cmd_queue;

// Defined below setup() and loop() - helper functions to make it secure
static uint32_t read_be32(const uint8_t *p);
static bool derive_keys(uint32_t *pairing_id);
static bool store_u32(const char *key, uint32_t value);
static bool pair_with(uint32_t pairing_id);
static bool load_replay_state(uint32_t pairing_id);
void receive_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);

void setup() {
  // Enable serial terminal
  Serial.begin(115200);

  // Work out our keys from the LMK before anything can arrive
  uint32_t pairing_id = 0;
  if (!derive_keys(&pairing_id)) {
    Serial.println("ERROR: no keys, every command will be rejected");
  }

  // Somewhere to hand verified commands to loop()
  cmd_queue = xQueueCreate(8, sizeof(pending_cmd_t));
  if (cmd_queue == NULL) {
    Serial.println("ERROR: could not create command queue");
  }

  // Establish how far the replay counter had got before this boot
  nvs_flash_init(); // Idempotent
  if (keys_ready) {
    storage_ok = load_replay_state(pairing_id);
  }

  persisted_counter = last_counter;

  if (!storage_ok) {
    Serial.println(ALLOW_WITHOUT_REPLAY_STORAGE
        ? "WARNING: running without replay protection across restarts"
        : "Commands will be refused until replay state can be stored");
  }

  // Setup Wi-Fi mode as station for ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // Disconnect from any APs

  // Initialize ESP-NOW itself
  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW init failed"); // Some debugging
  }

  // Setup the peer with encryption enabled and the custom LMK (local master key)
  esp_now_peer_info_t peerInfo = {0};
  memcpy(peerInfo.peer_addr, polycast5_mac, MAC_SIZE); // Copy over the MAC
  peerInfo.channel = 1; // Must match PolyCast5 Wi-Fi channel
  peerInfo.encrypt = true; // Enable encryption
  memcpy(peerInfo.lmk, local_master_key, LMK_SIZE); // Copy over your shared LMK

  // Add the peer
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("ERROR: esp_now_add_peer failed"); // Some debugging
  }

  // Register the receive callback
  esp_now_register_recv_cb(receive_cb);

  // Confirm in the terminal
  Serial.print("ESP-NOW receiver ready (encrypted + authenticated), counter at ");
  Serial.println(last_counter);
}

void loop() {
  static bool persist_warned = false;

  pending_cmd_t item;

  // Triggers whenever a command is received over ESP-NOW
  while (cmd_queue != NULL && xQueuePeek(cmd_queue, &item, 0) == pdTRUE) {
    // Check the received command isn't a replay of a previous one
    if (storage_ok && item.counter > persisted_counter) {
      // Store the new replay counter in NVS (to persist across reboots)
      if (!store_u32(NVS_KEY_CTR, item.counter)) { // Check success
        if (!persist_warned) {
          Serial.println("WARNING: could not store replay counter, command held");
          persist_warned = true;
        }
        break; // Leave it queued and try again next pass
      }

      persisted_counter = item.counter; // Save to global
      persist_warned = false;
    }

    xQueueReceive(cmd_queue, &item, 0); // Consume the queued item

    Serial.print("Got: ");
    Serial.println(item.cmd); // The command received

    // !HERE!
    // You would add extra code here to do something based on the item.cmd variable,
    // like control an LED or anything else!
  }

  delay(10);
}

// Callback that triggers when data is received
// Runs in the Wi-Fi task, so it only validates and hands off - no flash writes
void receive_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  if (!keys_ready || cmd_queue == NULL) { // Setup did not complete
    Serial.println("REJECTED: Setup did not complete, so nothing can be trusted");
    return;
  }

  if (!storage_ok && !ALLOW_WITHOUT_REPLAY_STORAGE) {
    // Cannot remember what has already been seen, so cannot rule out a replay
    static bool refusal_logged = false;
    if (!refusal_logged) {
      Serial.println("REJECTED: replay state unavailable, refusing all commands (see startup)");
      refusal_logged = true;
    }
    return;
  }

  // 1. Only look at frames claiming to come from your PolyCast5
  //    A source MAC can be spoofed, so this is a filter and not the security boundary
  if (memcmp(info->src_addr, polycast5_mac, MAC_SIZE) != 0) {
    Serial.println("REJECTED: MAC doesn't match");
    return;
  }

  // 2. A command frame is exactly FRAME_LEN bytes - anything else is not one
  if (len != FRAME_LEN) {
    Serial.println("REJECTED: Incorrect frame length");
    return;
  }

  // 3. Check the prefix matches the sender magic
  if (memcmp(data, POLYCAST5_MAGIC, MAGIC_LEN) != 0) {
    Serial.println("REJECTED: Magic doesn't match");
    return;
  }

  uint32_t counter = read_be32(data + MAGIC_LEN);

  // 4. Authenticate and decrypt
  //    This is the step that proves the frame came from a device holding your LMK, and that it wasn't altered
  uint8_t nonce[NONCE_LEN] = {0};
  memcpy(nonce + NONCE_LEN - CTR_LEN, data + MAGIC_LEN, CTR_LEN);

  uint8_t cmd = 0;
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);

  int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, local_master_key, LMK_SIZE * 8);
  if (rc == 0) {
    rc = mbedtls_ccm_auth_decrypt(&ctx, 1, // One byte of ciphertext: the command
                                  nonce, NONCE_LEN,
                                  data, HDR_LEN, // Associated data: the cleartext header
                                  data + HDR_LEN, &cmd,
                                  data + HDR_LEN + 1, MIC_LEN);
  }
  mbedtls_ccm_free(&ctx);

  if (rc != 0) { // Forged, corrupted, or built with a different key
    Serial.println("REJECTED: authentication failed");
    return;
  }

  // 5. Freshness
  //    The frame is genuine, but it could be replayed
  if (counter <= last_counter) {
    Serial.print("REJECTED: replay (counter ");
    Serial.print(counter);
    Serial.print(", already at ");
    Serial.print(last_counter);
    Serial.println(")");
    return;
  }

  last_counter = counter;

  // The counter travels with the command so loop() can make it durable before acting on it
  pending_cmd_t item = { cmd, counter };
  if (xQueueSend(cmd_queue, &item, 0) != pdTRUE) {
    Serial.println("WARNING: command queue full, command dropped");
  }
}

// Read a big-endian uint32 out of the frame
static uint32_t read_be32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Fingerprint the LMK so a change of pairing can be spotted
static bool derive_keys(uint32_t *pairing_id)
{
  uint8_t digest[32];
  if (mbedtls_sha256(local_master_key, LMK_SIZE, digest, 0) != 0) {
    Serial.println("ERROR: could not fingerprint the LMK");
    return false;
  }

  *pairing_id = read_be32(digest);
  memset(digest, 0, sizeof(digest));

  keys_ready = true;
  return true;
}

// Write one value and flush it: returns false if it did not land
static bool store_u32(const char *key, uint32_t value)
{
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    return false;
  }

  bool ok = (nvs_set_u32(h, key, value) == ESP_OK) && (nvs_commit(h) == ESP_OK);
  nvs_close(h);

  return ok;
}

// Start a fresh pairing: counter first, then the ID that vouches for it
static bool pair_with(uint32_t pairing_id)
{
  if (!store_u32(NVS_KEY_CTR, 0) || !store_u32(NVS_KEY_PAIR, pairing_id)) {
    Serial.println("ERROR: could not store the new pairing");
    return false;
  }

  last_counter = 0;
  return true;
}

// Load the replay counter for this pairing
// Returns false if the state could not be established, in which case commands are refused
static bool load_replay_state(uint32_t pairing_id)
{
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return pair_with(pairing_id); // Nothing stored at all: a genuinely new device
  }
  if (err != ESP_OK) {
    Serial.println("ERROR: storage unreadable");
    return false;
  }

  uint32_t stored_pair = 0;
  err = nvs_get_u32(h, NVS_KEY_PAIR, &stored_pair);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    // No pairing record
    uint32_t ctr_probe = 0;
    esp_err_t ctr_err = nvs_get_u32(h, NVS_KEY_CTR, &ctr_probe);
    nvs_close(h);

    // Nothing above zero was ever accepted, so re-pairing is safe
    if (ctr_err == ESP_ERR_NVS_NOT_FOUND || (ctr_err == ESP_OK && ctr_probe == 0)) {
      return pair_with(pairing_id);
    }

    Serial.println("ERROR: pairing record lost for a counter that is still in use");
    return false;
  }
  if (err != ESP_OK) {
    nvs_close(h);
    Serial.println("ERROR: pairing record unreadable"); // Guessing here would wipe the counter
    return false;
  }

  if (stored_pair != pairing_id) {
    nvs_close(h);
    Serial.println("New pairing detected, resetting replay counter");
    return pair_with(pairing_id);
  }

  uint32_t stored_ctr = 0;
  err = nvs_get_u32(h, NVS_KEY_CTR, &stored_ctr);
  nvs_close(h);

  if (err != ESP_OK) {
    // The pairing is known but its counter is missing or unreadable
    // Starting from zero would re-accept every command already seen, so treat it as lost rather than as a fresh start
    Serial.println("ERROR: replay counter lost for a known pairing");
    return false;
  }

  last_counter = stored_ctr;
  return true;
}
