# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PolyCast5 is an open-source multi-tool wireless remote firmware for ESP32-C5, built on ESP-IDF v6.0. It controls outlets (via LoRa), Bluetooth devices (HID), infrared remotes, custom ESP32 builds (ESP-NOW), and Wi-Fi/MQTT targets. Written in C.

## Build Commands

Do not run build commands, user will confirm build success manually.

There is no test framework; validation is done by flashing to hardware.

## Hardware & Memory

- **Target**: ESP32-C5, 16MB flash (DIO/80MHz), 8MB PSRAM (quad/80MHz)
- **Partition layout** (`partitions.csv`): 256KB NVS, dual 4.0MiB OTA slots, 7.6875MiB LittleFS for assets
- **Memory strategy**: Use `POLYCAST5_USE_PSRAM_BSS` / `POLYCAST5_USE_PSRAM_DATA` macros for large allocations in PSRAM. `CONFIG_SPIRAM_USE_MALLOC` is enabled so `malloc()` falls back to PSRAM.
- **Key peripherals**: SPI2 (SX1262 LoRa), I2C (TCA9535 GPIO expander), RMT (IR TX/RX), ADC (battery), LEDC (LCD backlight), I2S (audio codec)

## Architecture

The firmware is task-based. `main/main.c` initializes hardware (NVS, I2C, SPI, LCD/LVGL) then spawns independent FreeRTOS tasks:

| Task | Component | Purpose |
|------|-----------|---------|
| gpio_task | `components/gpio` | TCA9535 GPIO expander polling, button events, haptics, RGB LED, battery ADC |
| lcd_task | `components/lcd` | ST7789 LCD driver, LVGL rendering, UI menus, homescreen animations |
| lora_task | `components/lora` | SX1262 LoRa radio send/receive (via `components/sx126x` HAL) |
| infrared_task | `components/infrared` | IR signal capture/replay via RMT peripheral |
| bluetooth_task | `components/bluetooth` | NimBLE HID (media control, keyboard autotype) |
| wifi_task | `components/wifi` | MQTT client, beacon sniffing, network monitoring |
| espnow_task | `components/espnow` | Direct ESP-NOW commands to custom ESP32 devices |
| ai_task | `components/ai` | AI API integration, voice dictation, web config portals |

**Shared resources** are protected by mutexes: `xSPIBusMutex`, `xI2CBusMutex`, `xHapticsMutex`, `xRgbLedMutex`, `xLEDCMutex`. Inter-task communication uses FreeRTOS semaphores, queues, and occasional mutexes.

**Key shared headers** in `components/common/include/`:
- `polycast5_gpios.h` — All hardware pin definitions
- `polycast5_macros.h` — Task priorities, PSRAM macros, debug flags, animation selection

## Code Style

- Preserve existing comment style (`/** */` vs `//`) and formatting in surrounding code
- Keep functions separated with blank lines consistent with adjacent code
- Wrap lines consistently with the file's existing style
