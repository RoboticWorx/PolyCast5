/*
 * PolyPlug ESP-NOW eCompass Receive Example (Encrypted)
 * https://polycast5.com
 *
 * Receives the live eCompass stream sent from PolyCast5 over
 * ESP-NOW with LMK (Local Master Key) encryption enabled. Data
 * arrives ~40 times a second as "PC5: <roll>,<pitch>,<yaw>" where
 * each value is in degrees:
 *   roll  = X tilt (signed)
 *   pitch = Y tilt (signed)
 *   yaw   = compass heading, 0-360 (0 = where it pointed when streaming started)
 *
 * You must set the local_master_key and polycast5_mac below to
 * match the values shown on your PolyCast5.
 *
 * NOTE: Regular ESP-NOW encryption hides the stream from eavesdroppers,
 * but it will not prove who sent a frame or stop an old one being replayed.
 * Unlike commands, the eCompass stream carries no authentication tag, so
 * don't use it to drive anything where a faked reading would be unsafe.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

#define POLYCAST5_MAGIC "PC5: " // Data prefix to filter for PolyCast5 data specifically
#define LMK_SIZE 16 // Local Master Key size for ESP-NOW encryption in bytes
#define MAC_SIZE 6 // PolyCast5 MAC address size in bytes

// 16-byte local master key for encryption
// !CHANGE THIS TO THE GENERATED KEY SHOWN ON POLYCAST5
static const uint8_t local_master_key[LMK_SIZE] = {
  0xEB, 0xFA, 0xE9, 0xBE, 0xCA, 0xC4, 0x65, 0xB0,
  0xC7, 0x5A, 0xE3, 0x59, 0xD3, 0x8B, 0x4D, 0x2B
};

// MAC address of the sender
// !CHANGE THIS TO THE DEVICE MAC SHOWN ON POLYCAST5
static const uint8_t polycast5_mac[MAC_SIZE] = {0xD0, 0xCF, 0x13, 0xE0, 0xA7, 0x2C};

// Latest orientation received over ESP-NOW from your PolyCast5
volatile float roll_received;
volatile float pitch_received;
volatile float yaw_received;

// Callback that triggers when data is received
void receive_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  // Only look at frames from your PolyCast5
  // Unencrypted frames from other devices still reach this callback, so filter by sender
  if (memcmp(info->src_addr, polycast5_mac, MAC_SIZE) != 0) {
    Serial.println("REJECTED: MAC doesn't match");
    return;
  }

  // The received data will be in the form "PC5: %f,%f,%f" for filtering
  // We need to extract the three floats (roll, pitch, yaw)

  char buf[ESP_NOW_MAX_DATA_LEN]; // Create a buffer to store the data string

  // If the received data length is less than the max buffer size, make len the new buffer size
  size_t copy_len = len < ESP_NOW_MAX_DATA_LEN - 1 ? len : ESP_NOW_MAX_DATA_LEN - 1;
  memcpy(buf, data, copy_len); // Copy len bytes of the data into the buffer
  buf[copy_len] = '\0'; // Null-terminate the string

  // Try to parse "PC5: %f,%f,%f" out of the string
  float roll, pitch, yaw;
  if (sscanf(buf, POLYCAST5_MAGIC "%f,%f,%f", &roll, &pitch, &yaw) == 3) { // If success -> data is valid
    roll_received = roll; // Move the parsed values into global variables
    pitch_received = pitch;
    yaw_received = yaw;
    Serial.printf("Roll: %6.1f | Pitch: %6.1f | Yaw: %5.1f\n", roll, pitch, yaw);
  }
  else { // Data is not valid
    Serial.print("Unexpected data: ");
    Serial.println(buf);
  }
}

void setup() {
  // Enable serial terminal
  Serial.begin(115200);

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
  Serial.println("ESP-NOW eCompass receiver ready (encrypted)");
}

void loop() {
  // Nothing to do here yet (happens in callback)
  delay(100);

  // You would add extra code here to do something based on roll_received,
  // pitch_received and yaw_received, like drive a servo or anything else!
}
