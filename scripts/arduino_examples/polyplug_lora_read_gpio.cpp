/*
 * PolyPlug LoRa GPIO Read Example
 * https://polycast5.com
 * 
 * GPIO 5-Bit Bus Receiver
 *
 * Reads a 5-bit parallel value (0–31).
 * Connect each BIT pin on the PolyPlug
 * to the corresponding Arduino input pin below, plus a common GND.
 *
 * The sender holds all 5 lines high for a configurable pulse,
 * then clears them to 0. We detect the rising edge, read the
 * value, then wait for the lines to go idle before accepting
 * the next pulse.
 */

// Arduino input pins - wire these to the PolyPlug GPIO outputs
// !CHANGE THE PINS BELOW TO THE ONES YOU ARE USING
const uint8_t PIN_BIT4 = 0;  // MSB — connect to GPIO bit 4
const uint8_t PIN_BIT3 = 0;  //       connect to GPIO bit 3
const uint8_t PIN_BIT2 = 0;  //       connect to GPIO bit 2
const uint8_t PIN_BIT1 = 0;  //       connect to GPIO bit 1
const uint8_t PIN_BIT0 = 0;  // LSB — connect to GPIO bit 0

static bool was_idle = true; // To only read on the rising edge

void setup()
{
  Serial.begin(115200);

  // Set pins as inputs
  pinMode(PIN_BIT4, INPUT);
  pinMode(PIN_BIT3, INPUT);
  pinMode(PIN_BIT2, INPUT);
  pinMode(PIN_BIT1, INPUT);
  pinMode(PIN_BIT0, INPUT);

  Serial.println("5-bit bus receiver ready");
}

// Read all 5 pins and reconstruct the value
uint8_t readBus()
{
  uint8_t val = 0;
  val |= digitalRead(PIN_BIT4) << 4;
  val |= digitalRead(PIN_BIT3) << 3;
  val |= digitalRead(PIN_BIT2) << 2;
  val |= digitalRead(PIN_BIT1) << 1;
  val |= digitalRead(PIN_BIT0) << 0;
  return val;
}

void loop()
{
  if (was_idle && readBus() != 0) {
    // Rising edge: small settle delay then sample (pins high for 100ms)
    delay(10);
    uint8_t value = readBus();

    Serial.print("Received: ");
    Serial.println(value);

    // --- Do something with 'value' (0–31) here ---

    was_idle = false;
  }
  else if (!was_idle && readBus() == 0) {
    // Lines cleared: ready for next pulse
    was_idle = true;
  }
}
