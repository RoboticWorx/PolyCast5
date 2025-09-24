Technical Specs
====================
Here you can find some technical-ish details to get a better understanding of the full capabilities of PolyCast5.

First and foremost, Bluetooth!

## Bluetooth
* PolyCast5 utilizes [NimBLE](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/bluetooth/nimble/index.html), which is a small yet highly configurable Bluetooth Low Energy (BLE) stack providing both host and controller functionalities. This makes it perfect for embedded devices that require full-functionality without taking up too much memory. 
* Unlike other devices, PolyCast5 goes for full-functionality. For this reason, Bluetooth is **set to transmit at a higher power level (+12dBm)** so that you remain connected over longer distances than typical Bluetooth devices.
  * Don't worry, the the power consumption difference isn't too high but if you need to, this can be easily changed under 'menuconfig > component config > bluetooth > controller options > default tx power level'.

## LoRa
* LoRa is a **lo**ng-**ra**nge wireless technology that is excellent at sending small payloads over long distances (**up to many kilometers** with the appropriate settings). For this reason, LoRa is the base communication method for communicating all commands with [PolyPlugs](https://github.com/RoboticWorx/PolyPlug).
  * Yet, PolyCast5 is **open-source** so feel free to use the built-in LoRa for communicating with other things too!
* To strike the best balance of time-on-air and range, **PolyCast5 uses SF7** (spreading factor 7) along with BW_125 and CR_4_5 by default which has a range of around 1km and will hit the target and back in usually less than a second (varies if you're in a rural or urban area). This said, you can successfully send commands from anywhere within the general facility of a PolyPlug, whether you are using it to turn on a coffee maker or relay commands to a custom Arduino/other device to perform some cool task. You can rely on LoRa!
  * In addition, whenever you send anything a little white check mark will appear in the top left corner **to indicate the command has been successfully received**. If you see this check mark, you know your message was picked up and understood by a PolyPlug.
* If you're wondering how the hardware is able to transmit LoRa signals, I'm using a SX1262 with a pre-matched ceramic filter.

## ESP-NOW
* Speaking of interacting with custom devices, PolyCast5 uses the ESP32-C5 for its MCU which gives it (and therefore you) full access to Espressif technologies such as [ESP-NOW](https://www.espressif.com/en/solutions/low-power-solutions/esp-now). This is likely the best and easiest to use wireless technology for interacting with custom stuff like Arduino devices or anything else via Arduino IDE/others.
  * All that's needed is for you to grab a $5 off-the-shelf ESP32 dev kit of any kind and upload [this super simple example code](https://polycast5.com/blogs/tutorials/arduino-esp-now-receiver-examples). From there, you can wire it up to any Arduino Uno, STM32, etc. and you'll have a **direct line of wireless communication to use whenever you want**. (E.g. open a door, flip a switch, activate a robotic arm, etc.)
  * ESP-NOW is the best for this since it can activate quickly to send the signal at low power, then deactive itself afterwards. It also has a pretty impressive range, maxing out around 200m in an open area.

## Infrared
* PolyCast5 is equipped with a full infrared (IR) receiver and transmitter. It uses two infrared LEDs for double the signal strength, so any receiving device can still see it even if it is a bit obstructed.
  * If you're curious, I'm using the TSOP75338 for receiving the signals along with the [ESP-IDF RMT peripheral](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-reference/peripherals/rmt.html). Then just a simple IR LED setup with a N-ch MOSFET to transmit.

## Wi-Fi
* So you know that LoRa is used to transmit your commands long-range. But what if that could be infinite range? This is where Wi-Fi comes in.
  * PolyCast5 uses MQTT to send commands via a broker so that any device with a Wi-Fi connection is able to receive the signal. This means you could be across the country when you forgot to turn off whatever, and be able to do it from the device.
  * This said though, MQTT is a much slower to send and receive command confirmation (~5sec + connection time), which is why LoRa is better for most things. But if you ever want to control something from afar or prank someone by turning on a lamp when you're not in the house this could definitely do it!
* PolyCast5 is also able to sniff network data and beacon packets so you can get some valuable Wi-Fi information as well as see what other devices are on the network and their network strength relative to you. This means you can know if your neighbor is stealing your Wi-Fi, given you can find out their MAC address. This can also be useful in finding old devices that you don't use anymore which are stealing bandwidth from your network. You can track them down via their relative RSSIs then take them out manually.
* You can also sniff network beacon frames to check for Wi-Fi reboots in addition to network compatibility. This can be useful in comparing networks or assessing your own. You also get constant RSSI feedback meaning you can walk around your house/room to find the spot with the best network connection. Good for streaming!

## Offline
I've covered all 5 wireless technologioes now (thus the name PolyCast**5**), but we've got some awesome offline stuff too!

This includes a ton of customizable settings such as changing colors, sleep time, brightness, haptics, and even being able to set a security pin so only you can use your PolyCast5 with included notices when someone tries to sign in. Some other useful tools include a complete dice roller, coin flipper, random number generator, some calculators, and doc QRs should you ever have questions.

Of course, PolyCast5 also has many cool homescreen animations to give you something cool to look at when you wake it up. You can also add your own should you want to being open-source and all.

## Thanks for Reading!
For more info please check the links on the README prior:
* [PolyCast5 official website.](https://polycast5.com/) A graphical explanation of what you can do with it and how it works!
* [PolyCast5 user documentation.](https://polycast5.com/pages/docs) How to use it and unleash its full capabilities.
* [PolyCast5 attachment tutorials.](https://polycast5.com/pages/tutorials) Custom builds you can control with your PolyCast5. Including:
  * Home light switcher
  * Door locker/unlocker
  * Button pusher
  * and more!