#!/usr/bin/env python3
"""
flash.py - One-command, foolproof flasher for PolyCast5 (ESP32-C5)

Run this from an ESP-IDF terminal (so `idf.py` and `esptool` are importable):

    python flash.py

That's it. The script figures out everything else:

  * Auto-detects the serial port (the built-in "USB JTAG/serial debug unit").
  * Builds first (incremental - a no-op if nothing changed) so you never flash
    a stale binary. Skip with --no-build if you already built.
  * Flashes ONLY the partitions whose contents actually changed since the last
    flash, so an app-only change re-flashes just the app (seconds instead of
    the full ~2 minutes).
  * Gets the encryption right automatically for the current dev-mode Flash
    Encryption setup: bootloader / partition-table / otadata / app are written
    ENCRYPTED; the LittleFS `assets` partition is written PLAINTEXT (writing it
    encrypted is exactly what causes `esp_littlefs: mount failed`). It reads the
    real settings from the build AND cross-checks the chip's efuse state, so if
    you ever turn Flash Encryption off it adapts - and it refuses to flash if
    the build and the chip disagree (which would brick the board).

Common usage:
    python flash.py                 # build, then flash whatever changed
    python flash.py --no-build      # flash whatever changed (you built already)
    python flash.py --all           # force-flash every partition (safe recovery)
    python flash.py --monitor       # flash, then open idf.py monitor
    python flash.py -p COM7         # skip auto-detect, use this port
    python flash.py --dry-run       # show the plan, flash nothing
    python flash.py --list-ports    # just list detected serial ports and exit
    python flash.py --erase --yes   # full chip erase + reflash (DESTROYS NVS,
                                    #   saved Wi-Fi/keys/settings - last resort)

On Linux/macOS the port looks like /dev/ttyUSB0 or /dev/ttyACM0 instead of COMx;
auto-detect still works, or pass it with  -p /dev/ttyACM0 .

How "what changed" works:
    After a successful flash the script records the SHA-256 of every partition
    image it put on the chip, plus the chip's MAC, in build/flash_state.json.
    On the next run it re-hashes the freshly built images and only flashes the
    ones whose hash differs. If the MAC doesn't match (different board), the
    state file is missing/unreadable, or the chip's identity can't be read, it
    safely falls back to flashing everything. The record only reflects flashes
    done BY THIS SCRIPT on THIS machine - if you flash with `idf.py flash` or
    another tool in between, run `python flash.py --all` once to resync.

Why a separate plaintext write for assets:
    With Flash Encryption on, the hardware transparently decrypts only the
    partitions it's supposed to (bootloader, partition table, app, and anything
    flagged `encrypted` in partitions.csv). The LittleFS `assets` partition is
    NOT flagged, so if it's written encrypted the firmware reads ciphertext and
    the mount fails. This script writes it as plaintext - the automated
    equivalent of the manual recovery:
        idf.py -p PORT encrypted-flash
        esptool --chip esp32c5 -p PORT erase-region 0x850000 0x7B0000
        esptool --chip esp32c5 -p PORT write-flash 0x850000 build/assets.bin

OTA note:
    This tool always targets the ota_0 app slot (0x50000) and re-writes otadata
    whenever it flashes the app, so the board boots the firmware you just
    flashed even if it had previously OTA'd into the ota_1 slot.

Exit code: 0 on success (or nothing-to-do), non-zero on any failure.

----------------------------------------------------------------------------
Trace map (top to bottom): the flow lives in main(), which runs eight numbered
steps: 1) build, 2) load build outputs, 3) build a per-partition "plan",
4) connect + preflight the chip, 5) decide what changed, 6) print the plan,
7) flash, 8) save state. Everything above main() is a helper those steps call.
----------------------------------------------------------------------------
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path


# ===========================================================================
# Paths. Everything is resolved relative to this script's directory so the
# tool works no matter where it's launched from.
# ===========================================================================
ROOT           = Path(__file__).resolve().parent      # repo root (where this file lives)
BUILD_DIR      = ROOT / "build"                        # idf.py build output dir
FLASHER_ARGS   = BUILD_DIR / "flasher_args.json"       # offsets + flash params, written by build
SDKCONFIG_JSON = BUILD_DIR / "config" / "sdkconfig.json"  # resolved config (for the FE setting)
PARTITIONS_CSV = ROOT / "partitions.csv"               # source of each partition's type/flags
STATE_FILE     = BUILD_DIR / "flash_state.json"        # our change-tracking manifest

# Data-partition subtypes that the flash-encryption hardware does NOT decrypt
# on read, so their images must be written as PLAINTEXT even when FE is on.
# (nvs has its own NVS-encryption scheme and is likewise not FE-encrypted, but
# it is not part of the normal flash set.)
PLAINTEXT_SUBTYPES = {"littlefs", "spiffs", "fat", "fatfs", "nvs"}

# Espressif USB vendor id; the built-in USB-Serial-JTAG uses product id 0x1001.
# Used by autodetect_port() to recognise the board among other COM/tty devices.
ESPRESSIF_VID = 0x303A
USB_JTAG_PID  = 0x1001

# Retry serial connects a few times - the built-in USB-Serial-JTAG occasionally
# needs a second attempt to drop back into the download stub between commands.
CONNECT_ATTEMPTS = "3"


# ===========================================================================
# Color output. Copied from verify_encryption.py so the two tools look alike;
# auto-disables when stdout isn't a TTY.
# ===========================================================================
def _enable_colors() -> bool:
    # No colors when output is redirected to a file/pipe (keeps logs clean).
    if not sys.stdout.isatty():
        return False
    if os.name == "nt":
        # Windows consoles need VT processing turned on before ANSI codes work.
        try:
            import ctypes
            kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
            handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
            mode = ctypes.c_ulong()
            kernel32.GetConsoleMode(handle, ctypes.byref(mode))
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
        except Exception:
            return False  # old console without VT support -> plain text
    return True

USE_COLOR = _enable_colors()  # decided once at import time

def _c(text: str, code: str) -> str:
    # Wrap text in an ANSI color escape, or return it unchanged if colors are off.
    return f"\x1b[{code}m{text}\x1b[0m" if USE_COLOR else text

# Thin named wrappers around _c() so call sites read as green("...") etc.
def green(s: str)  -> str: return _c(s, "32")
def red(s: str)    -> str: return _c(s, "31")
def yellow(s: str) -> str: return _c(s, "33")
def cyan(s: str)   -> str: return _c(s, "36")
def gray(s: str)   -> str: return _c(s, "90")
def bold(s: str)   -> str: return _c(s, "1")


def die(msg: str, *hints: str) -> "NoReturn":   # type: ignore[name-defined]
    # Print a red error line, then any yellow follow-up hints, then exit non-zero
    # so the shell / caller sees the failure. Used everywhere a run can't continue.
    print(red(f"\nERROR: {msg}"))
    for h in hints:
        print(yellow(f"  {h}"))
    sys.exit(1)


def human_size(n: float) -> str:
    # Format a byte count for the plan table. Step B -> KB -> MB, dividing by
    # 1024 each loop, and stop at the first unit where the value is < 1024 (or
    # when we hit MB, the largest unit we display).
    for unit in ("B", "KB", "MB"):
        if n < 1024 or unit == "MB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} MB"  # unreachable, but keeps the type checker happy


# ===========================================================================
# partitions.csv parsing - we need each partition's type/subtype and flags to
# decide encrypted-vs-plaintext, plus its size to erase a filesystem region.
# ===========================================================================
def parse_num(s: str) -> int:
    """Parse an ESP-IDF size/offset: hex (0x..), decimal, or K/M suffixed."""
    s = s.strip()
    mult = 1
    # Strip a trailing K/M unit (e.g. "236K", "4M") and remember its multiplier;
    # the remaining text is a bare number parsed below.
    if s and s[-1] in "kK":
        mult, s = 1024, s[:-1]
    elif s and s[-1] in "mM":
        mult, s = 1024 * 1024, s[:-1]
    # "0x..." is hex, anything else is decimal.
    base = 16 if s.lower().startswith("0x") else 10
    return int(s, base) * mult


class Partition:
    # One row of partitions.csv. __slots__ keeps it lightweight and typo-proof.
    __slots__ = ("name", "ptype", "subtype", "offset", "size", "flags")

    def __init__(self, name, ptype, subtype, offset, size, flags):
        self.name = name
        self.ptype = ptype        # "app" or "data"
        self.subtype = subtype    # e.g. "ota_0", "littlefs", "nvs", "ota" (otadata)
        self.offset = offset      # absolute flash offset (int)
        self.size = size          # partition size in bytes (int)
        self.flags = flags        # set of lowercase flag strings (e.g. {"encrypted"})


def load_partitions() -> dict[int, Partition]:
    """Return {offset: Partition}. Empty dict if partitions.csv is absent."""
    parts: dict[int, Partition] = {}
    if not PARTITIONS_CSV.exists():
        return parts  # caller decides whether a missing CSV is fatal (it is under FE)
    for raw in PARTITIONS_CSV.read_text().splitlines():
        # Drop comments (everything after '#') and skip blank lines.
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        # Columns: Name, Type, SubType, Offset, Size, [Flags]
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            continue  # malformed row - not enough columns
        name, ptype, subtype, offset, size = cols[:5]
        # Flags column is optional; collect any non-empty extras as a lowercase set.
        flags = set(f.strip().lower() for f in cols[5:] if f.strip())
        try:
            off = parse_num(offset)
            sz = parse_num(size)
        except ValueError:
            continue  # offset/size we can't parse -> skip this row
        parts[off] = Partition(name, ptype.lower(), subtype.lower(), off, sz, flags)
    return parts


# ===========================================================================
# Encryption decision. This is the safety-critical part: writing an image to a
# partition with the wrong encrypt setting either bricks the boot (plaintext
# written where HW expects ciphertext) or corrupts the filesystem (ciphertext
# written where HW expects plaintext).
# ===========================================================================
def should_encrypt(fe_enabled: bool, part: Partition | None) -> bool:
    """Decide whether the image at this offset must be flashed --encrypt.

    Rules (only relevant when Flash Encryption is enabled):
      * bootloader / partition-table (no entry in partitions.csv) -> encrypted
      * any partition explicitly flagged `encrypted` in partitions.csv -> encrypted
      * filesystem/NVS data partitions -> PLAINTEXT (HW won't decrypt them)
      * everything else (app, otadata, phy) -> encrypted
    """
    if not fe_enabled:
        return False            # FE off -> nothing is encrypted, ever
    if part is None:
        return True             # bootloader / partition table -> always FE-encrypted
    if "encrypted" in part.flags:
        return True             # explicitly flagged in partitions.csv (e.g. nvs_keys)
    if part.subtype in PLAINTEXT_SUBTYPES:
        return False            # littlefs/spiffs/fat/nvs -> HW won't decrypt -> plaintext
    return True                 # app, otadata, phy, ... -> encrypted


# ===========================================================================
# Build inputs. Small readers for the three files the build produces that we
# depend on: sdkconfig.json (the FE setting), flasher_args.json (offsets), and
# the binaries themselves (hashed for change detection).
# ===========================================================================
def fe_enabled_from_build() -> bool:
    """Read the real Flash Encryption setting the firmware was built with."""
    try:
        cfg = json.loads(SDKCONFIG_JSON.read_text())
    except Exception:
        return False  # no/unreadable config -> assume FE off (callers also re-check the chip)
    return bool(cfg.get("SECURE_FLASH_ENC_ENABLED", False))


def load_flasher_args() -> dict:
    # flasher_args.json is the source of truth for offsets and flash params; it
    # is regenerated by every build. Without it we have nothing to flash.
    if not FLASHER_ARGS.exists():
        die(f"{FLASHER_ARGS} not found.",
            "Build the project first (`idf.py build`) or run without --no-build.")
    try:
        return json.loads(FLASHER_ARGS.read_text())
    except Exception as e:
        die(f"could not parse {FLASHER_ARGS}: {e}")


def sha256_file(path: Path) -> str:
    # Stream the file in 1 MiB chunks so hashing the 7.6 MB assets image doesn't
    # load it all into memory. The digest is what change-detection compares.
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ===========================================================================
# esptool / idf.py invocation helpers.
# ===========================================================================
def esptool_cmd(port: str, chip: str, before: str, after: str, *args: str) -> list[str]:
    # Build an argv list for `python -m esptool ...`. Using sys.executable means
    # we run the esptool bundled with the same ESP-IDF Python that launched us.
    # `before`/`after` are the reset behaviours; `*args` is the subcommand + its
    # arguments (e.g. "write-flash", "--encrypt", "0x50000", "PolyCast5.bin").
    return [sys.executable, "-m", "esptool", "--chip", chip, "-p", port,
            "--connect-attempts", CONNECT_ATTEMPTS,
            "--before", before, "--after", after, *args]


def run(cmd: list[str], capture: bool = False, fatal: bool = True):
    """Run a command. On a native non-zero exit: abort (fatal) or return None.

    capture=True returns a CompletedProcess with .stdout populated."""
    # Echo a readable form of the command (shorten the long python path to just
    # "python.exe") so the user can see / copy exactly what's being run.
    pretty = " ".join(Path(c).name if c == sys.executable else c for c in cmd)
    print(gray(f"  $ {pretty}"))
    try:
        if capture:
            # Capture stdout (with stderr merged in) for parsing, e.g. read-mac.
            return subprocess.run(cmd, check=True, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        # Stream output live (flash progress bars, build log) to the terminal.
        return subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        # Non-zero exit. Non-fatal callers (preflight) just get None and degrade.
        if not fatal:
            return None
        if capture and e.output:
            print(e.output)  # surface the captured output before dying
        die(f"command failed (exit {e.returncode}).",
            "Is the device connected and the right port selected?",
            "Close any open idf.py monitor / serial terminal holding the port.",
            "If the device was left half-flashed, recover with:  python flash.py --all")
    except FileNotFoundError:
        # The executable itself couldn't be launched (e.g. esptool not installed).
        if not fatal:
            return None
        die("could not launch the command.",
            "Run this from an ESP-IDF terminal so `esptool` is importable.")


def find_idf_py() -> Path | None:
    # Locate idf.py via the IDF_PATH env var the ESP-IDF terminal exports.
    # Returns None if IDF_PATH isn't set or the file isn't there.
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        return None
    candidate = Path(idf_path) / "tools" / "idf.py"
    return candidate if candidate.exists() else None


# ===========================================================================
# Port auto-detection.
# ===========================================================================
def list_serial_ports():
    # Returns a list of pyserial port objects, or None if pyserial isn't
    # installed (so callers can give a clear "pass -p" message instead of crashing).
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    return list(list_ports.comports())


def describe_port(p) -> str:
    # One-line, human-readable summary of a port for the picker / error lists.
    # vid/pid can be None for non-USB ports, hence the guards.
    vid = f"{p.vid:04X}" if p.vid is not None else "----"
    pid = f"{p.pid:04X}" if p.pid is not None else "----"
    # serial_number distinguishes two otherwise-identical boards on a bench.
    sn = f"  sn={p.serial_number}" if getattr(p, "serial_number", None) else ""
    return f"{p.device}  [{vid}:{pid}]  {p.description}{sn}"


def autodetect_port() -> str:
    ports = list_serial_ports()
    if ports is None:
        die("pyserial is not available, so the port can't be auto-detected.",
            "Pass the port explicitly, e.g.  python flash.py -p COM7")
    if not ports:
        die("no serial ports found.",
            "Plug the device in and check the USB cable, then retry.")

    # Three increasingly-loose candidate groups, tried most-specific first:
    esp_ports  = [p for p in ports if p.vid == ESPRESSIF_VID]            # any Espressif USB device
    jtag_ports = [p for p in esp_ports if p.pid == USB_JTAG_PID]        # the built-in USB-Serial-JTAG
    desc_ports = [p for p in ports                                       # USB-UART bridges by description
                  if p.description and re.search(r"JTAG|USB Serial|CP210|CH34", p.description, re.I)]

    # Walk the tiers: exactly one match -> use it; more than one -> ask the user.
    for group in (jtag_ports, esp_ports, desc_ports):
        if len(group) == 1:
            chosen = group[0]
            print(cyan(f"Auto-detected port: {describe_port(chosen)}"))
            return chosen.device
        if len(group) > 1:
            # Several equally-good matches -> don't guess, list them and stop.
            print(red("Could not auto-pick a port; multiple matching boards found:"))
            for p in group:
                print(yellow(f"  {describe_port(p)}"))
            die("ambiguous serial port (more than one board attached).",
                "Unplug all but one, or pass the right one with  -p COMx",
                "Use the sn= serial number above to tell identical boards apart.")

    # No tier matched but there's exactly one port overall -> it's almost certainly it.
    if len(ports) == 1:
        print(cyan(f"Auto-detected port: {describe_port(ports[0])}"))
        return ports[0].device

    # Several ports, none recognisably a board -> let the user choose.
    print(red("Could not auto-pick a port; multiple candidates found:"))
    for p in ports:
        print(yellow(f"  {describe_port(p)}"))
    die("ambiguous serial port.",
        "Pick one and pass it, e.g.  python flash.py -p COM7")


# ===========================================================================
# Device preflight: connectivity, identity (MAC), and the actual on-chip
# flash-encryption state. All non-fatal - if the chip can't be read we degrade
# to "flash everything" rather than aborting, EXCEPT a confirmed FE mismatch,
# which we refuse because flashing it would brick the board.
# ===========================================================================
def read_chip_mac(port: str, chip: str) -> str | None:
    """Return the board's base MAC, or None if it couldn't be read/parsed."""
    # fatal=False: a flaky read here must not kill the run; we just lose the
    # identity check and fall back to flashing everything. hard-reset leaves the
    # board running its app afterwards (in case we end up flashing nothing).
    proc = run(esptool_cmd(port, chip, "default-reset", "hard-reset", "read-mac"),
               capture=True, fatal=False)
    if proc is None:
        print(yellow("Couldn't read the chip MAC (connect failed); will flash everything."))
        return None
    out = proc.stdout or ""
    # On C5/C6 esptool prints an 8-byte EUI64 on the first "MAC:" line and the
    # real 6-byte address on "BASE MAC:". Prefer BASE MAC; accept exactly 6 octets.
    six = r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})"
    m = (re.search(r"BASE MAC:\s*" + six, out)                  # preferred: the true 6-octet MAC
         or re.search(r"(?m)^MAC:\s*" + six + r"\s*$", out))    # fallback for chips without EUI64
    if m:
        return m.group(1).upper()
    print(yellow("Connected, but couldn't parse the MAC; will flash everything."))
    return None


def read_chip_fe(port: str, chip: str) -> bool | None:
    """Return the chip's actual Flash Encryption state (True/False), or None if
    it couldn't be determined."""
    # esptool's get-security-info prints a line "Flash Encryption: Enabled/Disabled".
    proc = run(esptool_cmd(port, chip, "default-reset", "hard-reset", "get-security-info"),
               capture=True, fatal=False)
    if proc is None:
        return None  # couldn't read -> caller proceeds based on the build config
    m = re.search(r"Flash[ _]Encryption:\s*(Enabled|Disabled)", proc.stdout or "", re.I)
    if not m:
        return None
    return m.group(1).lower() == "enabled"


# ===========================================================================
# Change-tracking state (build/flash_state.json).
# ===========================================================================
def load_state() -> dict | None:
    # Read the last-flash manifest. Any problem (missing/corrupt) -> None, which
    # the caller treats as "no record" and flashes everything.
    try:
        return json.loads(STATE_FILE.read_text())
    except Exception:
        return None


def save_state(chip: str, mac: str | None, fe: bool, plan: list[dict]) -> None:
    # Record what is now on the chip: per-offset file + hash + encrypt flag,
    # keyed by offset, plus the chip identity (mac) and FE setting. Next run
    # compares fresh hashes against this to decide what to skip.
    files = {p["offset_hex"]: {"file": p["rel"], "sha256": p["sha"], "encrypted": p["encrypt"]}
             for p in plan}
    state = {"chip": chip, "mac": mac, "flash_encryption": fe, "files": files}
    try:
        STATE_FILE.write_text(json.dumps(state, indent=2))
    except Exception as e:
        # Not fatal - we just lose the optimization; next run re-flashes everything.
        print(yellow(f"Note: could not write {STATE_FILE.name} ({e}); "
                     "next run will re-flash everything."))


# ===========================================================================
# Destructive-action confirmation.
# ===========================================================================
def confirm_erase(assume_yes: bool) -> None:
    # Loud warning + typed confirmation before a full chip erase, because it
    # destroys partitions this tool never rewrites (nvs / nvs_keys / phy_init).
    print(red(bold("\n!!  --erase performs a FULL CHIP ERASE  !!")))
    print(yellow("    This wipes partitions this tool does NOT re-flash, including:"))
    print(yellow("      * nvs        - all saved Wi-Fi passwords, API keys, settings"))
    print(yellow("      * nvs_keys   - the NVS encryption keys (regenerated, old values lost)"))
    print(yellow("      * phy_init   - RF calibration data"))
    print(yellow("    For a normal 'flash everything' recovery, use --all instead "
                 "(it keeps NVS/keys)."))
    if assume_yes:
        print(yellow("    --yes given; proceeding."))
        return
    # No TTY to prompt on (e.g. CI) -> refuse rather than hang/guess.
    if not sys.stdin or not sys.stdin.isatty():
        die("refusing --erase without confirmation in a non-interactive shell.",
            "Re-run with --erase --yes if you really mean it.")
    resp = input(red("Type ERASE to continue (anything else aborts): "))
    if resp.strip() != "ERASE":
        die("aborted; nothing was erased.")


# ===========================================================================
# Main.
# ===========================================================================
def main() -> int:
    # ---- Parse command-line options -----------------------------------------
    parser = argparse.ArgumentParser(
        description="Foolproof, change-aware flasher for PolyCast5 (ESP32-C5).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("-p", "--port", help="Serial port (default: auto-detect).")
    parser.add_argument("--no-build", action="store_true",
                        help="Don't run `idf.py build` first (use existing build/).")
    parser.add_argument("--all", "--full", dest="all", action="store_true",
                        help="Flash every partition, ignoring change detection (safe recovery).")
    parser.add_argument("--erase", action="store_true",
                        help="FULL chip erase before flashing (DESTROYS NVS/keys/settings; "
                             "last-resort recovery - prefer --all).")
    parser.add_argument("--yes", action="store_true",
                        help="Skip the interactive confirmation for --erase.")
    parser.add_argument("--monitor", action="store_true",
                        help="Open `idf.py monitor` after flashing.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show the plan but don't touch the device.")
    parser.add_argument("--list-ports", action="store_true",
                        help="List detected serial ports and exit.")
    args = parser.parse_args()

    print(bold(cyan("\n=== PolyCast5 flash ===")))

    # --list-ports is a pure utility: print ports and exit before doing anything.
    if args.list_ports:
        ports = list_serial_ports()
        if not ports:
            print(yellow("No serial ports found (or pyserial unavailable)."))
            return 0
        print("Detected serial ports:")
        for p in ports:
            print(f"  {describe_port(p)}")
        return 0

    # -- 1. Build (default on) -------------------------------------------------
    # Build first so the binaries always match the current source. The build is
    # incremental, so it's a fast no-op when nothing changed. --no-build skips it.
    if not args.no_build:
        idf_py = find_idf_py()
        if idf_py is None:
            die("can't run the build: IDF_PATH isn't set / idf.py not found.",
                "Run this from an ESP-IDF terminal,",
                "or pass --no-build if you've already built.")
        print(cyan("Building (incremental)..."))
        run([sys.executable, str(idf_py), "-C", str(ROOT), "build"])  # aborts on build failure
    else:
        print(gray("Skipping build (--no-build)."))

    # -- 2. Load build outputs + settings -------------------------------------
    # Everything we flash is described by flasher_args.json; the FE setting comes
    # from the resolved sdkconfig; the partition table tells us encrypt-vs-plaintext.
    fa = load_flasher_args()
    fe = fe_enabled_from_build()                       # is Flash Encryption on in this build?
    extra = fa.get("extra_esptool_args", {})
    chip = extra.get("chip", "esp32c5")                # e.g. "esp32c5"
    before = extra.get("before", "default-reset")      # reset-before behaviour for esptool
    write_flash_args = fa.get("write_flash_args", [])  # ["--flash-mode","dio","--flash-size","16MB",...]
    flash_files: dict[str, str] = fa.get("flash_files", {})  # {"0x50000": "PolyCast5.bin", ...}
    parts = load_partitions()                          # {offset: Partition}

    if not flash_files:
        die("no flash_files in flasher_args.json - nothing to flash.")
    # Under FE we MUST know each partition's subtype to avoid encrypting littlefs;
    # a missing partition table would force us to guess, so refuse instead.
    if fe and not parts:
        die("Flash Encryption is on but partitions.csv couldn't be read.",
            "partitions.csv is required to decide which partitions are encrypted;",
            "refusing to flash blind (would risk corrupting littlefs or bricking boot).")

    # Offsets below the first real partition belong to the bootloader / partition
    # table, which are always FE-encrypted and aren't listed in partitions.csv.
    first_part_off = min(parts) if parts else 0x10000

    # -- 3. Build the per-partition plan --------------------------------------
    # Turn flash_files into a list of plan dicts, one per image, carrying
    # everything later steps need: offset, file, size, encrypt flag, hash, role.
    plan: list[dict] = []
    for off_hex, rel in sorted(flash_files.items(), key=lambda kv: int(kv[0], 16)):
        off = int(off_hex, 16)
        path = BUILD_DIR / rel
        if not path.exists():
            die(f"build artifact missing: {path}",
                "Run a full build (`idf.py build`) and try again.")
        part = parts.get(off)  # the matching partitions.csv row, or None for boot/PT
        # A flash_files offset with no partitions.csv match, sitting at or above
        # the first partition, is something we can't classify safely under FE.
        if fe and part is None and off >= first_part_off:
            die(f"offset {off_hex} isn't in partitions.csv; can't decide "
                f"encrypted-vs-plaintext safely under Flash Encryption.",
                "Check that partitions.csv matches the build.")
        # Friendly display name: partition name if known, else infer from the path.
        if part:
            name = part.name
        elif "bootloader" in rel:
            name = "bootloader"
        elif "partition" in rel:
            name = "partition-table"
        else:
            name = f"@{off_hex}"
        plan.append({
            "offset": off,                                   # int offset
            "offset_hex": f"0x{off:x}",                      # "0x50000" (esptool arg + state key)
            "rel": rel,                                      # path relative to build/
            "path": path,                                    # absolute path to the image
            "size": path.stat().st_size,                     # bytes (for the table + erase logic)
            "part": part,                                    # Partition or None
            "name": name,
            "is_app": bool(part and part.ptype == "app"),    # is this the app slot?
            "is_otadata": bool(part and part.subtype == "ota"),  # is this the otadata partition?
            "encrypt": should_encrypt(fe, part),             # THE encrypt-vs-plaintext decision
            "sha": sha256_file(path),                        # content hash for change detection
        })

    # -- 4. Port + device preflight (identity + on-chip FE state) -------------
    if args.dry_run:
        # Dry-run never touches hardware: don't auto-detect or connect.
        port = args.port or "(auto-detect at flash time)"
        mac = None
    else:
        port = args.port or autodetect_port()
        print(cyan("Connecting to device..."))
        mac = read_chip_mac(port, chip)  # also serves as the connectivity check
        if mac:
            print(green(f"Connected. Chip MAC: {mac}"))

        # Refuse to flash if the build's encryption assumption disagrees with the
        # chip's real efuse state - that mismatch is the classic way to brick.
        chip_fe = read_chip_fe(port, chip)
        if chip_fe is None:
            # Couldn't read it -> proceed on the build's setting (best effort).
            print(yellow("Couldn't read the chip's flash-encryption state; "
                         "proceeding based on the build config."))
        elif chip_fe != fe:
            print(red(f"\nFlash-encryption MISMATCH: build expects FE="
                      f"{'ON' if fe else 'OFF'} but the chip reports FE="
                      f"{'ON' if chip_fe else 'OFF'}."))
            if fe and not chip_fe:
                # Build wants encrypted images but the chip has no FE key yet.
                die("flashing encrypted images to a chip that doesn't have flash "
                    "encryption enabled would brick it.",
                    "If this is a brand-new board, do the FIRST flash with "
                    "`idf.py -p PORT flash` (plaintext) - the bootloader enables flash "
                    "encryption on first boot - then use this script for every flash after.",
                    "Otherwise rebuild with the encryption setting that matches the board.")
            else:
                # Chip is encrypted but the build is plaintext - the reverse brick.
                die("flashing plaintext images to a chip that HAS flash encryption "
                    "enabled would brick it.",
                    "Rebuild with CONFIG_SECURE_FLASH_ENC_ENABLED=y to match the board.")
        else:
            print(green(f"Chip flash-encryption: {'ON' if chip_fe else 'OFF'} (matches build)."))

    # -- 5. Decide what changed -----------------------------------------------
    # `reason` is set when we should flash EVERYTHING (skips per-file hashing).
    # Otherwise we compare each image's hash to the last-flash record.
    state = load_state()
    reason = None
    if args.all or args.erase:
        reason = "forced (--all/--erase)"
    elif state is None:
        reason = "no previous flash record"
    elif not args.dry_run and mac is None:
        reason = "device identity unknown"               # can't trust the record for this board
    elif not args.dry_run and state.get("mac") != mac:
        reason = "different board (MAC mismatch)"         # record belongs to another unit
    elif state.get("flash_encryption") != fe:
        reason = "flash-encryption setting changed"       # encrypt flags may all differ now

    # Mark each plan entry changed/unchanged.
    prev_files = (state or {}).get("files", {})
    for p in plan:
        if reason is not None:
            p["changed"] = True                           # full flash
        else:
            rec = prev_files.get(p["offset_hex"])         # last-flashed record at this offset
            p["changed"] = (rec is None or rec.get("sha256") != p["sha"])  # new or hash differs

    # Whenever the app is (re)flashed, also write otadata so the bootloader
    # selects the slot we just wrote (ota_0) instead of an OTA'd ota_1.
    if any(p["changed"] and p["is_app"] for p in plan):
        for p in plan:
            if p["is_otadata"]:
                p["changed"] = True

    if reason:
        print(yellow(f"\nFlashing everything: {reason}."))

    # -- 6. Print the plan ----------------------------------------------------
    # One row per partition: FLASH/skip, offset, name, encrypted/plaintext, size, file.
    print(bold(f"\nPlan  (chip {chip}, flash encryption {'ON' if fe else 'OFF'}):"))
    enc_label = lambda e: (green("encrypted") if e else yellow("plaintext "))
    for p in plan:
        tag = bold(red("FLASH")) if p["changed"] else gray("skip ")
        print(f"  {tag}  {p['offset_hex']:>9}  {p['name']:<14} "
              f"{enc_label(p['encrypt'])}  {human_size(p['size']):>9}  {p['rel']}")

    # The subset we'll actually write this run.
    to_flash = [p for p in plan if p["changed"]]
    if not to_flash:
        # Fast path: hashes all match -> nothing to do.
        print(green("\nDevice already up to date - nothing to flash.")
              + gray("  (based on this script's last flash; use --all if it was "
                     "flashed another way or erased.)"))
        if args.monitor and not args.dry_run:
            open_monitor(port)
        return 0

    if args.dry_run:
        # Stop here for dry-run: plan shown, device untouched.
        print(cyan(f"\n[dry-run] would flash {len(to_flash)} partition(s); "
                   "no changes made."))
        return 0

    if args.erase:
        confirm_erase(args.yes)  # may abort the whole run

    # -- 7. Flash -------------------------------------------------------------
    # Build an ordered list of esptool steps. Order matters so the device resets
    # into the app exactly once, at the very end. Encrypted images go first (one
    # combined --encrypt write), then each plaintext filesystem partition, to
    # dodge the littlefs-mount-failed trap.
    steps: list[list[str]] = []

    # Split the work by encryption: everything encrypted can share one write.
    enc_to_flash = [p for p in to_flash if p["encrypt"]]
    plain_to_flash = [p for p in to_flash if not p["encrypt"]]

    if enc_to_flash:
        # One `write-flash --encrypt` with all changed encrypted images, as
        # offset/file pairs sorted by offset (e.g. 0x2000 boot, 0x50000 app).
        pairs: list[str] = []
        for p in sorted(enc_to_flash, key=lambda p: p["offset"]):
            pairs += [p["offset_hex"], str(p["path"])]
        steps.append(["write-flash", "--encrypt", *write_flash_args, *pairs])

    for p in sorted(plain_to_flash, key=lambda p: p["offset"]):
        part = p["part"]
        # write-flash erases the sectors it writes, so a partition-filling image
        # already wipes any stale (possibly encrypted) bytes. Only when the image
        # is smaller than the partition do we erase the tail first - and on an
        # FE-enabled chip that erase needs --force to pass esptool's safety gate.
        if part is not None and p["size"] < part.size:
            steps.append(["erase-region", "--force", p["offset_hex"], f"0x{part.size:x}"])
        # Plaintext write (no --encrypt) - this is the assets/littlefs partition.
        steps.append(["write-flash", *write_flash_args, p["offset_hex"], str(p["path"])])

    if args.erase:
        # Full chip erase runs first of all. erase-flash is gated behind --force
        # once FE is burned, same as erase-region.
        steps.insert(0, ["erase-flash", "--force"])

    # Run the steps. Every step resets back into the bootloader before (default-
    # reset); only the LAST step boots the app (hard-reset), so the board resets
    # into firmware exactly once when the whole flash is done.
    print(cyan(f"\nFlashing {len(to_flash)} partition(s)..."))
    for i, step_args in enumerate(steps):
        after = "hard-reset" if i == len(steps) - 1 else "no-reset"
        run(esptool_cmd(port, chip, before, after, *step_args))  # aborts on any failure

    # -- 8. Record new state + optional monitor -------------------------------
    # Only reached if every step above succeeded. Persist the new on-chip state
    # so the next run can skip unchanged partitions.
    save_state(chip, mac, fe, plan)
    print(green(bold("\nDone. Firmware flashed successfully.")))

    if args.monitor:
        open_monitor(port)
    return 0


def open_monitor(port: str) -> None:
    # Hand off to `idf.py monitor`. Needs IDF_PATH (same as the build step).
    idf_py = find_idf_py()
    if idf_py is None:
        print(yellow("Can't open monitor: IDF_PATH not set."))
        return
    print(cyan(f"\nOpening monitor on {port} (Ctrl-] to exit)...\n"))
    # Blocks until the user exits the monitor; output streams straight through.
    subprocess.run([sys.executable, str(idf_py), "-C", str(ROOT),
                    "monitor", "-p", port])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        # Ctrl-C: exit quietly with the conventional 130 code instead of a traceback.
        print(red("\nInterrupted."))
        sys.exit(130)
