/*
 * PolyPlug ESP-NOW eCompass Receive Example (No Encryption)
 * https://polycast5.com
 *
 * Unencrypted ESP-NOW eCompass Receiver
 *
 * Receives the live eCompass stream sent from PolyCast5 over
 * ESP-NOW without encryption. Data arrives ~40 times a second as
 * "PC5: <roll>,<pitch>,<yaw>" where each value is in degrees:
 *   roll  = X tilt (signed)
 *   pitch = Y tilt (signed)
 *   yaw   = compass heading, 0-360 (0 = where it pointed when streaming started)
 *
 * No pairing or key exchange needed - just flash and go.
 *
 * NOTE: The prefix is a tag, not a password, and anyone in
 * Wi-Fi range can broadcast one. Use this for prototyping.
 */

#include <WiFi.h>
#include <esp_now.h>

#define POLYCAST5_MAGIC "PC5: " // Data prefix to filter for PolyCast5 data specifically

// Latest orientation received over ESP-NOW from your PolyCast5
volatile float roll_received;
volatile float pitch_received;
volatile float yaw_received;

// Callback that triggers when data is received
void receive_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
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

  // Register the receive callback
  esp_now_register_recv_cb(receive_cb);

  // Confirm in the terminal
  Serial.println("ESP-NOW eCompass receiver ready (no encryption)");
}

void loop() {
  // Nothing to do here yet (happens in callback)
  delay(100);

  // You would add extra code here to do something based on roll_received,
  // pitch_received and yaw_received, like drive a servo or anything else!
}
