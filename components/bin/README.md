# Compiled Binaries

This folder contains the precompiled PolyCast5 firmware binaries of the latest release.

These files are primarily intended for use with the PolyCast5 [**Firmware Updater**](https://polycast5.com/pages/firmware-updater).

> Note: The flash settings and addresses below must match your PolyCast5 hardware revision / partition layout.

## Flash settings

- `--flash_mode dio --flash_freq 80m --flash_size 16MB`

## Flash map

- `0x2000`   `bootloader/bootloader.bin`
- `0x8000`   `partition_table/partition-table.bin`
- `0x49000`  `ota_data_initial.bin`
- `0x50000`  `PolyCast5.bin`
- `0x790000` `assets.bin`

## Manual flashing (esptool.py)

Replace `COMx` with your serial port (e.g., `COM5` / `/dev/ttyUSB0`):

```bash
esptool.py -p COMx -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x2000 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x49000 ota_data_initial.bin \
  0x50000 PolyCast5.bin \
  0x790000 assets.bin
```
