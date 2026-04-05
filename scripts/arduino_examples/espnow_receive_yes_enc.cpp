#include <WiFi.h>
#include <esp_now.h>

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

volatile unsigned int cmd_received; // Command being received over ESP-NOW from your PolyCast5

// Callback that triggers when data is received
void receive_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  // Optionally print the sender MAC (PolyCast5)
  // Serial.print("From ");
  // for (int i = 0; i < MAC_SIZE; i++) {
  //   Serial.printf("%02X", info->src_addr[i]);
  //   if (i < MAC_SIZE - 1) Serial.print(":");
  // }
  // Serial.print(" | ");

  // The received data will be in the form "PC5: %u" for security purposes
  // We need to extract the %u (unsigned int) command

  char buf[ESP_NOW_MAX_DATA_LEN]; // Create a buffer to store the data string

  // If the received data length is less than the max buffer size, make len the new buffer size
  size_t copy_len = len < ESP_NOW_MAX_DATA_LEN - 1 ? len : ESP_NOW_MAX_DATA_LEN - 1;
  memcpy(buf, data, copy_len); // Copy len bytes of the data into the buffer
  buf[copy_len] = '\0'; // Null-terminate the string

  // Try to parse "PC5: %u" out of the string
  unsigned int tmp;
  if (sscanf(buf, POLYCAST5_MAGIC "%u", &tmp) == 1) { // If success -> data is valid and the command is now stored in cmd_received
    cmd_received = tmp;
    Serial.print("Got: ");
    Serial.println(cmd_received); // The command received
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
  Serial.println("ESP-NOW receiver ready (encrypted)");
}

void loop() {
  // Nothing to do here yet (happens in callback)
  delay(100);

  // You would add extra code here to do something based on the cmd_received variable,
  // like control an LED or anything else!
}