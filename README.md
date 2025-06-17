img here

PolyCast5 Firmware
====================
Welcome to the official GitHub page for the PolyCast5!

You can find some relevant links below:
* [PolyCast5 official website](). An easy explanation of what you can do with it and how it works!
* [PolyCast5 user documentation](). How to use it and unleash its full capabilities.
* [PolyCast5 attachment tutorials](). Custom builds you can control with your PolyCast5. Including:
    * Home light switcher
    * Door locker/unlocker
    * Button pusher
    * and more!

## Application
PolyCast5 was developed on [ESP-IDF](https://github.com/espressif/esp-idf) with the [ESP32-C5](https://www.espressif.com/en/products/socs/esp32-c5) for its high-performance and dual-band wireless capabilities.

Please check [ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for getting started instructions if you have any difficulties.

## Project Structure
* `main` - Initialization code. The program runs from here.
* `build` - How the program is built as well as the compiled code.
* `components` - The various pieces of the application broken into folders.
    * `bluetooth` - Relevant Bluetooth functions and task. Used to communicate with a connected BLE device as a HID.
    * `common` - Shared macros across the entirety of the program used for build and debugging.
    * `espnow` - Relevant [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now) functions and task. Used for sending instantaneous low-power commands to external ESP32 devices (e.g. light switcher, etc.).
    * `gpio` - Relevant GPIO functions and task. Used to sample from the onboard GPIO expander as well as provide global user input semaphores.
    * `infrared` - Relevant infrared functions and task. Used in obtaining, saving, and replaying various infrared signals.
    * `lcd` - Relevant LCD functions and task. Contains a driver for a ST7789 display and well as navigates user input in coordination with LVGL.
    * `lora` - Relevant LoRa functions and task. Used in communicating with the SX1262 LoRa radio as well as sending and receiving long-range signals for commanding [PolyPlugs]().
    * `lvgl` - Display library responsible for creating a nice user experience with the screen.
    * `sx126x` - Driver for the SX1262 LoRa radio. Communicates commands on the HAL over SPI.
    * `wifi` - Relevant Wi-Fi functions and task. Used for connecting to networks, monitoring packets, and other Wi-Fi capabilities.

## Licensing
*Code in this repository is licensed under [CC Attribution-NonCommercial-ShareAlike 4.0 International](https://github.com/RoboticWorx/PolyCast5/blob/main/LICENSE.md) ([canonical](https://creativecommons.org/licenses/by-nc-sa/4.0/)).*

*Unless required by applicable law or agreed to in writing, this
software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.*