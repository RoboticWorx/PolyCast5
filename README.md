<p align="center">
  <img
    src="https://raw.githubusercontent.com/RoboticWorx/PolyCast5/main/scripts/dev/pc5_wordmark.png"
    alt="PolyCast5"
    width="66%"
  />
</p>

# PolyCast5 Firmware

Welcome to the official GitHub page for the PolyCast5!

PolyCast5 is a open-source multi-tool wireless remote that you can use to control outlets, custom devices, bluetooth devices, anything infrared, and more.

You can find some relevant links below:

* [**PolyCast5 official website.**](https://polycast5.com/) A graphical explanation of what you can do with it and how it works!
* [**PolyCast5 user documentation.**](https://polycast5.com/pages/docs) How to use it and unleash its full capabilities.
* [**PolyCast5 attachment tutorials.**](https://polycast5.com/pages/tutorials) Custom builds you can control with your PolyCast5. Including:
  * Home light switcher
  * Door locker/unlocker
  * Button pusher
  * and more!

<p align="center">
  <img
    src="https://raw.githubusercontent.com/RoboticWorx/PolyCast5/main/scripts/dev/pc5_side_by_side.png"
    alt="White and Transparent Versions"
    width="66%"
  />
</p>

# Functionality

* **Outlet actuator** - [_tutorial_](https://polycast5.com/blogs/tutorials/using-polyplugs)
  * Wirelessly switch outlets/appliances ([PolyPlugs](https://github.com/RoboticWorx/PolyPlug)) over LoRa with one-press on/off, repeating timers, weekly schedules, and an away presence-simulation mode. All secured with per-plug AES encryption.
* **Bluetooth controller** - [_tutorial_](https://polycast5.com/blogs/docs/bluetooth-auto-keyboard)
  * Behave as a Bluetooth HID ("ordinary" keyboard) except it's actually a powerful autotype scripting engine (delays, key holds, etc.) to instantly types out repetitive text, snippets, and create custom executable commands. It also includes media remote modes for social media scrolling, pause/play/next/prev, presentation clicking, camera shutters, and more.
* **Wi-Fi sender and sniffer**
  * Sniff network data and beacon frames to view active network users, RSSI, security details (WPA2/WPA3/OWE, PMF, WPS), and tons of other network information. Also includes an AI packet analysis tool to have every frame sniffed and interpreted in its entirety by AI, then returned as a easy-to-read `.md` file.
* **AI voice keyboard** - [_tutorial_](https://polycast5.com/blogs/docs/ai-keyboard)
  * Hold to talk and your request is transcribed, interpreted by AI, and executed live on your paired computer or phone via Bluetooth. Also with modes for plain voice dictation and custom commands. _e.g. you say "break time" -> PC opens chrome, goes to your favorite TV show._
* **eCompass** - [_tutorial_](https://polycast5.com/blogs/tutorials/control-projects-using-the-ecompass)
  * A built-in accelerometer and magnetometer compass for streaming orientation/position data live over ESP-NOW to your other projects. Use your device like a literal steering wheel. Complete with automatic guided calibration.
* **ESP32 commander** - [_tutorial_](https://polycast5.com/blogs/tutorials/control-custom-builds)
  * Use [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now) to send instant, optionally-encrypted commands to custom Arduino-compatible ESP32 builds. The perfect universal remote for any custom projects you may build.
* **Offline password manager** - [_tutorial_](https://polycast5.com/blogs/docs/offline-password-manager-ai-keyboard)
  * Save your logins securely on-device, then autofill them on any computer or phone over Bluetooth. No cloud, no browser extension, no Wi-Fi. Just ask and let your password get autotyped, all under 6 seconds.
* **Meshtastic messenger**
  * Join the the open-source [Meshtastic](https://meshtastic.org/) LoRa mesh network for long-range communication without the need for internet, Wi-Fi, or any subscriptions. PolyCast5 talks to real Meshtastic nodes while hosting an easy-to-use chat portal.
* **Infrared remote**
  * Save and replay infrared signals for TVs, air conditioners, lamps, or anything else. Learning is protocol-agnostic (raw capture), and captures are organized into multiple on-device virtual remotes.
* **Games**
  * A built-in games featuring a DOOM-style raycasting shooter with procedurally generated levels, plus Tetris, T-Rex Runner, and Flappy Bird. Each with saved high scores and custom accent colors.
* **Offline tools**
  * Dice roller, coin flipper, random number generator, Pomodoro timer, spaced-repetition (SRS) study planner, Bitcoin-address QR, an offline docs viewer, and more.
* **Over-the-air updates**
  * Check for and install new firmware easily over Wi-Fi. Your device will literally only ever get better over time!
* **Customizable settings**
  * Set a security PIN, custom the device colors, haptics, RGB LED configuration, LCD brightness, sleep timer, LoRa region/spreading factor, and more. Complete with many detailed system-info pages reporting battery, memory, versions, and MACs.

PolyCast5 also comes with cool, customizable homescreen animations and hotkeys! Each of the six hotkeys can fire a saved LoRa, ESP-NOW, or infrared command, or jump straight to a page like the AI keyboard, timer, or study planner **instantly**.

# Project Structure

PolyCast5 is task-based: `main` brings up the hardware, then spawns one FreeRTOS task per subsystem. Tasks communicate through queues and semaphores, and share the SPI/I2C buses behind mutexes.

* `main` - The `app_main` entry point. Initializes NVS, the SPI/I2C buses, the LCD/LVGL stack, and the LoRa radio, then launches the tasks below.
* `assets` - UI media (icons, QR codes, and homescreen animation frames) stored as raw `.bin` files and flashed to a LittleFS partition.
* `anim` - The full library of homescreen animation frame sequences - City, Black Hole, Matrix Rain, and Pyramid - from which the selected animations are copied into `assets`.
* `bin` - Precompiled release binaries, kept in sync by `flash.py` for the OTA updater and web flasher.
* `scripts` - Bluetooth autotype examples, Arduino ESP-NOW receiver sketches, and miscellaneous development tooling.
* `simulator` - Desktop LVGL + SDL2 build that renders the device screens in a window, for previewing and testing UI layout without hardware.
* `partitions.csv` - Flash layout: NVS, dual OTA app slots, and the LittleFS assets partition (16MB flash total).
* `flash.py` - Change-aware flashing helper that auto-detects the serial port and reflashes only the partitions that changed.
* `components` - The application code, split into one folder per subsystem.
  * `ai` - Runs `ai_task`: records from the I2S MEMS microphone, sends audio and prompts to a cloud AI API for speech-to-text and responses, and hosts the AI key / packet-analysis web portals.
  * `bluetooth` - Runs `bluetooth_task`: a NimBLE HID stack that pairs as a keyboard + media device, executes the autotype scripting engine, and stores scripts/credentials in NVS.
  * `common` - Program-wide headers: `polycast5_gpios.h` (hardware pin map) and `polycast5_macros.h` (task priorities, PSRAM allocation macros, and build/debug flags).
  * `espnow` - Runs `espnow_task`: sends low-latency ESP-NOW frames to custom ESP32 builds, streams sensor orientation at ~40 Hz, and syncs encryption keys/settings to PolyPlugs. (e.g. light switcher, etc.)
  * `gpio` - Runs `gpio_task`: polls the TCA9535 I2C GPIO expander for button events, drives the haptic motor and RGB LED, reads the battery ADC, and samples the LIS2DH12 accelerometer and MMC5603 magnetometer.
  * `infrared` - Runs `infrared_task`: captures and replays raw IR waveforms through the RMT peripheral (38 kHz carrier) and saves learned remotes to NVS. (e.g. TVs, air conditioners, etc.)
  * `lcd` - Runs `lcd_task`: drives the ST7789 display over SPI and renders every LVGL screen - menus, homescreen animations, games, tools, and settings.
  * `lora` - Runs `lora_task`: drives the SX1262 transceiver (via `sx126x`) for both the encrypted PolyPlug outlet protocol and [Meshtastic](https://meshtastic.org/) mesh messaging.
  * `lvgl` - The [LVGL](https://lvgl.io/) graphics library used to build the on-screen UI.
  * `sx126x` - Semtech's SX126x driver/HAL for the LoRa transceiver, used by `lora`.
  * `wifi` - Runs `wifi_task`: station/AP connectivity, MQTT, 802.11 sniffing, over-the-air firmware updates, and the Wi-Fi config portals.

# Licensing

_Code in this repository is licensed under [CC Attribution-NonCommercial-ShareAlike 4.0 International](https://github.com/RoboticWorx/PolyCast5/blob/main/LICENSE.md) ([canonical](https://creativecommons.org/licenses/by-nc-sa/4.0/))._

_Unless required by applicable law or agreed to in writing, this
software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied._

# Development

PolyCast5 is written in C and built on [ESP-IDF](https://github.com/espressif/esp-idf) v6.0.1.

## Cloning

The repository can be cloned with the following git command:

```shell
git clone https://github.com/RoboticWorx/PolyCast5.git
```

## Flashing

Flash the full code (builds automatically):

```shell
python flash.py
```

Runs the PolyCast5 Python flashing script. Must be ran in an ESP-IDF terminal.

_Note: Development mode flash encryption is enabled by default for passive protection. For full protection, see [polycast5.com/blogs/docs/lock-it-down](https://polycast5.com/blogs/docs/lock-it-down)._

You can also flash online using the [firmware tools](https://polycast5.com/pages/tools).

## Build

Change the device configuration (if needed):

```shell
idf.py menuconfig
```

Build the code:

```shell
idf.py build
```

# Patches

Changes to the ESP-IDF framework are required to use the deauthenticator app.

If you would like this app to be functional, please see the following README:

`https://github.com/RoboticWorx/PolyCast5/blob/main/components/wifi/patch/README.md`
