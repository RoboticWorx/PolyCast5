# ESP-IDF eFuse rev 0.3 patch

All ESP32-C5-WROOM-1-N16R8 MCUs with an eFuse block revision of v0.3 (chip rev v1.2) will **corrupt PSRAM** at 240 MHz when PSRAM is encrypted resulting in random crashes.

The two workarounds are:

* This patch which prevents PSRAM from being encrypted.
* Capping the clock speed at 160MHz (typical max is 240MHz).

These workarounds are not required for ESP32-C5s with an eFuse rev of 0.4+. This patch does the first workaround (preventing PSRAM encryption), to prevent random crashes while keeping the device as snappy as possible.

*Flash/NVS stay encrypted in development mode.*

## esp32c5-psram-plaintext-mmu_ll.patch

Target: ESP-IDF v6.0.1, `components/hal/esp32c5/include/hal/mmu_ll.h`, `mmu_ll_write_entry()`.

### What it changes

With flash encryption enabled, stock IDF sets `SOC_MMU_SENSITIVE` on every MMU page it maps, which
routes that page through the MSPI XTS-AES engine. For PSRAM it makes one exception: chip revision
v1.0 and older, because of a hardware bug there. The patch makes that exception unconditional:

- PSRAM pages (`MMU_TARGET_PSRAM0`) never get the sensitive bit, on any chip revision.
- The anti-fault-injection assert that follows the write is inverted for PSRAM entries, so it now
  confirms the bit is clear. Without this half the chip resets while mapping PSRAM at boot.
- Flash pages are untouched. Flash, NVS and the firmware image at rest stay encrypted.

The effect is exactly the state IDF already ships on v1.0 silicon: encrypted flash, plaintext PSRAM.

### Security consequence

PSRAM contents are plaintext on the die. That includes everything the allocator puts there, which
with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` is nearly every heap allocation: mbedTLS buffers
(`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`), NimBLE (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`),
Wi-Fi and LWIP buffers, LVGL. Reading them requires probing the PSRAM bus on a powered, running
unit. Contents at rest are unaffected.

**This means if you store sensitive data on here you need to lock down your device ([tutorial](https://polycast5.com/blogs/docs/lock-it-down)) to prevent extraction.** This enables release mode encryption and secure boot, while capping the device at **160MHz** to secure PSRAM contents.

### Apply

```
git -C C:\Espressif\.espressif\v6.0.1\esp-idf apply C:\Projects\ESP\PolyCast5\components\common\patch\esp32c5-psram-plaintext-mmu_ll.patch
idf.py fullclean
idf.py build
```

The full clean is required: the header is inlined into every MMU mapping site, and an incremental
build does not rebuild them all.

*On an different ESP-IDF version, the line numbers and possibly the surrounding code may change. If this is the case, **re-derive the change** from the v1.0 branch in `mmu_ll_write_entry()` rather than forcing this patch.*

### Check that it is applied

Succeeds only when the patch is present:

```
git -C C:\Espressif\.espressif\v6.0.1\esp-idf apply --check -R C:\Projects\ESP\PolyCast5\components\common\patch\esp32c5-psram-plaintext-mmu_ll.patch
```

### Revert

```
git -C C:\Espressif\.espressif\v6.0.1\esp-idf checkout -- components/hal/esp32c5/include/hal/mmu_ll.h
```


### Evidence of bug

- A bare probe in `app_main` before any driver init, CPU pinned at 240 MHz, 512 KB of PSRAM,
  memset 0xFE, one byte written per 32-byte line, then verified, fails within 3 to 5 s on every
  run. Every failure is one wrong byte at offset 1 of a 32-byte cache line, with a value unrelated
  to the pattern. It only fails in the phase that partially writes lines. At 160 MHz it is clean.
- The application crash that started the investigation was the same signature: comprehensive
  heap poisoning reported byte 0x424095e1 (offset 1 of its line) changed from 0xfe to 0x77 in free
  memory, and the earlier TLSF `block_locate_free` asserts were free-block headers with the same
  kind of damage.
- With this patch applied, the same unit runs the full application at 240 MHz.

Espressif's own errata describe the engine involved: CPU-718 "PSRAM read-after-write
consistency" with encryption enabled, listed as fixed in v1.2, and FLASH-938 "flash manual
encryption may fail when the CPU runs at 240 MHz due to internal power consumption fluctuations",
which applies to v1.0 and v1.2 with no fix scheduled. A single wrong plaintext byte fits a fault
on the plaintext side of that engine; a fault on the PSRAM bus itself would garble a whole 16-byte
block after decryption.


### Alternatives

- Cap the DFS ceiling at 160 MHz on affected units (`POLYCAST5_CPU_240_MIN_EFUSE_BLK_REV` in
  `polycast5_macros.h`). Full encryption, no IDF patch, slower UI. The fallback if plaintext PSRAM
  is unacceptable.
- Make the patch lot-conditional: replace `target != MMU_TARGET_PSRAM0` with
  `!(target == MMU_TARGET_PSRAM0 && efuse_hal_blk_version() < 4)` and mirror the condition in
  the assert. v0.4 units stay encrypted at 240 MHz as they have run for weeks.
- Disabling flash encryption entirely also stops the corruption but gives up far more; it is
  unnecessary.
