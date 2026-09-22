/*
 * Boot-time hardware verification.
 *
 * Probes every external IC with an observable interface and logs one
 * PASS/WARN/FAIL/SKIP line per device plus a summary banner, so assembly or
 * connection faults show up in the serial log on the first boot after
 * flashing.
 *
 * Runs in two phases, because some probes must happen before the LCD and
 * SX1262 are initialized (they toggle the LCD_RST and SX_NRST lines through
 * the expander, which would leave an already-initialized panel blank):
 *
 *   verify_hardware_run_early()  Right after gpio_utils_init(), before
 *                                lcd_init_driver(). Expander/IO and identity.
 *   verify_hardware_run()        After all buses/HALs are up but BEFORE the
 *                                tasks are created. Everything else, then
 *                                prints the combined report for both phases.
 *
 * Both phases assume they are the only bus users and that no task has claimed
 * the peripherals yet. Results are buffered across the two phases and printed
 * once at the end of the late phase, so a halt in between loses the report.
 *
 * Checks that are intrusive (drive expander nets, spin the haptic, transmit)
 * or slow are gated behind POLYCAST5_FACTORY_TEST, which is off by default.
 *
 * Devices with no feedback path (RGB LEDs, backlight FET, LCD image) cannot be
 * probed at all; they are covered by the end-of-line operator step instead.
 */

#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_app_desc.h"
#include "esp_cpu.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "hal/efuse_hal.h"
#include "hal/mmu_ll.h"

#include "polycast5_gpios.h"
#include "polycast5_macros.h"

#include "TCA9535.h"
#include "gpio_task.h"
#include "gpio_utils.h"
#include "lis2dh12.h"
#include "mmc5603.h"
#include "ai_voice.h"
#include "sx126x.h"
#include "sx126x_regs.h"
#include "sx126x_hal.h"
#include "ir_exp.h"

#include "verify_hardware.h"

static const char *TAG = "VERIFY_HW";

// Bump when the VHR| line format changes so fixture scripts can reject old firmware
#define VH_SCHEMA_VER 3

// Room for every check in both phases; overflowing this drops lines from the summary
#define VH_MAX_ENTRIES 48

// Sentinel for the machine-readable <value> field when a check has no number to report
#define VH_NO_VALUE LONG_MIN

// The BOOT0 net, module pin 15 / IO28. Unused by this firmware, so it is free to
// read; strapping is latched at reset, so holding it low at runtime is inert
#define VH_BOOT0_GPIO 28

// Restore writes are retried the way gpio_utils_init retries its 3V3_EN preload
#define VH_RESTORE_RETRIES 5

// The three devices always on the board. The IR expansion module adds three more
// (see vh_addr_is_expansion); anything else answering is a fault
#define VH_I2C_ADDR_ACCEL 0x19 // LIS2DH12
#define VH_I2C_ADDR_TCA   0x20 // TCA9535
#define VH_I2C_ADDR_MAG   0x30 // MMC5603
#define VH_I2C_PROBE_TIMEOUT_MS 50
// A NACK returns immediately, but a stuck bus burns the full timeout on every
// address; bail out early rather than stalling boot for seconds
#define VH_I2C_STUCK_TIMEOUTS 5
#define VH_I2C_MAX_REPORTED 8 // 3 on-board + 3 expansion, with room to name a stranger
// Ceiling on the whole address sweep, so no fault pattern can stretch boot
#define VH_I2C_SWEEP_BUDGET_MS 400

// TCA_INT carries C39 100nF at the MCU against R5 100k: a 10 ms RC, so a released
// open-drain INT needs ~14 ms to cross VIH. Allow generous margin over that
#define VH_INT_RISE_CAP_MS 60

// Busy-wait window for the measured CPU clock. Long enough to average out timer
// granularity, short enough not to matter at boot
#define VH_CPU_MEASURE_US 10000

// Expected PSRAM size
#define VH_PSRAM_MIN_BYTES (8 * 1024 * 1024)

// SX1262 LoRa sync word reset default 0x1424 (see SX126X_REG_LR_SYNCWORD)
#define VH_SX1262_SYNC_MSB 0x14
#define VH_SX1262_SYNC_LSB 0x24

typedef enum {
    VH_PASS,
    VH_WARN,
    VH_FAIL,
    // A check that could not add information because a prerequisite already
    // failed, or because a precondition was not met. Deliberately NOT counted in
    // the pass/warn/fail tallies: one physical defect should produce one failure,
    // not a cascade that makes a single fault look like several
    VH_SKIP,
    VH_VERDICT_COUNT,
} vh_verdict_t;

// Entries print grouped so a long report stays readable
typedef enum {
    VH_G_IDENTITY,
    VH_G_MEMORY,
    VH_G_STORAGE,
    VH_G_I2C,
    VH_G_IO,
    VH_G_SPI,
    VH_G_RF,
    VH_G_SENSOR,
    VH_G_AUDIO,
    VH_G_IR,
    VH_G_POWER,
    VH_GROUP_COUNT,
} vh_group_t;

static const char *const vh_group_names[VH_GROUP_COUNT] = {
    "IDENTITY", "MEMORY", "STORAGE", "I2C", "IO",
    "SPI", "RF", "SENSOR", "AUDIO", "IR", "POWER",
};

static const char *const vh_verdict_names[VH_VERDICT_COUNT] = { "PASS", "WARN", "FAIL", "SKIP" };

// Set by vh_check_i2c_census so the per-device checks can tell "this part did not
// answer at all" (already reported once, by the census) from "it answered but is
// not the part we expected", which is a separate defect worth its own failure
static bool s_census_ran = false;
static bool s_census_saw_accel = false;
static bool s_census_saw_mag = false;
static bool s_census_bus_stuck = false;
// Published so a downstream check can skip rather than restate an upstream fault
static bool s_port1_pads_ok = true;
// s_port0_nets_ok lives with the factory checks: both its writer and its reader are
// inside POLYCAST5_FACTORY_TEST, so declaring it here leaves an unused static in
// shipping builds

typedef struct {
    const char *id;   // Stable token the fixture greps on; never reworded
    const char *name; // Human label; safe to reword
    uint8_t group;
    uint8_t verdict;
    uint16_t duration_ms;
    long value;       // Bare number for the machine line, or VH_NO_VALUE
    char detail[112];
} vh_entry_t;

// Verdicts are buffered so they print as one block at the end instead of scattering through boot logging
// The buffer spans both phases and is freed at the end of the late phase
static vh_entry_t *s_entries = NULL;
static int s_entry_count = 0;
static int s_counts[VH_VERDICT_COUNT];
static bool s_begun = false;
static int64_t s_last_mark_us = 0;
// Only time spent inside the phases counts. Wall-clock from the early phase to
// the late one would also bill the caller for lcd_init_driver, the LittleFS
// mount and LVGL init, which happen in between and are not part of the test
static int64_t s_busy_us = 0;
static int64_t s_phase_start_us = 0;

// The machine-readable lines are field-delimited, so a stray pipe would corrupt the record
static void vh_sanitize(char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '|' || *s == '\r' || *s == '\n') {
            *s = ' ';
        }
    }
}

static void vh_log_one(const vh_entry_t *e)
{
    switch (e->verdict) {
    case VH_PASS:
        ESP_LOGI(TAG, "PASS  %s: %s", e->name, e->detail);
        break;
    case VH_WARN:
        ESP_LOGW(TAG, "WARN  %s: %s", e->name, e->detail);
        break;
    case VH_SKIP:
        ESP_LOGI(TAG, "SKIP  %s: %s", e->name, e->detail);
        break;
    case VH_FAIL:
    default: // Anything unexpected is surfaced as a failure rather than dropped
        ESP_LOGE(TAG, "FAIL  %s: %s", e->name, e->detail);
        break;
    }
}

// Fixed-field line for the production fixture. printf, not ESP_LOG, so the block
// survives log-level filtering and carries no tag or colour escape codes
static void vh_emit_machine(int seq, const vh_entry_t *e)
{
    const char *verdict = (e->verdict < VH_VERDICT_COUNT) ? vh_verdict_names[e->verdict] : "FAIL";

    // Schema 3.  BEGIN:  schema|mac|elf_sha|version
    //            record: seq|id|verdict|value|ms|detail  (PASS, WARN, FAIL or SKIP)
    //            END:    fail|warn|pass|skip|ms|overall
    // Per-check ms so a stall (timing-out mic probe, slow census) shows up in the record
    if (e->value == VH_NO_VALUE) {
        printf("VHR|%d|%s|%s||%u|%s\n", seq, e->id, verdict, (unsigned)e->duration_ms, e->detail);
    } else {
        printf("VHR|%d|%s|%s|%ld|%u|%s\n",
                seq, e->id, verdict, e->value, (unsigned)e->duration_ms, e->detail);
    }
}

static void vh_emit_begin(void)
{
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // First 8 bytes of the ELF SHA-256 are plenty to pin the exact build
    char sha[17] = { 0 };
    const char *version = "?";
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL) {
        for (int i = 0; i < 8; i++) {
            snprintf(&sha[i * 2], 3, "%02x", desc->app_elf_sha256[i]);
        }
        version = desc->version;
    }

    printf("VHR|BEGIN|%d|%02X%02X%02X%02X%02X%02X|%s|%s\n",
            VH_SCHEMA_VER, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], sha, version);
}

static void vh_record(const char *id, vh_group_t group, const char *name,
        vh_verdict_t verdict, long value, const char *fmt, ...)
{
    s_counts[verdict]++; // Tallied here so the summary is right on either path below

    // Elapsed since the previous record, so a slow check is visible in the log
    int64_t now = esp_timer_get_time();
    int64_t elapsed_ms = (now - s_last_mark_us) / 1000;
    s_last_mark_us = now;
    if (elapsed_ms < 0) {
        elapsed_ms = 0;
    } else if (elapsed_ms > UINT16_MAX) {
        elapsed_ms = UINT16_MAX;
    }

    va_list args;
    va_start(args, fmt);
    if (s_entries != NULL && s_entry_count < VH_MAX_ENTRIES) {
        // Format straight into the entry; printed with the rest at the end
        vh_entry_t *e = &s_entries[s_entry_count++];
        e->id = id;
        e->name = name;
        e->group = (uint8_t)group;
        e->verdict = (uint8_t)verdict;
        e->duration_ms = (uint16_t)elapsed_ms;
        e->value = value;
        vsnprintf(e->detail, sizeof(e->detail), fmt, args);
        vh_sanitize(e->detail);
    } else {
        // No buffer, or the buffer is full: print now rather than lose the verdict
        vh_entry_t tmp = {
            .id = id,
            .name = name,
            .group = (uint8_t)group,
            .verdict = (uint8_t)verdict,
            .duration_ms = (uint16_t)elapsed_ms,
            .value = value,
        };
        vsnprintf(tmp.detail, sizeof(tmp.detail), fmt, args);
        vh_sanitize(tmp.detail);
        vh_log_one(&tmp);
        vh_emit_machine(-1, &tmp);
    }
    va_end(args);
}

// Safe to call from either phase; the late phase reuses whatever the early phase set up
static void vh_begin_if_needed(void)
{
    if (s_begun) {
        return;
    }
    s_begun = true;

    s_entry_count = 0;
    for (int i = 0; i < VH_VERDICT_COUNT; i++) {
        s_counts[i] = 0;
    }

    // Scratch buffer for the batched summary, released at the end of the late phase
    // May land in PSRAM, which is fine: reaching app_main at all proves PSRAM came up
    s_entries = malloc(sizeof(vh_entry_t) * VH_MAX_ENTRIES);
    if (s_entries == NULL) {
        ESP_LOGW(TAG, "verdict buffer unavailable; logging each result inline");
    }

    vh_emit_begin();
}

static void vh_phase_enter(void)
{
    s_phase_start_us = esp_timer_get_time();
    // Reset the mark too, so the first entry of a phase is not billed for the
    // unrelated init work that ran since the previous phase
    s_last_mark_us = s_phase_start_us;
}

static void vh_phase_exit(void)
{
    s_busy_us += esp_timer_get_time() - s_phase_start_us;
}

#ifdef POLYCAST5_FACTORY_TEST
// Put the expander back the way it was found, and prove it took
// Every intrusive port test must call this on an unconditional exit path: a port-0 pin left driven
// low reads as a held button, the expander survives an MCU reset, and gpio_task acts on a held
// power button within 20 ms. Caller holds xI2CBusMutex
static esp_err_t vh_tca_restore(uint8_t cfg0, uint8_t out0, uint8_t out1)
{
    esp_err_t last = ESP_FAIL;

    for (int attempt = 0; attempt < VH_RESTORE_RETRIES; attempt++) {
        if (attempt > 0) {
            esp_rom_delay_us(2000); // 2 ms settle, as gpio_utils_init does for its 3V3_EN preload
        }

        // Direction first: releasing the drivers before rewriting the latches
        // means a stale output bit can never reach a pin
        last = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, cfg0);
        if (last != ESP_OK) {
            continue;
        }
        last = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG0, out0);
        if (last != ESP_OK) {
            continue;
        }
        last = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG1, out1);
        if (last != ESP_OK) {
            continue;
        }

        uint8_t rb_cfg0 = 0, rb_out0 = 0, rb_out1 = 0;
        if (TCA9535ReadSingleRegister(TCA9535_CONFIG_REG0, &rb_cfg0) != ESP_OK ||
            TCA9535ReadSingleRegister(TCA9535_OUTPUT_REG0, &rb_out0) != ESP_OK ||
            TCA9535ReadSingleRegister(TCA9535_OUTPUT_REG1, &rb_out1) != ESP_OK) {
            last = ESP_FAIL;
            continue;
        }

        if (rb_cfg0 == cfg0 && rb_out0 == out0 && rb_out1 == out1) {
            return ESP_OK;
        }
        last = ESP_ERR_INVALID_STATE;
    }

    // Loud and unconditional: this is the one failure that can leave a unit unusable
    ESP_LOGE(TAG, "EXPANDER RESTORE FAILED (%s) - buttons and power latch may be stuck; "
                  "power-cycle the unit before use", esp_err_to_name(last));
    return last;
}
#endif // POLYCAST5_FACTORY_TEST

static void vh_report_and_free(void);

// Sample the TSOP output for up to ~20 ms and report whether it was ever high.
// Sampling over a window rather than once keeps a stray 38 kHz source in the
// room (which pulls the line low in bursts) from reading as a dead receiver
static bool tsop_reads_high(void)
{
    for (int i = 0; i < 10; i++) {
        if (gpio_get_level(RMT_RX_GPIO_PIN) == 1) {
            return true;
        }
        esp_rom_delay_us(2000);
    }
    return false;
}

// Sweep the whole 7-bit address space and compare against the expected set
// Turns "expander unreachable" into "expander answered at 0x21, check the A0 strap"
// A NACK returns immediately but a stuck bus burns the full timeout on every address, so bail out
// The IR expansion module adds three devices with no power gate: expected if present, not intruders
static bool vh_addr_is_expansion(uint8_t addr)
{
    return addr == VL53L4CX_I2C_ADDR || addr == MLX90632_I2C_ADDR || addr == MLX90642_I2C_ADDR;
}

static void vh_check_i2c_census(void)
{
    uint8_t found[VH_I2C_MAX_REPORTED] = { 0 };
    int n_found = 0;
    int consecutive_timeouts = 0;
    bool bus_stuck = false;
    // Presence is tracked by flag, not by position in found[]: that array is capped
    // at VH_I2C_MAX_REPORTED, so a busier bus would silently drop entries and a
    // position-based comparison would then mis-verdict
    bool have_accel = false, have_tca = false, have_mag = false;
    int n_expansion = 0, n_unexpected = 0;

    int64_t sweep_start_us = esp_timer_get_time();

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    for (uint16_t addr = 0x03; addr < 0x78; addr++) {
        // Hard ceiling on the whole sweep, not just consecutive timeouts: a marginal bus can
        // alternate timeout/NACK forever without ever hitting five in a row, and 58 timeouts at
        // 50 ms each would add ~3 s to boot
        if ((esp_timer_get_time() - sweep_start_us) > (VH_I2C_SWEEP_BUDGET_MS * 1000)) {
            bus_stuck = true;
            break;
        }

        esp_err_t e = i2c_master_probe(i2c_bus_handle, addr, VH_I2C_PROBE_TIMEOUT_MS);
        if (e == ESP_OK) {
            if (n_found < VH_I2C_MAX_REPORTED) {
                found[n_found] = (uint8_t)addr;
            }
            n_found++;
            consecutive_timeouts = 0;

            switch (addr) {
            case VH_I2C_ADDR_ACCEL: have_accel = true; break;
            case VH_I2C_ADDR_TCA:   have_tca = true;   break;
            case VH_I2C_ADDR_MAG:   have_mag = true;   break;
            default:
                if (vh_addr_is_expansion((uint8_t)addr)) {
                    n_expansion++;
                } else {
                    n_unexpected++;
                }
                break;
            }
        } else if (e == ESP_ERR_TIMEOUT) {
            if (++consecutive_timeouts >= VH_I2C_STUCK_TIMEOUTS) {
                bus_stuck = true;
                break;
            }
        } else {
            consecutive_timeouts = 0; // A clean NACK proves the bus is still alive
        }
    }
    xSemaphoreGive(xI2CBusMutex);

    // Publish before the early return below, so a stuck bus also suppresses the
    // per-device checks instead of having them repeat the same one fault
    s_census_ran = true;
    s_census_saw_accel = have_accel;
    s_census_saw_mag = have_mag;
    s_census_bus_stuck = bus_stuck;

    if (bus_stuck) {
        vh_record("i2c-census", VH_G_I2C, "I2C bus census", VH_FAIL, VH_NO_VALUE,
                "bus unusable, gave up after %d ms - check R6/R7 and for a shorted device",
                (int)((esp_timer_get_time() - sweep_start_us) / 1000));
        return;
    }

    // Build the found-address list for the detail string
    char list[64];
    int off = 0;
    int shown = (n_found < VH_I2C_MAX_REPORTED) ? n_found : VH_I2C_MAX_REPORTED;
    for (int i = 0; i < shown && off < (int)sizeof(list) - 6; i++) {
        off += snprintf(&list[off], sizeof(list) - off, "%s0x%02X", (i > 0) ? " " : "", found[i]);
    }
    if (n_found > shown) {
        snprintf(&list[off], sizeof(list) - off, " +%d", n_found - shown);
    } else if (n_found == 0) {
        snprintf(list, sizeof(list), "none");
    }

    // A missing on-board device, or a stranger on the bus, is a fault. A recognised
    // expansion address is not - it just means the accessory is plugged in
    if (!have_accel || !have_tca || !have_mag) {
        // Name the part, not just the address: whoever reads this log in production
        // should not have to know the address map to act on it
        vh_record("i2c-census", VH_G_I2C, "I2C bus census", VH_FAIL, n_found,
                "no answer from%s%s%s - found %d: %s",
                have_accel ? "" : " LIS2DH12 accel 0x19",
                have_tca ? "" : " TCA9535 expander 0x20",
                have_mag ? "" : " MMC5603 mag 0x30",
                n_found, list);
    } else if (n_unexpected > 0) {
        vh_record("i2c-census", VH_G_I2C, "I2C bus census", VH_FAIL, n_found,
                "%d unexpected device(s) answering - found %d: %s", n_unexpected, n_found, list);
    } else if (n_expansion > 0) {
        vh_record("i2c-census", VH_G_I2C, "I2C bus census", VH_PASS, n_found,
                "3 on-board + %d IR-expansion device(s): %s", n_expansion, list);
    } else {
        vh_record("i2c-census", VH_G_I2C, "I2C bus census", VH_PASS, n_found,
                "exactly the expected 3 devices: %s", list);
    }
}

// Every register gpio_utils_init set, plus the two polarity registers, which
// must be at their POR default. CONFIG_REG1 == 0x00 is load-bearing for every
// port-1 check below: if it read 0xFF the pins would be inputs and a pad
// readback would be measuring pull-ups rather than the expander's drivers
static void vh_check_tca_regmap(void)
{
    uint8_t cfg0 = 0, cfg1 = 0, pol0 = 0, pol1 = 0;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_CONFIG_REG0, &cfg0);
    if (err == ESP_OK) {
        err = TCA9535ReadSingleRegister(TCA9535_CONFIG_REG1, &cfg1);
    }
    if (err == ESP_OK) {
        err = TCA9535ReadSingleRegister(TCA9535_POLARITY_REG0, &pol0);
    }
    if (err == ESP_OK) {
        err = TCA9535ReadSingleRegister(TCA9535_POLARITY_REG1, &pol1);
    }
    xSemaphoreGive(xI2CBusMutex);

    if (err != ESP_OK) {
        vh_record("tca9535-config", VH_G_I2C, "TCA9535 register map (I2C 0x20)", VH_FAIL,
                VH_NO_VALUE, "unreachable: %s", esp_err_to_name(err));
    } else if (cfg0 != 0xFF || cfg1 != 0x00 || pol0 != 0x00 || pol1 != 0x00) {
        vh_record("tca9535-config", VH_G_I2C, "TCA9535 register map (I2C 0x20)", VH_FAIL, VH_NO_VALUE,
                "cfg0=0x%02X cfg1=0x%02X pol0=0x%02X pol1=0x%02X, expected FF 00 00 00",
                cfg0, cfg1, pol0, pol1);
    } else {
        vh_record("tca9535-config", VH_G_I2C, "TCA9535 register map (I2C 0x20)", VH_PASS,
                VH_NO_VALUE, "cfg0/cfg1/pol0/pol1 all as written");
    }
}

// Walk data patterns through POLARITY_REG1 to exercise all eight I2C data lanes
// Inert because polarity only affects pins configured as INPUTS, and port 1 is all outputs
// NEVER on POLARITY_REG0: port 0 is inputs, and a stray bit inverts button logic firmware-wide
// Cannot prove R6/R7 - the bus enables the MCU's internal pull-ups, enough alone at 100 kHz
static void vh_check_tca_regwalk(void)
{
    static const uint8_t patterns[] = { 0x00, 0xFF, 0x55, 0xAA, 0x00 };
    esp_err_t err = ESP_OK;
    uint8_t bad_pattern = 0, bad_readback = 0;
    bool mismatch = false;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(patterns) && err == ESP_OK && !mismatch; i++) {
        err = TCA9535WriteSingleRegister(TCA9535_POLARITY_REG1, patterns[i]);
        if (err != ESP_OK) {
            break;
        }
        uint8_t rb = 0;
        err = TCA9535ReadSingleRegister(TCA9535_POLARITY_REG1, &rb);
        if (err == ESP_OK && rb != patterns[i]) {
            mismatch = true;
            bad_pattern = patterns[i];
            bad_readback = rb;
        }
    }
    // Always land back on the POR default, whatever happened above, and prove it.
    // The expander is POR-only, so a polarity byte left non-zero survives every MCU
    // reset and makes vh_check_tca_regmap FAIL on every subsequent boot until the
    // battery and USB are both removed
    esp_err_t restore = ESP_FAIL;
    for (int attempt = 0; attempt < VH_RESTORE_RETRIES; attempt++) {
        if (attempt > 0) {
            esp_rom_delay_us(2000);
        }
        restore = TCA9535WriteSingleRegister(TCA9535_POLARITY_REG1, 0x00);
        if (restore != ESP_OK) {
            continue;
        }
        uint8_t rb = 0xFF;
        restore = TCA9535ReadSingleRegister(TCA9535_POLARITY_REG1, &rb);
        if (restore == ESP_OK && rb != 0x00) {
            restore = ESP_ERR_INVALID_STATE;
        }
        if (restore == ESP_OK) {
            break;
        }
    }
    xSemaphoreGive(xI2CBusMutex);

    // Restore first: a failed restore is stickier than whatever the walk found, and
    // testing err first would mask it on exactly the runs where it matters
    if (restore != ESP_OK) {
        vh_record("i2c-regwalk", VH_G_I2C, "TCA9535 I2C data integrity", VH_FAIL, VH_NO_VALUE,
                "POLARITY_REG1 left non-zero (%s) - power-cycle (battery + USB) before retest",
                esp_err_to_name(restore));
    } else if (err != ESP_OK) {
        vh_record("i2c-regwalk", VH_G_I2C, "TCA9535 I2C data integrity", VH_FAIL,
                VH_NO_VALUE, "transfer failed: %s", esp_err_to_name(err));
    } else if (mismatch) {
        vh_record("i2c-regwalk", VH_G_I2C, "TCA9535 I2C data integrity", VH_FAIL, VH_NO_VALUE,
                "wrote 0x%02X read 0x%02X (bad bits 0x%02X)",
                bad_pattern, bad_readback, (uint8_t)(bad_pattern ^ bad_readback));
    } else {
        vh_record("i2c-regwalk", VH_G_I2C, "TCA9535 I2C data integrity", VH_PASS,
                VH_NO_VALUE, "5 patterns, all 8 data lanes OK");
    }
}

// Compare what the expander is driving against what its pins actually sit at
// The Input Port registers read the real pin level even on output pins, while the Output Port
// registers only report the latch, so requiring both to agree catches a shorted net or dead driver
// Senses the expander pin only: a short past the 221R gate resistors is invisible
static void vh_check_port1_pads(void)
{
    uint8_t out1 = 0, in1 = 0;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_OUTPUT_REG1, &out1);
    if (err == ESP_OK) {
        err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG1, &in1);
    }
    xSemaphoreGive(xI2CBusMutex);

    // 3V3_EN, SX_NRST and LCD_NRST must be high no matter what the latch says:
    // the board is powered and neither reset line should be asserted
    const uint8_t must_be_high = (1 << TCA9535_3V3_EN_PIN) |
                                 (1 << TCA9535_SX126X_NRST_PIN) |
                                 (1 << TCA9535_LCD_NRST_PIN);

    // Published so the SX1262 check can skip rather than restate a stuck SX_NRST
    s_port1_pads_ok = (err == ESP_OK) && (in1 == out1) && ((in1 & must_be_high) == must_be_high);

    if (err != ESP_OK) {
        vh_record("port1-pads", VH_G_IO, "Expander port-1 pads", VH_FAIL,
                VH_NO_VALUE, "read failed: %s", esp_err_to_name(err));
    } else if (in1 != out1) {
        vh_record("port1-pads", VH_G_IO, "Expander port-1 pads", VH_FAIL, in1,
                "latch 0x%02X but pads 0x%02X (differing pins 0x%02X - shorted net?)",
                out1, in1, (uint8_t)(in1 ^ out1));
    } else if (out1 != TCA9535_PORT1_REST) {
        // Pads matching the latch is not enough: a latch stuck at its 0xFF POR default has the
        // pads follow it faithfully, and must_be_high is satisfied by 0xFF too, so the check above
        // would pass with the haptic and all three LEDs driven on
        vh_record("port1-pads", VH_G_IO, "Expander port-1 pads", VH_FAIL, out1,
                "latch 0x%02X, expected 0x%02X (wrong bits 0x%02X) - init did not take",
                out1, (unsigned)TCA9535_PORT1_REST, (uint8_t)(out1 ^ TCA9535_PORT1_REST));
    } else {
        // No must_be_high arm: in1 == out1 == the rest value here, and REST & must_be_high ==
        // must_be_high by construction, so it could never fail. The mask still feeds
        // s_port1_pads_ok, which the SX1262 check reads to explain a stuck-low NRST
        vh_record("port1-pads", VH_G_IO, "Expander port-1 pads", VH_PASS, in1,
                "pads match latch 0x%02X, 3V3_EN and both resets high", in1);
    }
}

// Every button must read released. A bit stuck low is a shorted switch, a solder
// bridge to GND, or a switch mechanically held. CHG is on the same port but is
// driven by the charger, so report it rather than judge it
static void vh_check_port0_idle(void)
{
    uint8_t in0 = 0;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &in0);
    xSemaphoreGive(xI2CBusMutex);

    const uint8_t buttons = 0x7F; // P00..P06; P07 is CHG
    const char *charging = ((in0 >> TCA9535_CHG_IND_PIN) & 0x1) ? "not charging" : "charging";

    if (err != ESP_OK) {
        vh_record("port0-idle", VH_G_IO, "Expander port-0 idle state", VH_FAIL,
                VH_NO_VALUE, "read failed: %s", esp_err_to_name(err));
    } else if ((in0 & buttons) != buttons) {
        // Hard FAIL, not WARN: nothing in this firmware reads a button at boot, so a low bit is a
        // defect - and the log is the production pass/fail signal
        vh_record("port0-idle", VH_G_IO, "Expander port-0 idle state", VH_FAIL, in0,
                "0x%02X: button bits 0x%02X read low - stuck switch, short to GND, or held",
                in0, (uint8_t)(~in0 & buttons));
    } else {
        vh_record("port0-idle", VH_G_IO, "Expander port-0 idle state", VH_PASS, in0,
                "all 7 buttons released, CHG %s", charging);
    }
}

// The TCA9535 INT line is open-drain with R5 pulling it to 3V3
// Read the input port to clear any pending interrupt, then sample GPIO1 with both internal pulls
// disabled so R5 is the only path high
// Proves the net is not shorted to GND, NOT that R5 is fitted - a floating pad reads high too
static void vh_check_tca_int_line(void)
{
    uint8_t scratch = 0;
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &scratch);
    xSemaphoreGive(xI2CBusMutex);
    (void)scratch;

    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << TCA9535_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    // Poll, and allow far longer than seems necessary: C39 100nF against R5 100k is a 10 ms RC, so
    // a released INT needs ~14 ms to cross VIH. INT may well be asserted here - the expander
    // latches any input change since the last INPUT-port read, and gpio_utils_init reads none
    int rise_ms = -1;
    for (int i = 0; i <= VH_INT_RISE_CAP_MS / 5; i++) {
        if (gpio_get_level(TCA9535_INT_PIN) == 1) {
            rise_ms = i * 5;
            break;
        }
        esp_rom_delay_us(5000);
    }

    // Second chance before calling it a short: INT latches on ANY port-0 change, so a button
    // released or CHG toggling during the poll above re-asserts it. Clear once more and re-poll -
    // a real short to GND still never rises
    if (rise_ms < 0) {
        xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
        esp_err_t retry_err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &scratch);
        xSemaphoreGive(xI2CBusMutex);

        if (retry_err == ESP_OK) {
            for (int i = 0; i <= VH_INT_RISE_CAP_MS / 5; i++) {
                if (gpio_get_level(TCA9535_INT_PIN) == 1) {
                    rise_ms = VH_INT_RISE_CAP_MS + (i * 5); // Flag that it took a retry
                    break;
                }
                esp_rom_delay_us(5000);
            }
        }
    }

    if (err != ESP_OK) {
        vh_record("tca-int-line", VH_G_IO, "TCA9535 INT line (GPIO1 + R5)", VH_FAIL,
                VH_NO_VALUE, "could not clear pending INT: %s", esp_err_to_name(err));
    } else if (rise_ms < 0) {
        vh_record("tca-int-line", VH_G_IO, "TCA9535 INT line (GPIO1 + R5)", VH_FAIL, VH_NO_VALUE,
                "still low after two %d ms polls, each preceded by an input-port read "
                "(INT shorted to GND)", VH_INT_RISE_CAP_MS);
    } else {
        vh_record("tca-int-line", VH_G_IO, "TCA9535 INT line (GPIO1 + R5)", VH_PASS,
                rise_ms, "high after %d ms; not shorted to GND (R5 vs a floating pad "
                "is indistinguishable here)", rise_ms);
    }
}

#ifdef POLYCAST5_FACTORY_TEST
// Set by the port-0 float test, read by the BOOT0 check so it can skip rather than
// restate a fault the float test already named. Declared here, not with the other
// published flags, because both ends live inside this block
static bool s_port0_nets_ok = true;

// Drive distinct patterns onto port 1 to find pin-to-pin solder bridges: five patterns give all
// eight pins a distinct signature while P12 stays high (a walking-ones sweep would force it low
// and power the board off). A bridged pair reads one level while commanded to two
// Early phase only: two patterns pulse SX_NRST and LCD_NRST, before those chips are initialized
static void vh_check_port1_bridges(void)
{
    // Every pattern and the restore target derive from this, so it is the constant,
    // never a readback. A corrupted read would otherwise be driven onto the pins and
    // then confirmed by the pad compare below, which cannot tell a faithfully-driven
    // wrong value from a right one - and a cleared SX_NRST or LCD_NRST bit would
    // leave the radio or the panel held in reset. port1-pads runs first and has
    // already proven OUTPUT_REG1 equals this, so there is nothing to read.
    const uint8_t rest = TCA9535_PORT1_REST;
    uint8_t in0_before = 0;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &in0_before);

    uint8_t patterns[5] = { 0 };
    uint8_t bad_pattern = 0, bad_pads = 0;
    bool mismatch = false;
    bool cross_port = false;
    // Same gate the two sibling port tests use. Without it a button pressed during
    // the sweep changes in0 and reads as a cross-port bridge that does not exist
    bool held = (err == ESP_OK) && ((in0_before & 0x7F) != 0x7F);

    if (err == ESP_OK && !held) {
        patterns[0] = rest;
        patterns[1] = rest | (1 << TCA9535_HAPTIC_PIN);
        // RED is driven high here and nowhere else. Left low in every pattern its
        // signature would be 00000, so a short from that gate net to GND would be
        // commanded low throughout and pass - while the same fault on haptic, green
        // or blue fails, because each of those is driven high in one pattern
        patterns[2] = (rest & (uint8_t)~(1 << TCA9535_SX126X_NRST_PIN)) |
                      (uint8_t)(1 << TCA9535_RED_RGB_LED_PIN);
        patterns[3] = (rest | (1 << TCA9535_GREEN_RGB_LED_PIN)) & (uint8_t)~(1 << TCA9535_TSOP_EN_PIN);
        patterns[4] = (rest | (1 << TCA9535_BLUE_RGB_LED_PIN)) & (uint8_t)~(1 << TCA9535_LCD_NRST_PIN);

        for (int i = 0; i < 5 && err == ESP_OK && !mismatch && !cross_port; i++) {
            err = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG1, patterns[i]);
            if (err != ESP_OK) {
                break;
            }
            esp_rom_delay_us(200); // Let the pads settle before sensing them

            uint8_t pads = 0, in0 = 0;
            err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG1, &pads);
            if (err == ESP_OK) {
                err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &in0);
            }
            if (err != ESP_OK) {
                break;
            }
            if (pads != patterns[i]) {
                mismatch = true;
                bad_pattern = patterns[i];
                bad_pads = pads;
            } else if ((in0 & 0x7F) != (in0_before & 0x7F)) {
                // Button bits only. P07 is CHG, driven by the charger, and it can
                // legitimately toggle mid-sweep - comparing it would false-FAIL
                cross_port = true; // A port-1 pin bridged to a port-0 pin
                bad_pattern = patterns[i];
                bad_pads = in0;
            }
        }
    }

    // Unconditional: leave the expander exactly as it was found, and prove it
    esp_err_t restore = vh_tca_restore(0xFF, 0xFF, rest);
    xSemaphoreGive(xI2CBusMutex);

    if (restore != ESP_OK) {
        vh_record("port1-bridges", VH_G_IO, "Expander port-1 bridge sweep", VH_FAIL,
                VH_NO_VALUE, "RESTORE FAILED (%s) - power-cycle before use", esp_err_to_name(restore));
    } else if (held) {
        vh_record("port1-bridges", VH_G_IO, "Expander port-1 bridge sweep", VH_FAIL,
                VH_NO_VALUE, "a button was held - a fixture should touch nothing; see port0-idle");
    } else if (err != ESP_OK) {
        vh_record("port1-bridges", VH_G_IO, "Expander port-1 bridge sweep", VH_FAIL,
                VH_NO_VALUE, "transfer failed: %s", esp_err_to_name(err));
    } else if (mismatch) {
        vh_record("port1-bridges", VH_G_IO, "Expander port-1 bridge sweep", VH_FAIL, VH_NO_VALUE,
                "drove 0x%02X, pads read 0x%02X (bridged pins 0x%02X)",
                bad_pattern, bad_pads, (uint8_t)(bad_pattern ^ bad_pads));
    } else if (cross_port) {
        vh_record("port1-bridges", VH_G_IO, "Expander port-1 bridge sweep", VH_FAIL, VH_NO_VALUE,
                "port-0 moved to 0x%02X while driving port-1 0x%02X (cross-port bridge)",
                bad_pads, bad_pattern);
    } else {
        vh_record("port1-bridges", VH_G_IO, "Expander port-1 bridge sweep", VH_PASS,
                VH_NO_VALUE, "5 patterns, all 8 pads followed, no bridges");
    }
}

// Prove each button net has its 100k pull-up, and that no two are bridged: drive low as an output,
// check neighbours stay high, release, confirm it snaps back
// P02 (RIGHT) and P05 (HOME) are NEVER driven - they feed U27A, and both low pulls the MCU EN pin
// down, unrecoverable because the expander keeps driving through the reset
static void vh_check_port0_pullups(void)
{
    static const uint8_t pins[] = {
        TCA9535_USER_BUTTON_POWER_PIN,  // P00 OFF
        TCA9535_USER_BUTTON_UP_PIN,     // P01 UP
        TCA9535_USER_BUTTON_SELECT_PIN, // P03 SELECT
        TCA9535_USER_BUTTON_LEFT_PIN,   // P04 LEFT
        TCA9535_USER_BUTTON_DOWN_PIN,   // P06 DOWN
    };
    const uint8_t buttons = 0x7F;

    esp_err_t err = ESP_OK;
    int failed_pin = -1;
    const char *reason = NULL;
    bool held = false;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(pins) && err == ESP_OK && failed_pin < 0; i++) {
        uint8_t pin = pins[i];
        uint8_t mask = (uint8_t)(1 << pin);

        // Re-check before every pin: an operator holding a button would both
        // corrupt the result and, for the U27A pair, risk an unintended reset
        uint8_t gate = 0;
        err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &gate);
        if (err != ESP_OK) {
            break;
        }
        if ((gate & buttons) != buttons) {
            held = true;
            break;
        }

        // Latch before direction: the OUTPUT_REG0 POR default is 0xFF, so flipping direction first
        // would drive the net HIGH into a possibly-closed switch
        err = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG0, (uint8_t)(0xFF & ~mask));
        if (err == ESP_OK) {
            err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, (uint8_t)(0xFF & ~mask));
        }
        if (err != ESP_OK) {
            break;
        }

        uint8_t driven = 0;
        err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &driven);
        if (err != ESP_OK) {
            break;
        }
        if ((driven & mask) != 0) {
            failed_pin = pin;
            reason = "would not pull low (shorted to 3V3?)";
        } else if ((driven & buttons & (uint8_t)~mask) != (buttons & (uint8_t)~mask)) {
            failed_pin = pin;
            reason = "dragged a neighbour low (bridged nets)";
        }

        // Release regardless of the verdict above, then look for the recovery
        err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, 0xFF);
        if (err != ESP_OK) {
            break;
        }
        // Keep SHORT: a healthy 100k net recovers in ~15 us, so 200 us is ample. The bad case is
        // bounded the other way - a floating pin creeps up on the expander's input leakage, so a
        // longer wait makes a missing pull-up more likely to read high. Typical leakage is nA but
        // the spec allows +-1 uA, so this proves a pull-up on a typical part, not on every part
        esp_rom_delay_us(200);

        if (failed_pin < 0) {
            uint8_t released = 0;
            err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &released);
            if (err != ESP_OK) {
                break;
            }
            if ((released & mask) == 0) {
                failed_pin = pin;
                reason = "did not recover high (pull-up missing or open)";
            }
        }
    }

    esp_err_t restore = vh_tca_restore(0xFF, 0xFF, TCA9535_PORT1_REST);
    xSemaphoreGive(xI2CBusMutex);

    // Published so boot0-nor can skip: it drives the same P00/P06 nets, and a
    // bridge between them would make it report a broken U26A instead
    s_port0_nets_ok = (restore == ESP_OK) && !held && (err == ESP_OK) && (failed_pin < 0);

    if (restore != ESP_OK) {
        vh_record("port0-pullups", VH_G_IO, "Button pull-ups (float test)", VH_FAIL,
                VH_NO_VALUE, "RESTORE FAILED (%s) - power-cycle before use", esp_err_to_name(restore));
    } else if (held) {
        vh_record("port0-pullups", VH_G_IO, "Button pull-ups (float test)", VH_FAIL,
                VH_NO_VALUE, "a button was held - a fixture should touch nothing; see port0-idle");
    } else if (err != ESP_OK) {
        vh_record("port0-pullups", VH_G_IO, "Button pull-ups (float test)", VH_FAIL,
                VH_NO_VALUE, "transfer failed: %s", esp_err_to_name(err));
    } else if (failed_pin >= 0) {
        vh_record("port0-pullups", VH_G_IO, "Button pull-ups (float test)", VH_FAIL,
                failed_pin, "P0%d %s", failed_pin, reason);
    } else {
        vh_record("port0-pullups", VH_G_IO, "Button pull-ups (float test)", VH_PASS,
                VH_NO_VALUE, "5 of 7 nets verified (RIGHT/HOME skipped: they drive the EN combo)");
    }
}

// Prove the OFF+DOWN hardware combo end to end, without pressing anything: both nets feed
// U26A -> R62 -> U24 -> BOOT0 (module pin 15 / IO28, unused here), so driving them low and reading
// IO28 covers both button nets, the gate and the FET. Single-pin phases prove AND-of-lows
// Strapping latches at reset only; the both-low window is two back-to-back writes, no logging
static void vh_check_boot0_nor(void)
{
    /*
     * Bail BEFORE driving anything, not at the verdict.
     *
     * This test is the only one that can strand a board: GPIO28 is the MSB of the
     * C5 boot strapping word, so 1XXXX is SPI boot and ANY value with GPIO28 low
     * is a download/diag/test mode. If the release ever fails, the expander keeps
     * driving P00/P06 low across an MCU reset (it is POR-only), the chip samples
     * GPIO28 low, and the ROM loader runs instead of the firmware that would have
     * let go of the pin.
     *
     * So don't take that risk on a board whose P00/P06 nets already failed - the
     * result would be uninterpretable anyway, since a bridge between them makes
     * OFF-alone pull BOOT0 low and reads as a broken U26A.
     */
    if (!s_port0_nets_ok) {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_SKIP,
                VH_NO_VALUE, "not run: P00/P06 nets already failed - see port0-pullups");
        return;
    }

    gpio_config_t boot0_cfg = {
        .pin_bit_mask = (1ULL << VH_BOOT0_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   // R61 must be the only path high, so an
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // internal pull-up cannot mask it missing
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot0_cfg);
    esp_rom_delay_us(1000);

    const uint8_t off_mask = (uint8_t)(1 << TCA9535_USER_BUTTON_POWER_PIN);
    const uint8_t down_mask = (uint8_t)(1 << TCA9535_USER_BUTTON_DOWN_PIN);
    const uint8_t buttons = 0x7F;

    int idle = -1, off_only = -1, down_only = -1, both = -1, released = -1;
    bool held = false;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);

    uint8_t gate = 0;
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG0, &gate);
    if (err == ESP_OK && (gate & buttons) != buttons) {
        held = true;
    }

    if (err == ESP_OK && !held) {
        idle = gpio_get_level(VH_BOOT0_GPIO);

        // OFF alone
        err = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG0, (uint8_t)(0xFF & ~off_mask));
        if (err == ESP_OK) {
            err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, (uint8_t)(0xFF & ~off_mask));
        }
        if (err == ESP_OK) {
            esp_rom_delay_us(200);
            off_only = gpio_get_level(VH_BOOT0_GPIO);
            err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, 0xFF);
        }

        // DOWN alone
        if (err == ESP_OK) {
            err = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG0, (uint8_t)(0xFF & ~down_mask));
        }
        if (err == ESP_OK) {
            err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, (uint8_t)(0xFF & ~down_mask));
        }
        if (err == ESP_OK) {
            esp_rom_delay_us(200);
            down_only = gpio_get_level(VH_BOOT0_GPIO);
            err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, 0xFF);
        }

        // Both: keep this window as short as possible
        if (err == ESP_OK) {
            err = TCA9535WriteSingleRegister(TCA9535_OUTPUT_REG0,
                    (uint8_t)(0xFF & ~(off_mask | down_mask)));
        }
        if (err == ESP_OK) {
            err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0,
                    (uint8_t)(0xFF & ~(off_mask | down_mask)));
        }
        if (err == ESP_OK) {
            // Short but non-zero: U26A + U24 propagate in ns and BOOT0's RC is
            // ~100 ns, so this is pure margin against reading before it settles.
            // It leaves the both-driven window at roughly 0.5 ms, dominated by
            // the I2C writes either side of it
            esp_rom_delay_us(20);
            both = gpio_get_level(VH_BOOT0_GPIO);
            err = TCA9535WriteSingleRegister(TCA9535_CONFIG_REG0, 0xFF);
        }
        if (err == ESP_OK) {
            esp_rom_delay_us(200);
            released = gpio_get_level(VH_BOOT0_GPIO);
        }
    }

    esp_err_t restore = vh_tca_restore(0xFF, 0xFF, TCA9535_PORT1_REST);
    xSemaphoreGive(xI2CBusMutex);

    // Leave IO28 as a plain input; gpio_reset_pin would enable the internal pull-up
    gpio_config(&boot0_cfg);

    // Did the pin actually come back? This is the condition that matters, not the
    // restore's return code: while BOOT0 is held low the next reset enters a ROM
    // boot mode and the firmware never runs to release it. A plain power cycle does
    // not clear it either - the expander is POR-only, and its 3V3 is held up by USB
    // 5V through D2 as well as by the 3V3_EN latch, so BOTH have to come off.
    esp_rom_delay_us(200);
    // Only "we left it low" counts as stranded. If it was already low before the
    // test (idle != 1) the cause is on the board - a missing R61 or a short - and
    // telling the operator to strip the unit down would send them the wrong way.
    // The chip still boots to ROM either way, which the idle branch below says.
    bool boot0_stranded = (idle == 1) && (gpio_get_level(VH_BOOT0_GPIO) != 1);

    if (boot0_stranded) {
        ESP_LOGE(TAG, "BOOT0 LEFT LOW - next reset will enter ROM download mode; "
                      "remove BOTH the battery and USB to clear the expander");
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_FAIL, VH_NO_VALUE,
                "BOOT0 left low (%s) - next reset boots to ROM; remove battery AND USB",
                esp_err_to_name(restore));
    } else if (restore != ESP_OK) {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_FAIL, VH_NO_VALUE,
                "RESTORE FAILED (%s) - BOOT0 released, but check buttons; power-cycle before use",
                esp_err_to_name(restore));
    } else if (held) {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_FAIL,
                VH_NO_VALUE, "a button was held - a fixture should touch nothing; see port0-idle");
    } else if (err != ESP_OK) {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_FAIL,
                VH_NO_VALUE, "transfer failed: %s", esp_err_to_name(err));
    } else if (idle != 1 || released != 1) {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_FAIL, VH_NO_VALUE,
                "BOOT0 not high at rest (idle=%d released=%d) - R61 missing or net shorted low",
                idle, released);
    } else if (off_only != 1 || down_only != 1) {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_FAIL, VH_NO_VALUE,
                "one button alone pulled BOOT0 low (off=%d down=%d) - net shorted past U26A",
                off_only, down_only);
    } else if (both != 0) {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_FAIL, VH_NO_VALUE,
                "both low did not pull BOOT0 down - check U26A, R62, U24, R64");
    } else {
        vh_record("boot0-nor", VH_G_IO, "OFF+DOWN NOR combo (U26A -> BOOT0)", VH_PASS,
                VH_NO_VALUE, "truth table correct: 1,1,1,0,1");
    }
}
#endif // POLYCAST5_FACTORY_TEST

void verify_hardware_run_early(void)
{
    vh_begin_if_needed();
    vh_phase_enter();

    // Order matters: the census explains a missing expander, the register map
    // proves the directions every later port check depends on, and the data walk
    // proves the bus itself before any verdict is drawn from a readback
    vh_check_i2c_census();
    vh_check_tca_regmap();
    vh_check_tca_regwalk();
    vh_check_port1_pads();
#ifdef POLYCAST5_FACTORY_TEST
    // Before port0-idle: a port1-to-port0 bridge makes a button read low, and the
    // sweep names the actual bridged pins where port0-idle could only blame the switch
    vh_check_port1_bridges();
#endif
    vh_check_port0_idle();
    vh_check_tca_int_line();

#ifdef POLYCAST5_FACTORY_TEST
    // Intrusive: these drive expander pins. Every one restores unconditionally
    // and verifies the restore, because a port-0 pin left driven low reads as a
    // permanently held button and gpio_task would act on it moments later
    vh_check_port0_pullups();
    vh_check_boot0_nor();
#endif

    vh_phase_exit();
}

void verify_hardware_run(void)
{
    vh_begin_if_needed();
    vh_phase_enter();

    // PSRAM: external QSPI RAM die
    // Cell integrity is already covered elsewhere: CONFIG_SPIRAM_MEMTEST=y runs
    // esp_psram_extram_test() over the whole die during startup
    // !initialized branch is defense-in-depth should the SPIRAM config change
    if (!esp_psram_is_initialized()) {
        vh_record("psram-size", VH_G_MEMORY, "PSRAM (8 MB)", VH_FAIL, 0, "not detected at startup");
    } else {
        size_t psram = esp_psram_get_size();
        if (psram < VH_PSRAM_MIN_BYTES) {
            vh_record("psram-size", VH_G_MEMORY, "PSRAM (8 MB)", VH_FAIL,
                    (long)(psram / 1024), "only %u KB detected", (unsigned)(psram / 1024));
        } else {
            vh_record("psram-size", VH_G_MEMORY, "PSRAM (8 MB)", VH_PASS,
                    (long)(psram / 1024), "%u KB", (unsigned)(psram / 1024));
        }
    }

    // Accelerometer + magnetometer: report what the WHO_AM_I/Product ID probes in gpio_utils_init
    // left behind. Skip if the census already found nothing at the address - one fault, one report
    // is_present() is only s_dev != NULL, which the drivers also null on a failed read, so only
    // ESP_ERR_NOT_FOUND may be called a wrong part - anything else is a bad joint
    esp_err_t accel_init = lis2dh12_init_status();
    if (lis2dh12_is_present()) {
        vh_record("accel-present", VH_G_SENSOR, "LIS2DH12 accelerometer (I2C 0x19)", VH_PASS,
                VH_NO_VALUE, "WHO_AM_I OK");
    } else if (s_census_bus_stuck) {
        vh_record("accel-present", VH_G_SENSOR, "LIS2DH12 accelerometer (I2C 0x19)", VH_SKIP,
                VH_NO_VALUE, "I2C bus unusable - see i2c-census");
    } else if (s_census_ran && !s_census_saw_accel) {
        vh_record("accel-present", VH_G_SENSOR, "LIS2DH12 accelerometer (I2C 0x19)", VH_SKIP,
                VH_NO_VALUE, "nothing answers at 0x19 - already reported by i2c-census");
    } else if (accel_init == ESP_ERR_NOT_FOUND) {
        vh_record("accel-present", VH_G_SENSOR, "LIS2DH12 accelerometer (I2C 0x19)", VH_FAIL,
                VH_NO_VALUE, "answers at 0x19 but WHO_AM_I is wrong - wrong part fitted");
    } else {
        vh_record("accel-present", VH_G_SENSOR, "LIS2DH12 accelerometer (I2C 0x19)", VH_FAIL,
                VH_NO_VALUE, "answers at 0x19 but init failed (%s) - suspect the joints, not the part",
                esp_err_to_name(accel_init));
    }

    esp_err_t mag_init = mmc5603_init_status();
    if (mmc5603_is_present()) {
        vh_record("mag-present", VH_G_SENSOR, "MMC5603 magnetometer (I2C 0x30)", VH_PASS,
                VH_NO_VALUE, "Product ID OK");
    } else if (s_census_bus_stuck) {
        vh_record("mag-present", VH_G_SENSOR, "MMC5603 magnetometer (I2C 0x30)", VH_SKIP,
                VH_NO_VALUE, "I2C bus unusable - see i2c-census");
    } else if (s_census_ran && !s_census_saw_mag) {
        vh_record("mag-present", VH_G_SENSOR, "MMC5603 magnetometer (I2C 0x30)", VH_SKIP,
                VH_NO_VALUE, "nothing answers at 0x30 - already reported by i2c-census");
    } else if (mag_init == ESP_ERR_NOT_FOUND) {
        vh_record("mag-present", VH_G_SENSOR, "MMC5603 magnetometer (I2C 0x30)", VH_FAIL,
                VH_NO_VALUE, "answers at 0x30 but Product ID is wrong - wrong part fitted");
    } else {
        vh_record("mag-present", VH_G_SENSOR, "MMC5603 magnetometer (I2C 0x30)", VH_FAIL,
                VH_NO_VALUE, "answers at 0x30 but init failed (%s) - suspect the joints, not the part",
                esp_err_to_name(mag_init));
    }

    // TSOP IR receiver: power it on now so it settles while the mic probe runs
    // Q3 (the power gate) has NO coverage: VS carries C29 10uF with no discharge path but the
    // TSOP's own ~0.3 mA, which stops at its ~2.5 V minimum, so OUT reads high gated on or off
    // Both internal pulls stay off - a pull-down against the TSOP's ~30k sits near 2.0 V
    gpio_config_t tsop_cfg = {
        .pin_bit_mask = (1ULL << RMT_RX_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tsop_cfg);
    gpio_utils_en_tsop_receiver(true);

    // T5848 microphone: no control interface, so presence is inferred from data
    // liveness (a present mic's noise floor vs. a pulled-down silent line)
    bool mic_alive = false;
    esp_err_t err = ai_voice_mic_selftest(&mic_alive);
    if (err != ESP_OK) {
        vh_record("mic-alive", VH_G_AUDIO, "T5848 microphone (I2S)", VH_FAIL,
                VH_NO_VALUE, "probe failed: %s", esp_err_to_name(err));
    } else if (!mic_alive) {
        vh_record("mic-alive", VH_G_AUDIO, "T5848 microphone (I2S)", VH_FAIL,
                VH_NO_VALUE, "data line silent or stuck (mic absent, SD disconnected or shorted)");
    } else {
        vh_record("mic-alive", VH_G_AUDIO, "T5848 microphone (I2S)", VH_PASS,
                VH_NO_VALUE, "data line alive");
    }

    // SX1262: reset, then read a register with a known nonzero reset default
    // The HAL BUSY wait is bounded (100 ms) so an absent chip fails fast
    // SX_NRST comes from expander P16, and a pin stuck low holds the radio in reset - which reads
    // as "no SPI response" and bills one expander fault as two, so re-read the pad if port1-pads failed
    bool nrst_stuck_low = false;
    if (!s_port1_pads_ok) {
        uint8_t pads = 0;
        xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
        esp_err_t pad_err = TCA9535ReadSingleRegister(TCA9535_INPUT_REG1, &pads);
        xSemaphoreGive(xI2CBusMutex);
        nrst_stuck_low = (pad_err == ESP_OK) &&
                         (((pads >> TCA9535_SX126X_NRST_PIN) & 0x1) == 0);
    }

    if (nrst_stuck_low) {
        vh_record("sx1262-spi", VH_G_RF, "SX1262 LoRa radio (SPI2)", VH_SKIP, VH_NO_VALUE,
                "SX_NRST held low by the expander, radio cannot leave reset - see port1-pads");
    } else {
        sx126x_hal_reset(NULL);
        vTaskDelay(pdMS_TO_TICKS(10)); // Settle in STDBY_RC after reset

        uint8_t sync[2] = { 0 };
        if (sx126x_read_register(NULL, SX126X_REG_LR_SYNCWORD, sync, 2) != SX126X_STATUS_OK) {
            vh_record("sx1262-spi", VH_G_RF, "SX1262 LoRa radio (SPI2)", VH_FAIL,
                    VH_NO_VALUE, "no SPI response (BUSY timeout or bus error)");
        } else if (sync[0] == VH_SX1262_SYNC_MSB && sync[1] == VH_SX1262_SYNC_LSB) {
            vh_record("sx1262-spi", VH_G_RF, "SX1262 LoRa radio (SPI2)", VH_PASS,
                    VH_NO_VALUE, "sync word readback OK");
        } else {
            vh_record("sx1262-spi", VH_G_RF, "SX1262 LoRa radio (SPI2)", VH_FAIL,
                    ((long)sync[0] << 8) | sync[1],
                    "sync word readback 0x%02X%02X, expected 0x1424 (chip absent, MISO floating?)",
                    sync[0], sync[1]);
        }
    }

    // ST7789: no verdict is possible - the ER-TFT1.14-2 connector has no SDO/MISO pin, so the panel
    // can never answer a readback. The probe that used to live here was a permanent WARN and was
    // removed rather than left to masquerade as a check
    // LCD_RST is covered by the port-1 pad readback; the image itself by the operator card
    ESP_LOGI(TAG, "NOTE  ST7789 LCD panel: no readback path exists on this hardware "
                  "(panel connector has no SDO pin) - image is verified by the operator card");

    // Battery sense: short ADC burst through the divider + op-amp path
    // Integer formatting on purpose: float printf is heavy on the 3.5 KB main-task stack
    float vbat = gpio_utils_battery_selftest_voltage();
    int vbat_mv = (int)(vbat * 1000.0f + 0.5f);
    if (vbat <= 0.0f) {
        vh_record("batt-sense", VH_G_POWER, "Battery voltage sense (ADC)", VH_FAIL,
                0, "ADC read/calibration failed");
    } else if (vbat < 2.15f || vbat > 4.40f) {
        // The divider/op-amp math bottoms out at ~2.0 V when the sense pin reads
        // 0 V, so a value pinned there means a dead sense path or a flat battery
        vh_record("batt-sense", VH_G_POWER, "Battery voltage sense (ADC)", VH_WARN, vbat_mv,
                "implausible reading %d.%03d V - check battery and sense path",
                vbat_mv / 1000, vbat_mv % 1000);
    } else {
        vh_record("batt-sense", VH_G_POWER, "Battery voltage sense (ADC)", VH_PASS, vbat_mv,
                "%d.%02d V", vbat_mv / 1000, (vbat_mv % 1000) / 10);
    }

    // TSOP steady state, after the receiver has been powered through a few hundred ms of other work
    // Sampling over a window keeps a stray 38 kHz source from reading as a failure
    // Weak evidence of presence: GPIO6 has no internal pull, so an absent U15 or open OUT trace
    // floats and usually reads high too. What it reliably catches is an output held low
    bool idle_high = tsop_reads_high();

    gpio_utils_en_tsop_receiver(false); // Back to the power-saving idle state
    gpio_reset_pin(RMT_RX_GPIO_PIN);    // infrared_task reconfigures the pin when RX is used

    if (idle_high) {
        vh_record("tsop-idle", VH_G_IR, "TSOP IR receiver (GPIO6)", VH_PASS,
                VH_NO_VALUE, "not held low while powered");
    } else {
        vh_record("tsop-idle", VH_G_IR, "TSOP IR receiver (GPIO6)", VH_FAIL, VH_NO_VALUE,
                "went low after power on (receiver dropping out, or saturated by ambient IR)");
    }

    // Silicon lot and PSRAM encryption state, the two inputs to app_main's clock decision
    // The eFuse blk rev v0.3 lot corrupts ENCRYPTED PSRAM at 240 MHz; plaintext PSRAM or 160 MHz are both clean
    uint32_t chip_rev = efuse_hal_chip_revision(); // major * 100 + minor
    uint32_t blk_rev = efuse_hal_blk_version();
    uint32_t hp_dbias = 0, vol_gap = 0, pvt_dbias = 0;
    esp_efuse_read_field_blob(ESP_EFUSE_ACTIVE_HP_DBIAS, &hp_dbias, 4);
    esp_efuse_read_field_blob(ESP_EFUSE_LP_HP_DBIAS_VOL_GAP, &vol_gap, 5);
    esp_efuse_read_block(EFUSE_BLK2, &pvt_dbias, 249, 5);
    uint32_t fixed_dbias = (hp_dbias == 0) ? 28 : ((hp_dbias + 19 > 31) ? 31 : hp_dbias + 19);

    // Raw MMU entry for a PSRAM page: mmu_ll_read_entry() strips the bit, so read the registers.
    // Same fail-closed rule as app_main, and the index/content pair is held against interrupts
    bool psram_xts = esp_psram_is_initialized();
    if (psram_xts) {
        void *psram_probe = heap_caps_malloc(16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (psram_probe != NULL) {
            portMUX_TYPE mmu_mux = portMUX_INITIALIZER_UNLOCKED;
            portENTER_CRITICAL(&mmu_mux);
            REG_WRITE(SPI_MEM_MMU_ITEM_INDEX_REG(0), mmu_ll_get_entry_id(0, (uint32_t)psram_probe));
            psram_xts = (REG_READ(SPI_MEM_MMU_ITEM_CONTENT_REG(0)) & SOC_MMU_SENSITIVE) != 0;
            portEXIT_CRITICAL(&mmu_mux);
            heap_caps_free(psram_probe);
        }
    }
    bool lot_needs_cap = psram_xts && (blk_rev < POLYCAST5_CPU_240_MIN_EFUSE_BLK_REV);

    vh_record("efuse-rev", VH_G_IDENTITY, "Silicon lot (eFuse block rev)",
            lot_needs_cap ? VH_WARN : VH_PASS, (long)blk_rev,
            "chip v%lu.%lu blk v%lu.%lu HP_DBIAS %lu->%lu gap %lu PVT_DBIAS %lu PSRAM XTS %s (%s)",
            (unsigned long)(chip_rev / 100), (unsigned long)(chip_rev % 100),
            (unsigned long)(blk_rev / 100), (unsigned long)(blk_rev % 100),
            (unsigned long)hp_dbias, (unsigned long)fixed_dbias, (unsigned long)vol_gap,
            (unsigned long)pvt_dbias, psram_xts ? "on" : "off",
            lot_needs_cap ? "240 MHz unsafe on this lot, capped to 160" : "240 MHz eligible");

    // CPU clock, MEASURED rather than asked for: the Kconfig boot frequency, the DFS ceiling and
    // what the core is actually running at are three different numbers, and only the last matters
    // Count real cycles against esp_timer rather than trusting any reported value
    // Also catches a unit slower than its siblings - app_main only asks for 240 MHz on rev v1.2+
    int64_t clk_t0 = esp_timer_get_time();
    uint32_t clk_c0 = esp_cpu_get_cycle_count();
    while ((esp_timer_get_time() - clk_t0) < VH_CPU_MEASURE_US) {
        // Busy-wait on purpose: yielding would let DFS drop the clock mid-measurement
    }
    uint32_t clk_c1 = esp_cpu_get_cycle_count();
    int64_t clk_us = esp_timer_get_time() - clk_t0;
    int cpu_mhz = (clk_us > 0) ? (int)((uint32_t)(clk_c1 - clk_c0) / (uint32_t)clk_us) : 0;

    esp_pm_config_t pm_now = { 0 };
    int pm_max = (esp_pm_get_configuration(&pm_now) == ESP_OK) ? pm_now.max_freq_mhz : 0;

    if (cpu_mhz <= 0) {
        vh_record("cpu-freq", VH_G_IDENTITY, "CPU clock (measured)", VH_FAIL,
                VH_NO_VALUE, "measurement failed");
    } else {
        // Informational: DFS legitimately runs below the ceiling when idle, so a
        // low reading here is not a fault - it is the number, and the batch tells
        // you what normal looks like
        vh_record("cpu-freq", VH_G_IDENTITY, "CPU clock (measured)", VH_PASS, cpu_mhz,
                "%d MHz now, DFS ceiling %d MHz", cpu_mhz, pm_max);
    }

    // Heap integrity, last so it covers everything before it. A bisect point as much as a check:
    // corrupt here means the culprit ran before the tasks started; clean here but a later TLSF
    // assert means task code is to blame
    // Only sees corruption in block headers - POISONING_COMPREHENSIVE catches the write itself
#ifdef CONFIG_HEAP_POISONING_COMPREHENSIVE
    // Internal RAM only. Comprehensive poisoning turns this into a byte-by-byte verify of every
    // free block, and the free PSRAM pool is over 7 MB - held under the heap spinlock it overruns
    // the interrupt watchdog and panics. Nothing is lost: poisoning validates those on alloc/free
    bool heap_ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true);
    const char *heap_scope = "internal RAM only";
#else
    bool heap_ok = heap_caps_check_integrity_all(true);
    const char *heap_scope = "all regions";
#endif

    if (heap_ok) {
        vh_record("heap-integrity", VH_G_MEMORY, "Heap integrity after boot", VH_PASS,
                VH_NO_VALUE, "%s intact", heap_scope);
    } else {
        vh_record("heap-integrity", VH_G_MEMORY, "Heap integrity after boot", VH_FAIL, VH_NO_VALUE,
                "corrupt BEFORE tasks started (%s) - suspect init, not task code", heap_scope);
    }

    vh_phase_exit(); // Stop the clock before the banner: printing is not test time
    vh_report_and_free();
}

// Print the combined report for whatever phases ran, then release the buffer
// Two callers need it: the normal late phase, and the bus-only path when gpio_utils_init fails
// Without it the worst defect on the board - an unreachable expander - would emit a VHR|BEGIN and
// then nothing, indistinguishable from a unit that died mid-boot
static void vh_report_and_free(void)
{
    // Summary banner. Buffered entries from BOTH phases print here, grouped
    // If the buffer was unavailable they were already logged inline and s_entry_count is 0, so this loop is a no-op
    ESP_LOGI(TAG, "================ HARDWARE VERIFICATION ===============");
    int seq = 0;
    for (int g = 0; g < VH_GROUP_COUNT && s_entries != NULL; g++) {
        bool header_done = false;
        for (int i = 0; i < s_entry_count; i++) {
            if (s_entries[i].group != g) {
                continue;
            }
            if (!header_done) {
                ESP_LOGI(TAG, "-- %s", vh_group_names[g]);
                header_done = true;
            }
            vh_log_one(&s_entries[i]);
            vh_emit_machine(seq++, &s_entries[i]);
        }
    }

    int pass = s_counts[VH_PASS];
    int warn = s_counts[VH_WARN];
    int fail = s_counts[VH_FAIL];
    int skip = s_counts[VH_SKIP];
    int total_ms = (int)(s_busy_us / 1000);
    // Skips are excluded deliberately: they carry no verdict, so they must not turn
    // a clean run into a WARN, nor mask a failure
    const char *overall = (fail > 0) ? "FAIL" : ((warn > 0) ? "WARN" : "PASS");

    char skipped[24] = "";
    if (skip > 0) {
        snprintf(skipped, sizeof(skipped), ", %d SKIP", skip);
    }

    if (fail > 0) {
        ESP_LOGE(TAG, "RESULT: %d FAIL, %d WARN, %d PASS%s - HARDWARE PROBLEM DETECTED (%d ms)",
                fail, warn, pass, skipped, total_ms);
    } else if (warn > 0) {
        ESP_LOGW(TAG, "RESULT: 0 FAIL, %d WARN, %d PASS%s (%d ms)", warn, pass, skipped, total_ms);
    } else {
        ESP_LOGI(TAG, "RESULT: all %d checks passed%s (%d ms)", pass, skipped, total_ms);
    }
    ESP_LOGI(TAG, "======================================================");

    printf("VHR|END|%d|%d|%d|%d|%d|%s\n", fail, warn, pass, skip, total_ms, overall);

    free(s_entries); // free(NULL) is a no-op if the allocation failed
    s_entries = NULL;
    s_entry_count = 0; // Must track s_entries: the summary loop indexes one by the other
    // Counts survive for verify_hardware_get_summary(); only the detail buffer is released
}

// Bus-only run for the boot-halt path: app_main calls this when gpio_utils_init fails, so the
// fixture still gets a parseable record naming the reason instead of silence
// The bus object survives a missing expander - TCA9535Init creates it without putting a byte on
// the wire - so the census is still meaningful here
void verify_hardware_run_bus_only(esp_err_t init_err)
{
    vh_begin_if_needed();
    vh_phase_enter();

    // Record the boot failure FIRST and unconditionally: the census alone is not a verdict here
    // gpio_utils_init can fail on a register write while all three parts still answer an address
    // probe, since a probe is the shortest transaction on the bus - reporting only the census then
    // emits an overall PASS for a unit that never starts a task
    vh_record("boot-halted", VH_G_IO, "Boot halted in gpio_utils_init", VH_FAIL, VH_NO_VALUE,
            "%s - no tasks started; the checks below are bus-level only",
            esp_err_to_name(init_err));

    // Only then the census, and only if there is a bus to sweep
    if (i2c_bus_handle != NULL) {
        vh_check_i2c_census();
    }

    vh_phase_exit();
    vh_report_and_free();
}

void verify_hardware_get_summary(int *pass, int *warn, int *fail)
{
    if (pass != NULL) {
        *pass = s_counts[VH_PASS];
    }
    if (warn != NULL) {
        *warn = s_counts[VH_WARN];
    }
    if (fail != NULL) {
        *fail = s_counts[VH_FAIL];
    }
}
