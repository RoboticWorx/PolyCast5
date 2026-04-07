/*
 * PolyPlug ESP-NOW Receive Example (No Encryption)
 * https://polycast5.com
 *
 * Unencrypted ESP-NOW Receiver
 *
 * Receives commands sent from PolyCast5 over ESP-NOW
 * without encryption. Data arrives as "PC5: <cmd>"
 * where <cmd> is an unsigned integer you can act on.
 *
 * No pairing or key exchange needed - just flash and go.
 */

#include <WiFi.h>
#include <esp_now.h>

#define POLYCAST5_MAGIC "PC5: " // Data prefix to filter for PolyCast5 data specifically

volatile unsigned int cmd_received; // Command being received over ESP-NOW from your PolyCast5

// Callback that triggers when data is received
void receive_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
  // Optionally print the sender MAC (PolyCast5)
  // Serial.print("From ");
  // for (int i = 0; i < 6; i++) {
  //   Serial.printf("%02X", info->src_addr[i]);
  //   if (i < 5) Serial.print(":");
  // }
  // Serial.print(" | ");

  // The received data will be in the form "PC5: %u" for filtering
  // We need to extract the %u (unsigned int) command

  char buf[ESP_NOW_MAX_DATA_LEN]; // Create a buffer to store the data string

  // If the received data length is less than the max buffer size, make len the new buffer size
  size_t copy_len = len < ESP_NOW_MAX_DATA_LEN - 1 ? len : ESP_NOW_MAX_DATA_LEN - 1;
  memcpy(buf, data, copy_len); // Copy len bytes of the data into the buffer
  buf[copy_len] = '\0'; // Null-terminate the string

  // Try to parse "PC5: %u" out of the string
  unsigned int tmp;
  if (sscanf(buf, POLYCAST5_MAGIC "%u", &tmp) == 1) { // If success -> data is valid
    cmd_received = tmp; // Move the parsed value into global variable
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

  // Register the receive callback
  esp_now_register_recv_cb(receive_cb);

  // Confirm in the terminal
  Serial.println("ESP-NOW receiver ready (no encryption)");
}

void loop() {
  // Nothing to do here yet (happens in callback)
  delay(100);

  // You would add extra code here to do something based on the cmd_received variable,
  // like control an LED or anything else!
}