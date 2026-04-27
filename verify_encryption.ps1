# =============================================================================
# verify_encryption.ps1
#
# Verifies Flash Encryption (FE) and NVS Encryption are actually working on a
# PolyCast5 device by dumping raw flash partitions over UART and inspecting
# them for plaintext leaks.
#
# Note this is of default DEV mode encryption only!
# Enable full protection: www.polycast5.com/blogs/docs/lock-it-down
#
# How it works:
#   1. esptool reads raw bytes directly from flash (NOT through CPU/MMU). This
#      means encrypted partitions come back as ciphertext - exactly what an
#      attacker with a chip-off setup or USB cable would get.
#   2. We compare each dump to the plaintext build artifact in build/ to prove
#      the difference (encrypted dump = no readable strings; build artifact =
#      lots of readable strings).
#   3. The "killer test" stores a known secret on the device, then proves it
#      cannot be found anywhere in the flash dump.
#
# Usage examples:
#   .\verify_encryption.ps1                                         # default port, no killer test
#   .\verify_encryption.ps1 -Port COM7                              # different port
#   .\verify_encryption.ps1 -Secret "TestPassword_PolyCast5_12345"  # killer test enabled
#   .\verify_encryption.ps1 -Port COM13 -Secret "Yellow-Guyana-34?" # port + killer test
#   .\verify_encryption.ps1 -SkipDump                               # reuse existing dumps
#   .\verify_encryption.ps1 -SkipDump -Secret "AnotherSecret"       # try another secret, no re-dump
#
# Parameters (all optional):
#   -Port <string>      Serial port the chip is on. Default: COM14
#   -Secret <string>    Plaintext secret to grep for in all dumps. Empty = skip killer test.
#   -SkipDump           Reuse existing dump_*.bin files instead of re-dumping (~30s saved).
#
# Typical workflow:
#   1. Flash firmware, save a known Wi-Fi password to the device.
#   2. Reboot the device, wait for it to reconnect.
#   3. Run: .\verify_encryption.ps1 -Port COM13 -Secret "Yellow-Guyana-34?"
#   4. Iterate on more secrets with -SkipDump (no re-dump needed).
#
# Exit code: 0 if all checks pass, 1 if any check fails. Useful for scripting.
#
# If PowerShell blocks the script (first run):
#   Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
# =============================================================================

param(
    # Serial port the ESP32-C5 is connected to. Override with -Port COM7 etc.
    [string]$Port   = "COM14",

    # OPTIONAL: a secret string you've previously saved into NVS (e.g. a
    # Wi-Fi password). When provided, the script searches every dump for it
    # and FAILS LOUDLY if found in plaintext anywhere. This is the strongest
    # proof that NVS encryption actually works.
    [string]$Secret = "",

    # OPTIONAL: skip the ~30-second flash dump step and re-use existing
    # dump_*.bin files in the current directory. Useful when iterating on
    # the analysis steps without changing what's on the chip.
    [switch]$SkipDump
)

# Stop on any unhandled error so a failed esptool call halts the script.
$ErrorActionPreference = "Stop"

# Track elapsed time across the whole run for final summary.
$scriptStart = Get-Date

# Locate Python with esptool installed. ESP-IDF ships its own venv with esptool;
# the system Python on most machines does NOT have esptool. Fall back to "python"
# only if the IDF venv isn't found (in which case the user must have esptool
# installed globally for this to work).
$IdfPython = "C:\Espressif\tools\python\v6.0\venv\Scripts\python.exe"
if (Test-Path $IdfPython) {
    $PythonExe = $IdfPython
    Write-Host "Using ESP-IDF Python: $PythonExe" -ForegroundColor DarkGray
}
else {
    $PythonExe = "python"
    Write-Host "WARNING: ESP-IDF Python not found at $IdfPython - falling back to 'python'" -ForegroundColor Yellow
    Write-Host "         If you see 'No module named esptool', open an IDF terminal and rerun." -ForegroundColor Yellow
}

# Common args used in every esptool invocation below.
$EsptoolArgs = @("--chip", "esp32c5", "-p", $Port)

# Helper: run esptool and abort the script if it fails. Native command failures
# don't trigger PowerShell's error system by default - we have to check
# $LASTEXITCODE manually after each call.
function Invoke-Esptool {
    # Note: avoid $Args as a parameter name - it's a PowerShell automatic
    # variable. Use $EsptoolPassthrough instead.
    param([Parameter(ValueFromRemainingArguments)] $EsptoolPassthrough)
    & $PythonExe -m esptool @EsptoolArgs @EsptoolPassthrough
    if ($LASTEXITCODE -ne 0) {
        Write-Host "`nABORT: esptool failed with exit code $LASTEXITCODE" -ForegroundColor Red
        Write-Host "Common causes:" -ForegroundColor Red
        Write-Host "  - Device not connected or wrong -Port (current: $Port)" -ForegroundColor Red
        Write-Host "  - esptool not installed in the active Python environment" -ForegroundColor Red
        Write-Host "  - Another process holding the serial port (close idf.py monitor etc.)" -ForegroundColor Red
        exit 1
    }
}


# ---------------------------------------------------------------------------
# Helper: Strings-Like
#   Mimics the Unix `strings` utility: walks a binary file and extracts every
#   run of >=4 consecutive printable-ASCII bytes, returning them as a single
#   newline-separated blob. We use this to scan dumps for readable text -
#   encrypted partitions should produce ALMOST NOTHING; plaintext partitions
#   produce a flood of readable strings.
# ---------------------------------------------------------------------------
function Strings-Like {
    param([string]$Path)

    $bytes   = [System.IO.File]::ReadAllBytes($Path)
    $result  = New-Object System.Text.StringBuilder      # accumulates output
    $current = New-Object System.Text.StringBuilder      # current run-in-progress

    foreach ($b in $bytes) {
        # Printable ASCII range: space (32) through ~ (126)
        if ($b -ge 32 -and $b -lt 127) {
            [void]$current.Append([char]$b)
        }
        else {
            # Non-printable byte ends the current run. Keep it only if long enough.
            if ($current.Length -ge 4) {
                # Use Append + "`n" rather than AppendLine: AppendLine emits
                # Environment.NewLine ("\r\n" on Windows), which would leave a
                # stray \r on every string after downstream `-split "`n"`,
                # inflating reported Length by 1 char.
                [void]$result.Append($current.ToString())
                [void]$result.Append("`n")
            }
            [void]$current.Clear()
        }
    }
    # Don't forget a run that extends to the very end of the file.
    if ($current.Length -ge 4) {
        [void]$result.Append($current.ToString())
        [void]$result.Append("`n")
    }

    return $result.ToString()
}


# ---------------------------------------------------------------------------
# Helper: Find-RawByteOccurrences
#   Counts literal occurrences of $Needle in the raw bytes of $FilePath.
#   Used by the killer test (Step 6) INSTEAD of Strings-Like, because
#   Strings-Like discards ASCII runs < 4 chars - so a short leaked secret
#   bordered by non-printable bytes (e.g., NVS null terminators) would slip
#   through that path. This function searches the literal byte sequence
#   regardless of length or surrounding bytes.
#
#   Implementation: map each byte 1:1 to a Unicode char via ISO-8859-1, then
#   leverage .NET's optimized String.IndexOf for fast substring scanning.
#   Works on both PS 5.1 (.NET Framework) and PS 7+ (.NET 5+).
#
#   StringComparison.Ordinal is REQUIRED here. The default IndexOf overload
#   uses CurrentCulture comparison, which (a) treats certain bytes like null
#   and control chars as "ignorable" - producing false-positive matches
#   across non-printable boundaries - and (b) applies linguistic equivalences
#   (e.g., "ss" matching U+00DF in de-DE locales) that would distort raw byte
#   semantics. Ordinal forces a strict byte-for-byte comparison.
# ---------------------------------------------------------------------------
function Find-RawByteOccurrences {
    param(
        [string]$FilePath,
        [string]$Needle
    )
    $bytes    = [System.IO.File]::ReadAllBytes($FilePath)
    $haystack = [System.Text.Encoding]::GetEncoding("iso-8859-1").GetString($bytes)
    $count = 0
    $idx   = 0
    while (($idx = $haystack.IndexOf($Needle, $idx, [System.StringComparison]::Ordinal)) -ge 0) {
        $count++
        $idx++
    }
    return $count
}


# ---------------------------------------------------------------------------
# Result tracking + helper for inline [OK]/[FAIL] markers and final summary.
# Each check calls Add-CheckResult with a short name, pass/fail, and detail.
# At the end we print a verdict block and exit 0 (all pass) or 1 (any fail).
# ---------------------------------------------------------------------------
$checkResults = @()

function Add-CheckResult {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )
    $marker = if ($Passed) { "[OK]  " } else { "[FAIL]" }
    $color  = if ($Passed) { "Green" } else { "Red" }
    Write-Host "  $marker $Detail" -ForegroundColor $color
    $script:checkResults += [PSCustomObject]@{
        Name   = $Name
        Passed = $Passed
        Detail = $Detail
    }
}


# ===========================================================================
# STEP 1 - Read raw flash partitions over UART.
#
# Each `read-flash <offset> <length> <outfile>` returns the bytes EXACTLY as
# they sit on the SPI flash chip - no decryption is performed. So if FE is on,
# encrypted partitions come back as ciphertext.
#
# Offsets and lengths come from partitions.csv:
#   bootloader        @ 0x2000   size 0xE000   (FE-encrypted)
#   nvs_keys          @ 0x11000  size 0x1000   (FE-encrypted)
#   nvs               @ 0x12000  size 0x3B000  (NOT FE-encrypted - but values
#                                                 inside are NVS-encrypted)
#   ota_0 (app)       @ 0x50000  size 0x3B0000 (FE-encrypted)
#   assets (SPIFFS)   @ 0x7B0000 size 0x10000  (PLAINTEXT - first 64 KB sample,
#                                                 enough to verify it's not over-encrypted)
# ===========================================================================
if ($SkipDump) {
    Write-Host "=== Skipping dump (using existing dump_*.bin files) ===" -ForegroundColor DarkGray
}
else {
    Write-Host "=== Dumping partitions from chip ===" -ForegroundColor Cyan
    Invoke-Esptool read-flash 0x2000   0xE000   dump_bootloader.bin
    Invoke-Esptool read-flash 0x11000  0x1000   dump_nvs_keys.bin
    Invoke-Esptool read-flash 0x12000  0x3B000  dump_nvs.bin
    Invoke-Esptool read-flash 0x50000  0x3B0000 dump_app.bin
    Invoke-Esptool read-flash 0x7B0000 0x10000  dump_spiffs_sample.bin
}

# Sanity-check that all dumps exist and are non-empty before analyzing them.
# Catches the case where -SkipDump was used but no prior dumps exist, or where
# esptool somehow returned 0 but produced no file.
$requiredDumps = @(
    "dump_bootloader.bin", "dump_nvs_keys.bin", "dump_nvs.bin",
    "dump_app.bin", "dump_spiffs_sample.bin"
)
foreach ($f in $requiredDumps) {
    if (-not (Test-Path $f) -or (Get-Item $f).Length -eq 0) {
        Write-Host "`nABORT: $f is missing or empty." -ForegroundColor Red
        Write-Host "  Run without -SkipDump to fetch fresh dumps from the chip." -ForegroundColor Yellow
        exit 1
    }
}


# ---------------------------------------------------------------------------
# Pre-extract strings from the heavy dumps once, reuse across multiple checks.
# Strings-Like is a pure-PowerShell byte loop and slow on large files
# (~30 sec for the 3.6 MB app dump). Without caching, the same dumps would
# be re-scanned by multiple steps below.
# ---------------------------------------------------------------------------
Write-Host "`n=== Pre-extracting strings from dumps (one-time) ===" -ForegroundColor Cyan
$extractStart = Get-Date
Write-Host "  Scanning dump_app.bin (3.6 MB, slowest)..."
$dumpAppStrings    = Strings-Like dump_app.bin
Write-Host "  Scanning dump_nvs.bin (236 KB)..."
$dumpNvsStrings    = Strings-Like dump_nvs.bin
Write-Host "  Scanning dump_spiffs_sample.bin (64 KB)..."
$dumpSpiffsStrings = Strings-Like dump_spiffs_sample.bin
$extractElapsed = (Get-Date) - $extractStart
Write-Host "  Done in $([int]$extractElapsed.TotalSeconds)s." -ForegroundColor DarkGray


# ===========================================================================
# STEP 2 - Bootloader encryption check.
#
# Compares the dumped bootloader (from chip) against the plaintext build
# artifact (from your build/ folder). Recognizable strings like "ESP-IDF" and
# "boot:" should appear MANY times in the plaintext but ZERO times in the dump.
# ===========================================================================
Write-Host "`n=== Bootloader encryption check ===" -ForegroundColor Cyan

# Distinctive strings verified to exist in the plaintext bootloader.bin produced
# by ESP-IDF v6.0. These are log messages, IDF source paths, and component names
# burned into the binary as string literals. NONE of them should appear in an
# encrypted flash dump if FE is working.
# Note: "boot:" was removed - it does NOT actually exist as a literal.
$bootSearchTerms = @(
    'ESP-IDF',                          # version banner: "ESP-IDF v6.0 2nd stage bootloader"
    'esp_image',                        # path: esp_image_format.c
    '2nd stage',                        # boot banner literal
    'bootloader_support',               # IDF component path
    'bootloader_utility',               # IDF component path
    'flash_encrypt',                    # FE module log tag
    'Partition Table:',                 # log message printed at boot
    'Loaded app from partition',        # log message
    'Checking flash encryption',        # log message
    'Resetting with flash encryption',  # log message
    'RNG early entropy',                # log message
    'load partition table'              # error message fragment
)
$bootPattern = $bootSearchTerms -join '|'
$dumpedHits = (Strings-Like dump_bootloader.bin)             -split "`n" | Where-Object { $_ -match $bootPattern }
$plainHits  = (Strings-Like build\bootloader\bootloader.bin) -split "`n" | Where-Object { $_ -match $bootPattern }

# Pass if encrypted dump shows zero hits AND plaintext build shows some.
$bootDumpPass  = ($dumpedHits.Count -eq 0)
$bootPlainPass = ($plainHits.Count  -gt 0)
Add-CheckResult -Name "bootloader-dump"  -Passed $bootDumpPass `
    -Detail "encrypted dump matches: $($dumpedHits.Count) (expect 0)"
Add-CheckResult -Name "bootloader-plain" -Passed $bootPlainPass `
    -Detail "plain build matches:    $($plainHits.Count) (expect many)"


# ===========================================================================
# STEP 3 - App encryption check.
#
# Same idea as bootloader, but for the application. We grep for PolyCast5-
# specific identifiers harvested from the codebase: ESP_LOG TAG strings,
# FreeRTOS task names passed to xTaskCreate, and the project name itself.
# All of these are stored as plain string literals in the .bin file. With FE
# active, ZERO of them should appear in the dumped (encrypted) bytes.
# ===========================================================================
Write-Host "`n=== App encryption check ===" -ForegroundColor Cyan

# Project-unique identifiers harvested from the PolyCast5 codebase.
# Sources: ESP_LOG TAGs (`#define TAG "..."`), task names (xTaskCreate),
# project descriptor strings, distinctive log messages.
# Add more here as the codebase grows. Generic terms (wifi/bluetooth/error)
# are deliberately avoided because they show up in IDF strings too.
$appSearchTerms = @(
    'polycast5',
    # GPIO subsystem
    'gpio_task', 'gpio_utils',
    # Wi-Fi subsystem
    'wifi_task', 'wifi_funcs', 'wifi_ota_update', 'wifi_mqtt',
    'wifi_autoconnect', 'wifi_btc_portal', 'wifi_deauth', 'wifi_ping',
    # Bluetooth subsystem
    'bluetooth_task', 'bluetooth_nvs', 'bluetooth_utils', 'bluetooth_web_portal',
    # AI subsystem
    'ai_task', 'ai_voice', 'ai_utils', 'ai_portal', 'ai_analysis_portal',
    # LCD/UI subsystem
    'lcd_task', 'lcd_funcs', 'lcd_settings', 'lcd_hotkey', 'lcd_anim',
    'lcd_wifi', 'lcd_bluetooth', 'lcd_infrared', 'lcd_tools', 'lcd_espnow', 'lcd_lora',
    # LoRa subsystem
    'lora_task', 'lora_pcp', 'lora_radio',
    # IR subsystem
    'infrared_task', 'ir_task', 'ir_utils',
    # ESPNOW subsystem
    'espnow_task', 'espnow_utils',
    # HAL / drivers
    'sx126x_hal', 'st7789', 'srs_memory', 'esp_hid_gap',
    # Distinctive log messages
    'nvs initialized', 'polycast5_priority'
)
$appPattern = '(?i)' + ($appSearchTerms -join '|')

# Reuse cached app dump strings (avoid 30-sec re-scan).
$dumpedHits = $dumpAppStrings                    -split "`n" | Where-Object { $_ -match $appPattern }
$plainHits  = (Strings-Like build\PolyCast5.bin) -split "`n" | Where-Object { $_ -match $appPattern }

$appDumpPass  = ($dumpedHits.Count -eq 0)
$appPlainPass = ($plainHits.Count  -gt 0)
Add-CheckResult -Name "app-dump"  -Passed $appDumpPass `
    -Detail "encrypted dump matches: $($dumpedHits.Count) (expect 0)"
Add-CheckResult -Name "app-plain" -Passed $appPlainPass `
    -Detail "plain build matches:    $($plainHits.Count) (expect hundreds)"


# ===========================================================================
# STEP 4 - nvs_keys "data was written" check.
#
# Originally this step did a gzip-entropy test on the whole nvs_keys partition,
# but that test is structurally flawed:
#   - The partition is 4 KB, but only the first ~64-128 bytes hold the actual
#     XTS keys. The rest is erased flash (0xFF), which compresses to almost
#     nothing - producing a misleading "entropy is low!" false alarm even when
#     encryption is working perfectly.
#   - Even if we tested only the key region, plaintext XTS keys also look like
#     random bytes (they ARE 64 bytes of cryptographic key material). Entropy
#     can't distinguish encrypted key data from raw key data.
#
# So the realistic verification is: confirm nvs_flash_generate_keys actually
# wrote SOMETHING to the partition (proves the NVS encryption code path ran).
# The killer test (step 6) provides the definitive proof that the chain works
# end-to-end: a known plaintext doesn't leak anywhere.
# ===========================================================================
Write-Host "`n=== nvs_keys data presence ===" -ForegroundColor Cyan

$bytes = [System.IO.File]::ReadAllBytes("dump_nvs_keys.bin")

# Count bytes that aren't 0xFF (erased flash). XTS-AES-128 keys + metadata
# write ~64-128 bytes of non-erased data; XTS-AES-256 writes ~128-256 bytes.
$nonFFCount = ($bytes | Where-Object { $_ -ne 0xFF }).Count

# A working setup writes at LEAST 16 bytes of non-erased data. Anything less
# means the keys were never generated (gpio_utils_init_nvs failed silently or
# the partition is in an unexpected state).
$keysWrittenPass = ($nonFFCount -ge 16)
Add-CheckResult -Name "nvs_keys-written" -Passed $keysWrittenPass `
    -Detail "non-0xFF bytes: $nonFFCount (expect >= 16; keys were written to partition)"


# ===========================================================================
# STEP 5 - NVS partition structure encryption check.
#
# With NVS encryption ON, the NVS library encrypts each 32-byte entry (key +
# value together) using XTS-AES with keys from nvs_keys. Only page headers
# and entry-state bitmaps stay plaintext - and those are mostly binary
# metadata that doesn't form long readable strings.
#
# Random ciphertext DOES produce short ASCII runs by chance (~2800 runs of
# length >=4 in 236 KB, by the geometric distribution of printable bytes).
# So a raw count of all >=4 strings is too noisy to be discriminating. We
# filter to length >= 16: random 16-char ASCII runs are statistically near-
# zero (~0.02 expected per 236 KB), while plaintext NVS values (Wi-Fi
# passwords 8-63 chars, OAuth tokens, OpenAI keys 50+ chars) routinely
# exceed 16 chars and would show up readily.
#
# Caveat: a fresh device with only short settings stored could pass this
# check even if NVS encryption were broken. The killer test (Step 6) is the
# definitive end-to-end proof.
# ===========================================================================
Write-Host "`n=== NVS structure encryption check ===" -ForegroundColor Cyan

$nvsLongStrings   = ($dumpNvsStrings -split "`n") | Where-Object { $_.Length -ge 16 }
$nvsEncryptedPass = ($nvsLongStrings.Count -lt 5)
Add-CheckResult -Name "nvs-encrypted" -Passed $nvsEncryptedPass `
    -Detail "long (>=16 char) strings: $($nvsLongStrings.Count) (expect < 5; entries should be ciphertext)"


# ===========================================================================
# STEP 6 - Secret leak check (THE KILLER TEST).
#
# This only runs if you pass -Secret "yourPassword". It's the most rigorous
# proof of NVS encryption: you saved a known string into NVS via the device
# (Wi-Fi password, BT bond, etc.), and now we verify that string does NOT
# appear in plaintext anywhere on flash.
#
# If even ONE dump file contains the secret in plaintext, encryption is
# broken somewhere and we shout about it in red.
# ===========================================================================
if ($Secret -ne "") {
    Write-Host "`n=== Secret leak check (THE KILLER TEST) ===" -ForegroundColor Yellow
    Write-Host "  Searching for '$Secret' in all dumps (raw byte scan)..."

    # Raw byte scan via Find-RawByteOccurrences - finds the literal byte
    # sequence regardless of length or surrounding bytes. Earlier versions
    # piped through Strings-Like, but that discards ASCII runs < 4 chars,
    # so a short secret bordered by non-printables could slip through.
    $totalHits = 0
    $leakLocations = @()
    $allDumps = @(
        "dump_bootloader.bin",
        "dump_nvs_keys.bin",
        "dump_nvs.bin",
        "dump_app.bin",
        "dump_spiffs_sample.bin"
    )

    foreach ($file in $allDumps) {
        $hits = Find-RawByteOccurrences -FilePath $file -Needle $Secret
        if ($hits -gt 0) {
            $leakLocations += "$file ($hits)"
            $totalHits += $hits
        }
    }

    $secretPass = ($totalHits -eq 0)
    if ($secretPass) {
        Add-CheckResult -Name "secret-leak" -Passed $true `
            -Detail "no plaintext leaks of '$Secret' in any dump"
    }
    else {
        Add-CheckResult -Name "secret-leak" -Passed $false `
            -Detail "LEAK: '$Secret' found in $totalHits places: $($leakLocations -join ', ')"
    }
}
else {
    Write-Host "`n(Skip secret-leak test: pass -Secret 'YourPassword' to enable)" -ForegroundColor DarkGray
}


# ===========================================================================
# STEP 7 - SPIFFS plaintext sanity check.
#
# We DELIBERATELY did not encrypt SPIFFS (the assets partition holds fonts,
# icons, animations - no secrets). This step confirms we haven't accidentally
# over-encrypted: it should show LOTS of readable strings.
#
# If this returned ~0 strings, something is very wrong (SPIFFS was somehow
# encrypted, or we dumped the wrong region).
# ===========================================================================
Write-Host "`n=== SPIFFS plaintext sanity ===" -ForegroundColor Cyan

# Reuse cached SPIFFS strings.
$lines = ($dumpSpiffsStrings -split "`n") | Where-Object { $_.Length -gt 0 }

# A correctly unencrypted SPIFFS sample should produce hundreds of strings.
# Threshold of 100 is conservative - real builds typically have 500-5000.
$spiffsPass = ($lines.Count -ge 100)
Add-CheckResult -Name "spiffs-plain" -Passed $spiffsPass `
    -Detail "string count: $($lines.Count) (expect >= 100; many strings = NOT over-encrypted)"


# ===========================================================================
# Final summary + exit code.
# ===========================================================================
$totalElapsed = (Get-Date) - $scriptStart
$total  = $checkResults.Count
$passed = ($checkResults | Where-Object { $_.Passed }).Count
$failed = $total - $passed

Write-Host ""
Write-Host "==========================================================" -ForegroundColor DarkGray
if ($failed -eq 0) {
    if ($Secret -ne "") {
        Write-Host "  ENCRYPTION VERIFIED ($passed/$total checks passed)" -ForegroundColor Green
    }
    else {
        Write-Host "  ENCRYPTION CHECKS PASSED ($passed/$total)" -ForegroundColor Green
        Write-Host "  Note: -Secret not provided; for end-to-end NVS-value proof," -ForegroundColor Yellow
        Write-Host "        save a known string into NVS and re-run with -Secret 'thatstring'" -ForegroundColor Yellow
    }
    Write-Host "  Total runtime: $([int]$totalElapsed.TotalSeconds)s" -ForegroundColor DarkGray
    Write-Host "==========================================================" -ForegroundColor DarkGray
    exit 0
}
else {
    Write-Host "  ISSUES FOUND ($failed/$total checks failed)" -ForegroundColor Red
    Write-Host "  Total runtime: $([int]$totalElapsed.TotalSeconds)s" -ForegroundColor DarkGray
    Write-Host "==========================================================" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "Failed checks:" -ForegroundColor Red
    $checkResults | Where-Object { -not $_.Passed } | ForEach-Object {
        Write-Host "  - $($_.Name): $($_.Detail)" -ForegroundColor Red
    }
    exit 1
}
