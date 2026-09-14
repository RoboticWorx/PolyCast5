# VL53LX bare driver (vendored)

ST's Time-of-Flight bare driver for the VL53L4CX, plus the ESP-IDF platform layer that
adapts it to PolyCast5.

## Provenance

| | |
|---|---|
| Upstream | <https://github.com/STMicroelectronics/x-cube-tof1> |
| Path | `Drivers/BSP/Components/vl53l4cx` |
| Licence | BSD-3-Clause (`LICENSE.md`, copied verbatim) |

`core/` is ST's `modules/` directory **unmodified** — 20 `.c` files and their headers. Do
not edit anything in there; if it needs changing, change the platform layer or the wrapper
instead, and if upstream must be patched, record the patch here.

`port/` is the platform layer. Most of it is ST's `porting/` directory taken verbatim
(`vl53lx_types.h`, `vl53lx_platform_user_config.h`, `vl53lx_platform_user_defines.h`,
`vl53lx_platform_log.h`, `vl53lx_platform_ipp.[ch]`, `vl53lx_platform_ipp_imports.h`,
`vl53lx_platform.h`). Three files are ours:

- `vl53lx_platform_user_data.h` — replaces ST's, which wires the device handle to their BSP
  layer (`VL53L4CX_Object_t`, function-pointer IO, `<math.h>`). The BSP result structs are
  the only floating point in the whole driver and the ESP32-C5 has no FPU, so the BSP is
  dropped entirely and `VL53LX_Dev_t` is declared here instead.
- `vl53lx_platform.c` — the ten comms/timing functions ST's core actually calls, on
  `driver/i2c_master.h`. Read its file header before touching it: the bus-mutex hand-back
  and the tick-rounding are both load-bearing.
- `vl53lx_port.h` — the one hook our platform layer adds on top of ST's contract.

ST's `vl53l4cx.[ch]` BSP shim, `vl53lx_platform_init.h` and `vl53lx_platform_log.c` are
deliberately not vendored.

## Call sequence vs ST's reference

Audited against ST's own integration, `Drivers/BSP/Components/vl53l4cx/vl53l4cx.c` in
x-cube-tof1 (not vendored — it drags in their BSP types and the only `float` in the tree).

| ST's reference | `src/vl53l4cx.c` |
|---|---|
| `WaitDeviceBooted` → `DataInit` → `PerformRefSpadManagement` | same, but RefSpad moved (see below) |
| `SetDistanceMode` → `SetMeasurementTimingBudgetMicroSeconds` | same, same order |
| `StartMeasurement` → `ClearInterruptAndStartMeasurement` | same |
| poll `GetMeasurementDataReady` → `GetMultiRangingData` → `ClearInterruptAndStartMeasurement` | same |
| `StopMeasurement` | same |

Four deliberate departures:

1. **Two raw register writes before `WaitDeviceBooted`** (`SYSTEM__MODE_START` = 0,
   `SYSTEM__INTERRUPT_CLEAR` = 1). ST recovers a confused part with XSHUT; this module has
   XSHUT tied off, so a warm reboot can find the sensor mid-ranging with its interrupt
   latched and there is no hardware reset to fall back on.
2. **`PerformRefSpadManagement` runs inside `vl53l4cx_calibrate_xtalk()`, not at every
   init.** ST's BSP is a minimal reference that re-runs it on every boot; it is a
   *calibration*, and its result lives in `VL53LX_CustomerNvmManaged_t` inside the blob that
   is persisted and restored each session. Consequence worth knowing: **on a device that has
   never had the calibration run, RefSpad has never run** and the part uses its factory NVM
   defaults.
3. **`SetXTalkCompensationEnable` + `SmudgeCorrectionEnable(CONTINUOUS)` at init.** Not in
   ST's BSP, but `DataInit` explicitly selects `SMUDGE_CORRECTION_NONE`, and this module's
   package crosstalk resolves as a phantom target without it.
4. **Non-blocking reads.** ST's BSP offers a blocking continuous mode; `vl53l4cx_read_mm()`
   returns `ESP_ERR_NOT_FINISHED` instead so `ir_exp_task` keeps servicing the other two
   sensors. Device-side behaviour is identical.

`SetUserROI` is not called — the full ROI is what a tape measure wants.

## Calibration: two entry points, deliberately

`VL53LX_PerformXTalkCalibration` **is** referenced, via `vl53l4cx_calibrate_xtalk()`. It is
what suppresses the package crosstalk that otherwise resolves as a phantom target a
centimetre or two out — and since ST's extended-range unwrap refuses to run unless the
current and previous measurement each found exactly one target, that phantom also capped the
usable range. Cost measured at **+8,824 B (8.6 KiB)**.

`VL53LX_PerformRefSpadManagement` is also referenced, from the same routine — ST's
documented calibration order is RefSpad, then crosstalk, then offset. Cost **+912 B**.

`VL53LX_PerformOffsetCalibration` is still never referenced and `--gc-sections` drops it;
the range offset stays the simple software correction in `src/vl53l4cx.c`. Measure before
adding it.

Linked total with both calibration entry points: **52,854 B (51.6 KiB)** at `-Og`.

## Measured cost

Standalone link tests, same toolchain as the project, `--gc-sections`, built up in the order
the features were added:

| configuration | `-Og` (this project) | `-Os` |
|---|---|---|
| ranging only, no calibration | 42,678 B (41.7 KiB) | 35,348 B (34.5 KiB) |
| + xtalk compensation & smudge correction | 42,934 B (41.9 KiB) | — |
| + `PerformXTalkCalibration` | 51,942 B (50.7 KiB) | — |
| + `PerformRefSpadManagement` (**current**) | **52,854 B (51.6 KiB)** | — |

The crosstalk-compensation and smudge-correction calls are nearly free (256 B) because their
internals live in `vl53lx_api_core.c`, which is linked regardless.

For cross-reference, the ranging-only configuration measured **41,284 B / 40.3 KiB** across
224 symbols in the real firmware image (`nm` over `build/PolyCast5.elf`, all `vl53l*`
symbols) — close to the 41.9 KiB standalone figure. Re-measure that way after a build rather
than trusting the `.map`: the map's cross-reference table lists defined-but-discarded symbols
and reads as though calibration were linked when it is not.

RAM is one `VL53LX_Dev_t` (9,432 B) and one `VL53LX_MultiRangingData_t` (92 B), both placed
in PSRAM by `src/vl53l4cx.c`, so the internal-SRAM cost is essentially zero. The linked
image contains no soft-float helpers.
