<p align="center">
  <img
    src="https://github.com/user-attachments/assets/745ff4fc-c3f0-4f40-b177-9631ba14db68"
    alt="PolyCast5"
    width="66%"
  />
</p>

PolyCast5 Firmware
====================
Welcome to the official GitHub page for the PolyCast5!

PolyCast5 is a open-source multi-tool wireless remote that you can use to control outlets, custom devices, bluetooth devices, anything infrared, and more.

You can find some relevant links below:
* [PolyCast5 official website.](https://polycast5.com/) A graphical explanation of what you can do with it and how it works!
* [PolyCast5 user documentation.](https://polycast5.com/pages/docs) How to use it and unleash its full capabilities.
* [PolyCast5 attachment tutorials.](https://polycast5.com/pages/tutorials) Custom builds you can control with your PolyCast5. Including:
  * Home light switcher
  * Door locker/unlocker
  * Button pusher
  * and more!

## Functionality
* **Outlet actuator**  - Turn on and off outlets/applicances wirelessly and set schedules, timers, and modes.
* **Bluetooth controller** - Send media and autotype commands to instantly type out repetitive text, control volume, etc.
* **Infrared remote** - Save and replay infrared signals for TVs, air conditioners, lamps, etc.
* **ESP32 commander** - Use [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now) to send instant commands for controlling custom builds such as with Arduino. (It's really easy and Arduino IDE compatible.)
* **Wi-Fi sender and sniffer** - Send commands over long distances via MQTT or sniff network data and beacon frames to view active users, RSSI, and tons of network information.
* **Offline tools** - Dice roller, coin flipper, random number generator, and more.
* **Customizable settings** - Set a security pin, custom colors, haptics, RGB LED configuration, and more.

_PolyCast5 also comes with cool, customizable homescreen animations and hotkeys!_

## Project Structure
* `main` - Initialization code. The program runs from here.
* `assets` - Media image files stored as `.bin` for SPIFFS access.
* `scripts` - Bluetooth keystroke injection (autotype) examples as well as a few miscellaneous developement scripts.
* `build` - Generated program build files. Contains ESP-IDF as well as the compiled code.
* `components` - The various pieces of the application code broken into folders.
  * `bluetooth` - Communicates with a connected BLE device as a Human Interface Device (HID) to behave as a media controller and autotype keyboard.
  * `common` - Shared macros across the entirety of the program used for build and debugging.
  * `espnow` - Sends instantaneous low-power ESP-NOW commands to external ESP32 devices for interacting with custom builds or anything else. (e.g. light switcher, etc.)
  * `gpio` - Samples from the onboard GPIO expander, handles battery indication, as well as button press/hold logic.
  * `infrared` - Obtains, saves, and replays infrared signals for user infrared remotes. (e.g. TVs, air conditioners, etc.)
  * `lcd` - Everything LCD.
  * `lora` - Communicates with the LoRa radio to send and receive long-range signals for communicating with [PolyPlugs](https://github.com/RoboticWorx/PolyPlug).
  * `lvgl` - [LVGL](https://lvgl.io/) - Display library used to create nice graphics for the LCD.
  * `sx126x` - Driver for the SX1262 LoRa radio chip.
  * `wifi` - Connects to networks, monitors packets, and sends data over long distances via MQTT.

## Licensing
*Code in this repository is licensed under [CC Attribution-NonCommercial-ShareAlike 4.0 International](https://github.com/RoboticWorx/PolyCast5/blob/main/LICENSE.md) ([canonical](https://creativecommons.org/licenses/by-nc-sa/4.0/)).*

*Unless required by applicable law or agreed to in writing, this
software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.*

## Donations
If you love the open-source nature of PolyCast5, please consider donating to boost development and fuel a coffee run :)

All donations will be spent on adding new features and making PolyCast5 even cooler!

| Service | Remark | QR Code | Link / Wallet |
|:--|:--|:--:|:--|
| **BTC** | Bitcoin | <img src="https://github.com/user-attachments/assets/01d53028-4c2f-4638-9c6e-fa7d2625e07e" width="18" /> | bc1qwh60k0wjcjdgj78pmrdrk8huty6dx7xqxnrgkn |