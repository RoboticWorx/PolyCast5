#!/usr/bin/env python3
"""
verify_encryption.py - Run in ESP-IDF Terminal

Verifies Flash Encryption (FE) and NVS Encryption are actually working on a
PolyCast5 device by dumping raw flash partitions over UART and inspecting
them for plaintext leaks.

Note this is for default DEV mode encryption only.
Enable full protection: www.polycast5.com/blogs/docs/lock-it-down

How it works:
  1. esptool reads raw bytes directly from flash (NOT through CPU/MMU). This
     means encrypted partitions come back as ciphertext - exactly what an
     attacker with a chip-off setup or USB cable would get.
  2. We compare each dump to the plaintext build artifact in build/ to prove
     the difference (encrypted dump = no readable strings; build artifact =
     lots of readable strings).
  3. The "killer test" stores a known secret on the device, then proves it
     cannot be found anywhere in the flash dump.

Usage examples:
  python verify_encryption.py
  python verify_encryption.py --port COM7
  python verify_encryption.py --secret "TestPassword_PolyCast5_12345"
  python verify_encryption.py --port COM13 --secret "exists"
  python verify_encryption.py --skip-dump
  python verify_encryption.py --skip-dump --secret "AnotherSecret"

Parameters (all optional):
  --port PORT       Serial port the chip is on. Default: COM14
                    On Linux/macOS use e.g. /dev/ttyUSB0 or /dev/cu.usbserial-*.
  --secret SECRET   Plaintext secret to grep for in all dumps.
                    Empty = skip killer test.
  --skip-dump       Reuse existing dump_*.bin files instead of re-dumping
                    (~30s saved).

Typical workflow:
  1. Flash firmware, save a known Wi-Fi password to the device.
  2. Reboot the device, wait for it to reconnect.
  3. python verify_encryption.py --port COM13 --secret "Yellow-Guyana-34?"
  4. Iterate on more secrets with --skip-dump (no re-dump needed).

Exit code: 0 if all checks pass, 1 if any check fails. Useful for scripting.

Run from your ESP-IDF Python environment so `esptool` is importable
(`idf.py shell`, or activate the IDF venv directly).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path


# ===========================================================================
# Partition layout (from partitions.csv).
#
# Each tuple: (label, dump filename, flash offset, size).
# esptool read-flash returns bytes EXACTLY as they sit on flash - no
# decryption, no MMU translation - so encrypted partitions come back as
# ciphertext.
# ===========================================================================
DUMPS: list[tuple[str, str, int, int]] = [
    ("bootloader",  "dump_bootloader.bin",     0x002000, 0x00E000),  # FE-encrypted
    ("nvs_keys",    "dump_nvs_keys.bin",       0x011000, 0x001000),  # FE-encrypted
    ("nvs",         "dump_nvs.bin",            0x012000, 0x03B000),  # values NVS-encrypted
    ("app",         "dump_app.bin",            0x050000, 0x3B0000),  # FE-encrypted
    ("spiffs",      "dump_spiffs_sample.bin",  0x7B0000, 0x010000),  # plaintext (assets)
]

BUILD_BOOTLOADER = Path("build") / "bootloader" / "bootloader.bin"
BUILD_APP        = Path("build") / "PolyCast5.bin"


# ===========================================================================
# Search-term lists for Steps 2 & 3.
#
# These are distinctive byte sequences known to exist as plaintext literals
# in the matching build artifact. NONE should appear in an encrypted dump if
# FE is working.
# ===========================================================================
BOOT_SEARCH_TERMS: list[bytes] = [
    b"ESP-IDF",                           # version banner: "ESP-IDF v6.0 2nd stage bootloader"
    b"esp_image",                         # path: esp_image_format.c
    b"2nd stage",                         # boot banner literal
    b"bootloader_support",                # IDF component path
    b"bootloader_utility",                # IDF component path
    b"flash_encrypt",                     # FE module log tag
    b"Partition Table:",                  # log message printed at boot
    b"Loaded app from partition",         # log message
    b"Checking flash encryption",         # log message
    b"Resetting with flash encryption",   # log message
    b"RNG early entropy",                 # log message
    b"load partition table",              # error message fragment
]

# Project-unique identifiers harvested from the PolyCast5 codebase.
# Sources: ESP_LOG TAGs, FreeRTOS task names, distinctive log messages.
# Generic terms (wifi/bluetooth/error) deliberately avoided - they show up
# in IDF strings too and would dilute the signal.
APP_SEARCH_TERMS: list[bytes] = [
    b"polycast5",
    # GPIO subsystem
    b"gpio_task", b"gpio_utils",
    # Wi-Fi subsystem
    b"wifi_task", b"wifi_funcs", b"wifi_ota_update", b"wifi_mqtt",
    b"wifi_autoconnect", b"wifi_btc_portal", b"wifi_deauth", b"wifi_ping",
    # Bluetooth subsystem
    b"bluetooth_task", b"bluetooth_nvs", b"bluetooth_utils", b"bluetooth_web_portal",
    # AI subsystem
    b"ai_task", b"ai_voice", b"ai_utils", b"ai_portal", b"ai_analysis_portal",
    # LCD / UI subsystem
    b"lcd_task", b"lcd_funcs", b"lcd_settings", b"lcd_hotkey", b"lcd_anim",
    b"lcd_wifi", b"lcd_bluetooth", b"lcd_infrared", b"lcd_tools", b"lcd_espnow", b"lcd_lora",
    # LoRa subsystem
    b"lora_task", b"lora_pcp", b"lora_radio",
    # IR subsystem
    b"infrared_task", b"ir_task", b"ir_utils",
    # ESPNOW subsystem
    b"espnow_task", b"espnow_utils",
    # HAL / drivers
    b"sx126x_hal", b"st7789", b"srs_memory", b"esp_hid_gap",
    # Distinctive log messages
    b"nvs initialized", b"polycast5_priority",
]

# Pre-compiled regex patterns.
_PRINTABLE_RUN = re.compile(rb"[\x20-\x7e]{4,}")
_BOOT_PATTERN  = re.compile(b"|".join(re.escape(t) for t in BOOT_SEARCH_TERMS))
_APP_PATTERN   = re.compile(b"|".join(re.escape(t) for t in APP_SEARCH_TERMS), re.IGNORECASE)


# ===========================================================================
# Color output. ANSI escape codes; auto-disables when stdout isn't a TTY
# (so redirected/CI output stays clean).
# ===========================================================================
def _enable_colors() -> bool:
    if not sys.stdout.isatty():
        return False
    if os.name == "nt":
        # Enable VT processing on Windows 10+. No-op on terminals that
        # already have it (Windows Terminal, VS Code terminal, modern PS).
        try:
            import ctypes
            kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
            handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
            mode = ctypes.c_ulong()
            kernel32.GetConsoleMode(handle, ctypes.byref(mode))
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        except Exception:
            return False
    return True

USE_COLOR = _enable_colors()

def _c(text: str, code: str) -> str:
    return f"\x1b[{code}m{text}\x1b[0m" if USE_COLOR else text

def green(s: str)  -> str: return _c(s, "32")
def red(s: str)    -> str: return _c(s, "31")
def yellow(s: str) -> str: return _c(s, "33")
def cyan(s: str)   -> str: return _c(s, "36")
def gray(s: str)   -> str: return _c(s, "90")


# ===========================================================================
# Helpers.
# ===========================================================================

def extract_strings(data: bytes) -> list[bytes]:
    """Return all maximal runs of >=4 contiguous printable-ASCII bytes.
    Equivalent to `strings -a -n 4`."""
    return _PRINTABLE_RUN.findall(data)


def run_esptool(port: str, *args: str) -> None:
    """Invoke esptool via the active Python; abort the script on failure.

    Native exit codes don't raise Python exceptions automatically, so we
    use check_call and catch CalledProcessError for a friendlier message."""
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c5", "-p", port, *args]
    try:
        subprocess.check_call(cmd)
    except subprocess.CalledProcessError as e:
        print(red(f"\nABORT: esptool failed with exit code {e.returncode}"))
        print(red("Common causes:"))
        print(red(f"  - Device not connected or wrong --port (current: {port})"))
        print(red("  - esptool not installed in the active Python environment"))
        print(red("  - Another process holding the serial port (close idf.py monitor etc.)"))
        sys.exit(1)
    except FileNotFoundError:
        print(red("\nABORT: could not invoke esptool."))
        print(red("  Run this script from your ESP-IDF Python environment"))
        print(red("  (e.g. via `idf.py shell`)."))
        sys.exit(1)


# Each entry is (name, passed, detail). Mutated by add_check_result.
check_results: list[tuple[str, bool, str]] = []

def add_check_result(name: str, passed: bool, detail: str) -> None:
    marker = "[OK]  " if passed else "[FAIL]"
    line = f"  {marker} {detail}"
    print(green(line) if passed else red(line))
    check_results.append((name, passed, detail))


# ===========================================================================
# Main.
# ===========================================================================

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify FE + NVS encryption on a PolyCast5 device.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("-p", "--port", default="COM14",
                        help="Serial port (default: COM14; on Unix use e.g. /dev/ttyUSB0)")
    parser.add_argument("-s", "--secret", default="",
                        help="Plaintext secret to grep for. Empty = skip killer test.")
    parser.add_argument("--skip-dump", action="store_true",
                        help="Reuse existing dump_*.bin files (~30s saved)")
    args = parser.parse_args()

    script_start = time.monotonic()

    # -----------------------------------------------------------------------
    # STEP 1 - Read raw flash partitions over UART.
    #
    # Offsets and lengths come from partitions.csv:
    #   bootloader      @ 0x2000   size 0xE000   (FE-encrypted)
    #   nvs_keys        @ 0x11000  size 0x1000   (FE-encrypted)
    #   nvs             @ 0x12000  size 0x3B000  (NOT FE-encrypted - but
    #                                              values inside are NVS-
    #                                              encrypted)
    #   ota_0 (app)     @ 0x50000  size 0x3B0000 (FE-encrypted)
    #   assets (SPIFFS) @ 0x7B0000 size 0x10000  (PLAINTEXT - first 64 KB
    #                                              sample, enough to verify
    #                                              it's not over-encrypted)
    # -----------------------------------------------------------------------
    if args.skip_dump:
        print(gray("=== Skipping dump (using existing dump_*.bin files) ==="))
    else:
        print(cyan("=== Dumping partitions from chip ==="))
        for _, fname, offset, size in DUMPS:
            run_esptool(args.port, "read-flash", f"0x{offset:X}", f"0x{size:X}", fname)

    # Sanity-check that all dumps exist and are non-empty.
    for _, fname, _, _ in DUMPS:
        path = Path(fname)
        if not path.exists() or path.stat().st_size == 0:
            print(red(f"\nABORT: {fname} is missing or empty."))
            print(yellow("  Run without --skip-dump to fetch fresh dumps from the chip."))
            return 1

    # -----------------------------------------------------------------------
    # Read all dump bytes once and extract strings once. Python's regex-based
    # extraction is fast (~100 ms for 3.6 MB), but caching keeps Step 6 simple
    # and avoids re-reading the same files.
    # -----------------------------------------------------------------------
    print(cyan("\n=== Pre-extracting strings from dumps (one-time) ==="))
    extract_start = time.monotonic()

    dump_bytes:   dict[str, bytes]       = {}
    dump_strings: dict[str, list[bytes]] = {}
    for label, fname, _, _ in DUMPS:
        # Only the heavy ones get progress lines; small dumps are instantaneous.
        if fname in ("dump_app.bin", "dump_nvs.bin", "dump_spiffs_sample.bin"):
            sz_kb = Path(fname).stat().st_size // 1024
            note = " (slowest)" if fname == "dump_app.bin" else ""
            print(f"  Scanning {fname} ({sz_kb} KB){note}...")
        dump_bytes[fname]   = Path(fname).read_bytes()
        dump_strings[fname] = extract_strings(dump_bytes[fname])

    extract_elapsed = time.monotonic() - extract_start
    print(gray(f"  Done in {extract_elapsed:.1f}s."))

    # -----------------------------------------------------------------------
    # STEP 2 - Bootloader encryption check.
    # -----------------------------------------------------------------------
    print(cyan("\n=== Bootloader encryption check ==="))

    if not BUILD_BOOTLOADER.exists():
        print(red(f"\nABORT: {BUILD_BOOTLOADER} not found."))
        print(yellow("  Run `idf.py build` first."))
        return 1

    boot_dump_strings  = dump_strings["dump_bootloader.bin"]
    boot_build_strings = extract_strings(BUILD_BOOTLOADER.read_bytes())

    boot_dump_hits  = [s for s in boot_dump_strings  if _BOOT_PATTERN.search(s)]
    boot_build_hits = [s for s in boot_build_strings if _BOOT_PATTERN.search(s)]

    add_check_result("bootloader-dump",  len(boot_dump_hits)  == 0,
                     f"encrypted dump matches: {len(boot_dump_hits)} (expect 0)")
    add_check_result("bootloader-plain", len(boot_build_hits) >  0,
                     f"plain build matches:    {len(boot_build_hits)} (expect many)")

    # -----------------------------------------------------------------------
    # STEP 3 - App encryption check.
    # -----------------------------------------------------------------------
    print(cyan("\n=== App encryption check ==="))

    if not BUILD_APP.exists():
        print(red(f"\nABORT: {BUILD_APP} not found."))
        print(yellow("  Run `idf.py build` first."))
        return 1

    app_dump_strings  = dump_strings["dump_app.bin"]
    app_build_strings = extract_strings(BUILD_APP.read_bytes())

    app_dump_hits  = [s for s in app_dump_strings  if _APP_PATTERN.search(s)]
    app_build_hits = [s for s in app_build_strings if _APP_PATTERN.search(s)]

    add_check_result("app-dump",  len(app_dump_hits)  == 0,
                     f"encrypted dump matches: {len(app_dump_hits)} (expect 0)")
    add_check_result("app-plain", len(app_build_hits) >  0,
                     f"plain build matches:    {len(app_build_hits)} (expect hundreds)")

    # -----------------------------------------------------------------------
    # STEP 4 - nvs_keys "data was written" check.
    #
    # nvs_flash_generate_keys writes 32 B + 32 B XTS keys + 4 B CRC = 68 B
    # at the start of the partition; the rest stays 0xFF. After FE, those
    # 68 B become ciphertext (~1/256 of the bytes happen to be 0xFF, so
    # ~64-68 non-0xFF bytes expected in practice). Threshold of 16 has
    # ample margin.
    #
    # We don't try entropy here - plaintext XTS keys ARE random bytes by
    # design, so entropy can't distinguish encrypted from raw key material.
    # The killer test (Step 6) provides definitive end-to-end proof.
    # -----------------------------------------------------------------------
    print(cyan("\n=== nvs_keys data presence ==="))

    nvs_keys_data = dump_bytes["dump_nvs_keys.bin"]
    non_ff = len(nvs_keys_data) - nvs_keys_data.count(b"\xff")
    add_check_result("nvs_keys-written", non_ff >= 16,
                     f"non-0xFF bytes: {non_ff} (expect >= 16; keys were written to partition)")

    # -----------------------------------------------------------------------
    # STEP 5 - NVS partition structure encryption check.
    #
    # With NVS encryption ON, each 32-byte entry is XTS-AES encrypted. Only
    # page headers and entry-state bitmaps stay plaintext - mostly binary
    # metadata that doesn't form long readable strings.
    #
    # Random ciphertext DOES produce short ASCII runs by chance (~2800 of
    # length >=4 in 236 KB). So a raw count is too noisy. We filter to
    # length >= 16: random 16-char ASCII runs are statistically near-zero
    # (~0.02 expected per 236 KB), while plaintext NVS values (Wi-Fi
    # passwords 8-63 chars, OAuth tokens, OpenAI keys 50+ chars) routinely
    # exceed 16 chars.
    #
    # Caveat: a fresh device with only short settings stored could pass
    # this check even if NVS encryption were broken. The killer test
    # (Step 6) is the definitive end-to-end proof.
    # -----------------------------------------------------------------------
    print(cyan("\n=== NVS structure encryption check ==="))

    nvs_long_strings = [s for s in dump_strings["dump_nvs.bin"] if len(s) >= 16]
    add_check_result("nvs-encrypted", len(nvs_long_strings) < 5,
                     f"long (>=16 char) strings: {len(nvs_long_strings)} (expect < 5; entries should be ciphertext)")

    # -----------------------------------------------------------------------
    # STEP 6 - Secret leak check (THE KILLER TEST).
    #
    # Raw byte search via bytes.count: finds the literal byte sequence
    # regardless of length or surrounding bytes. UTF-8 encoding the needle
    # matches what ESP-IDF stores for non-ASCII string values. ASCII
    # secrets encode identically.
    # -----------------------------------------------------------------------
    if args.secret:
        print(yellow("\n=== Secret leak check (THE KILLER TEST) ==="))
        print(f"  Searching for {args.secret!r} in all dumps (raw byte scan)...")

        needle = args.secret.encode("utf-8")
        total_hits = 0
        leak_locations: list[str] = []
        for _, fname, _, _ in DUMPS:
            hits = dump_bytes[fname].count(needle)
            if hits > 0:
                leak_locations.append(f"{fname} ({hits})")
                total_hits += hits

        if total_hits == 0:
            add_check_result("secret-leak", True,
                             f"no plaintext leaks of {args.secret!r} in any dump")
        else:
            add_check_result("secret-leak", False,
                             f"LEAK: {args.secret!r} found in {total_hits} places: {', '.join(leak_locations)}")
    else:
        print(gray("\n(Skip secret-leak test: pass --secret 'YourPassword' to enable)"))

    # -----------------------------------------------------------------------
    # STEP 7 - SPIFFS plaintext sanity check.
    #
    # SPIFFS holds fonts, icons, animations - intentionally NOT encrypted.
    # This step confirms we haven't accidentally over-encrypted: it should
    # produce LOTS of readable strings.
    # -----------------------------------------------------------------------
    print(cyan("\n=== SPIFFS plaintext sanity ==="))

    spiffs_count = len(dump_strings["dump_spiffs_sample.bin"])
    add_check_result("spiffs-plain", spiffs_count >= 100,
                     f"string count: {spiffs_count} (expect >= 100; many strings = NOT over-encrypted)")

    # -----------------------------------------------------------------------
    # Final summary + exit code.
    # -----------------------------------------------------------------------
    total_elapsed = time.monotonic() - script_start
    total = len(check_results)
    passed = sum(1 for _, p, _ in check_results if p)
    failed = total - passed

    print()
    print(gray("=========================================================="))
    if failed == 0:
        if args.secret:
            print(green(f"  ENCRYPTION VERIFIED ({passed}/{total} checks passed)"))
        else:
            print(green(f"  ENCRYPTION CHECKS PASSED ({passed}/{total})"))
            print(yellow("  Note: --secret not provided; for end-to-end NVS-value proof,"))
            print(yellow("        save a known string into NVS and re-run with --secret 'thatstring'"))
        print(gray(f"  Total runtime: {int(total_elapsed)}s"))
        print(gray("=========================================================="))
        return 0
    else:
        print(red(f"  ISSUES FOUND ({failed}/{total} checks failed)"))
        print(gray(f"  Total runtime: {int(total_elapsed)}s"))
        print(gray("=========================================================="))
        print()
        print(red("Failed checks:"))
        for name, p, detail in check_results:
            if not p:
                print(red(f"  - {name}: {detail}"))
        return 1


if __name__ == "__main__":
    sys.exit(main())
