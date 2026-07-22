/*
 * Boot-time hardware verification.
 *
 * Probes every external IC with an observable interface and logs one
 * PASS/WARN/FAIL line per device plus a summary banner, so assembly or
 * connection faults show up in the serial log on the first boot after
 * flashing.
 *
 * Runs from app_main after the buses and HALs are up but BEFORE the
 * FreeRTOS tasks are created: the probes assume they are the only bus
 * users and that no task has claimed the peripherals yet.
 *
 * Devices with no feedback path (haptic motor, RGB LEDs, IR TX LED,
 * backlight FET, charger status line) cannot be probed directly; their
 * control lines all run through the TCA9535, so the expander check
 * covers their reachability.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_rom_sys.h"

#include "driver/gpio.h"

#include "polycast5_gpios.h"

#include "TCA9535.h"
#include "gpio_task.h"
#include "gpio_utils.h"
#include "lis2dh12.h"
#include "mmc5603.h"
#include "lcd_utils.h"
#include "ai_voice.h"
#include "sx126x.h"
#include "sx126x_regs.h"
#include "sx126x_hal.h"

#include "verify_hardware.h"

static const char *TAG = "VERIFY_HW";

// Expected PSRAM size
#define VH_PSRAM_MIN_BYTES (8 * 1024 * 1024)

// SX1262 LoRa sync word reset default 0x1424 (see SX126X_REG_LR_SYNCWORD)
#define VH_SX1262_SYNC_MSB 0x14
#define VH_SX1262_SYNC_LSB 0x24

// Values lcd_init_driver() wrote to the panel, expected back from the readback probe
#define VH_LCD_MADCTL 0x60 // Memory Data Access Control (rotation)
#define VH_LCD_COLMOD 0x55 // Interface Pixel Format (RGB565)

typedef enum {
    VH_PASS,
    VH_WARN,
    VH_FAIL,
    VH_VERDICT_COUNT,
} vh_verdict_t;

typedef struct {
    const char *name;
    vh_verdict_t verdict;
    char detail[112];
} vh_entry_t;

#define VH_MAX_ENTRIES 10

// Verdicts are buffered so they print as one block at the end instead of scattering through boot logging
// The buffer is allocated for the duration of the run and freed before returning
static vh_entry_t *s_entries = NULL;
static int s_entry_count = 0;
static int s_counts[VH_VERDICT_COUNT];

static void vh_log_one(const char *name, vh_verdict_t verdict, const char *detail)
{
    switch (verdict) {
    case VH_PASS:
        ESP_LOGI(TAG, "PASS  %s: %s", name, detail);
        break;
    case VH_WARN:
        ESP_LOGW(TAG, "WARN  %s: %s", name, detail);
        break;
    case VH_FAIL:
    default: // Anything unexpected is surfaced as a failure rather than dropped
        ESP_LOGE(TAG, "FAIL  %s: %s", name, detail);
        break;
    }
}

static void vh_record(const char *name, vh_verdict_t verdict, const char *fmt, ...)
{
    s_counts[verdict]++; // Tallied here so the summary is right on either path below

    va_list args;
    va_start(args, fmt);
    if (s_entries != NULL && s_entry_count < VH_MAX_ENTRIES) {
        // Format straight into the entry; printed with the rest at the end
        vh_entry_t *e = &s_entries[s_entry_count++];
        e->name = name;
        e->verdict = verdict;
        vsnprintf(e->detail, sizeof(e->detail), fmt, args);
    } else {
        // No buffer: print now rather than lose the verdict
        char detail[112];
        vsnprintf(detail, sizeof(detail), fmt, args);
        vh_log_one(name, verdict, detail);
    }
    va_end(args);
}

// Sample the TSOP output for up to ~20 ms and report whether it ever drives high
// Idle is high; lows only occur during (brief) IR activity, so any high = driven
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

void verify_hardware_run(void)
{
    s_entry_count = 0;
    for (int i = 0; i < VH_VERDICT_COUNT; i++) {
        s_counts[i] = 0;
    }

    // Scratch buffer for the batched summary, released before this function returns
    // May land in PSRAM, which is fine: reaching app_main at all proves PSRAM came up
    s_entries = malloc(sizeof(vh_entry_t) * VH_MAX_ENTRIES);
    if (s_entries == NULL) {
        ESP_LOGW(TAG, "verdict buffer unavailable; logging each result inline");
    }

    // PSRAM: external QSPI RAM die
    // !initialized branch is defense-in-depth should the SPIRAM config change
    if (!esp_psram_is_initialized()) {
        vh_record("PSRAM (8 MB)", VH_FAIL, "not detected at startup");
    } else {
        size_t psram = esp_psram_get_size();
        if (psram < VH_PSRAM_MIN_BYTES) {
            vh_record("PSRAM (8 MB)", VH_FAIL, "only %u KB detected", (unsigned)(psram / 1024));
        } else {
            vh_record("PSRAM (8 MB)", VH_PASS, "%u KB", (unsigned)(psram / 1024));
        }
    }

    // TCA9535: read back the port0 direction config gpio_utils_init wrote (0xFF)
    // A dead expander already halts boot in app_main
    uint8_t cfg0 = 0;
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    esp_err_t err = TCA9535ReadSingleRegister(TCA9535_CONFIG_REG0, &cfg0);
    xSemaphoreGive(xI2CBusMutex);
    if (err != ESP_OK) {
        vh_record("TCA9535 GPIO expander (I2C 0x20)", VH_FAIL, "unreachable: %s", esp_err_to_name(err));
    } else if (cfg0 != 0xFF) {
        vh_record("TCA9535 GPIO expander (I2C 0x20)", VH_FAIL, "config readback 0x%02X, expected 0xFF", cfg0);
    } else {
        vh_record("TCA9535 GPIO expander (I2C 0x20)", VH_PASS, "config readback OK");
    }

    // Accelerometer + magnetometer: their WHO_AM_I/Product ID probes already ran
    // inside gpio_utils_init; report the outcome those probes left behind
    if (lis2dh12_is_present()) {
        vh_record("LIS2DH12 accelerometer (I2C 0x19)", VH_PASS, "WHO_AM_I OK");
    } else {
        vh_record("LIS2DH12 accelerometer (I2C 0x19)", VH_FAIL, "not found at init (see LIS2DH12 log above)");
    }
    if (mmc5603_is_present()) {
        vh_record("MMC5603 magnetometer (I2C 0x30)", VH_PASS, "Product ID OK");
    } else {
        vh_record("MMC5603 magnetometer (I2C 0x30)", VH_FAIL, "not found at init (see MMC5603 log above)");
    }

    // TSOP IR receiver: power it on now so it settles while the mic probe runs
    gpio_utils_en_tsop_receiver(true);

    // T5848 microphone: no control interface, so presence is inferred from data
    // liveness (a present mic's noise floor vs. a pulled-down silent line)
    bool mic_alive = false;
    err = ai_voice_mic_selftest(&mic_alive);
    if (err != ESP_OK) {
        vh_record("T5848 microphone (I2S)", VH_FAIL, "probe failed: %s", esp_err_to_name(err));
    } else if (!mic_alive) {
        vh_record("T5848 microphone (I2S)", VH_FAIL, "data line silent or stuck (mic absent, SD disconnected or shorted)");
    } else {
        vh_record("T5848 microphone (I2S)", VH_PASS, "data line alive");
    }

    // SX1262: reset, then read a register with a known nonzero reset default
    // The HAL BUSY wait is bounded (100 ms) so an absent chip fails fast instead of hanging
    sx126x_hal_reset(NULL);
    vTaskDelay(pdMS_TO_TICKS(10)); // Settle in STDBY_RC after reset

    uint8_t sync[2] = { 0 };
    if (sx126x_read_register(NULL, SX126X_REG_LR_SYNCWORD, sync, 2) != SX126X_STATUS_OK) {
        vh_record("SX1262 LoRa radio (SPI2)", VH_FAIL, "no SPI response (BUSY timeout or bus error)");
    } else if (sync[0] == VH_SX1262_SYNC_MSB && sync[1] == VH_SX1262_SYNC_LSB) {
        vh_record("SX1262 LoRa radio (SPI2)", VH_PASS, "sync word readback OK");
    } else {
        vh_record("SX1262 LoRa radio (SPI2)", VH_FAIL,
                "sync word readback 0x%02X%02X, expected 0x1424 (chip absent, MISO floating?)", sync[0], sync[1]);
    }

    // ST7789: read back MADCTL/COLMOD written during lcd_init_driver
    // WARN (not FAIL) on no-readback because many panels do not route their SDO pin
    uint8_t madctl[2] = { 0 };
    uint8_t colmod[2] = { 0 };
    err = lcd_probe_panel(madctl, colmod);
    if (err != ESP_OK) {
        vh_record("ST7789 LCD panel (SPI2)", VH_WARN, "readback transaction failed: %s", esp_err_to_name(err));
    } else {
        // Accept each register framed directly or shifted by one dummy clock (OR
        // within a register), but require BOTH to read back (AND between them)
        uint8_t mad_a = madctl[0];
        uint8_t mad_b = ((madctl[0] << 1) | (madctl[1] >> 7)) & 0xFF;
        uint8_t col_a = colmod[0];
        uint8_t col_b = ((colmod[0] << 1) | (colmod[1] >> 7)) & 0xFF;
        bool match = (mad_a == VH_LCD_MADCTL || mad_b == VH_LCD_MADCTL) &&
                     (col_a == VH_LCD_COLMOD || col_b == VH_LCD_COLMOD);
        bool floating = ((madctl[0] == 0x00 && colmod[0] == 0x00) ||
                         (madctl[0] == 0xFF && colmod[0] == 0xFF));
        if (match) {
            vh_record("ST7789 LCD panel (SPI2)", VH_PASS, "MADCTL/COLMOD readback OK");
        } else if (floating) {
            vh_record("ST7789 LCD panel (SPI2)", VH_WARN,
                    "no readback (panel absent, or its SDO pin is not wired) - verify the screen visually");
        } else {
            vh_record("ST7789 LCD panel (SPI2)", VH_WARN,
                    "unexpected readback MADCTL=0x%02X COLMOD=0x%02X - verify the screen visually",
                    madctl[0], colmod[0]);
        }
    }

    // Battery sense: short ADC burst through the divider + op-amp path
    // Integer formatting on purpose: float printf is heavy on the 3.5 KB main-task stack
    float vbat = gpio_utils_battery_selftest_voltage();
    int vbat_mv = (int)(vbat * 1000.0f + 0.5f);
    if (vbat <= 0.0f) {
        vh_record("Battery voltage sense (ADC)", VH_FAIL, "ADC read/calibration failed");
    } else if (vbat < 3.05f || vbat > 4.40f) {
        // The divider/op-amp math bottoms out at ~3.0 V when the sense pin reads
        // 0 V, so a value pinned there means a dead sense path or a flat battery
        vh_record("Battery voltage sense (ADC)", VH_WARN,
                "implausible reading %d.%03d V - check battery and sense path", vbat_mv / 1000, vbat_mv % 1000);
    } else {
        vh_record("Battery voltage sense (ADC)", VH_PASS, "%d.%02d V", vbat_mv / 1000, (vbat_mv % 1000) / 10);
    }

    // TSOP IR receiver: it actively drives its output high at idle, so probe the line against an internal pull-down
    gpio_config_t tsop_cfg = {
        .pin_bit_mask = (1ULL << RMT_RX_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&tsop_cfg);
    esp_rom_delay_us(2000); // Pull settle (sub-tick: pdMS_TO_TICKS(2) is 0 at 100 Hz)
    bool high_vs_pulldown = tsop_reads_high();

    gpio_set_pull_mode(RMT_RX_GPIO_PIN, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(2000);
    bool high_vs_pullup = tsop_reads_high();

    gpio_reset_pin(RMT_RX_GPIO_PIN); // infrared_task reconfigures the pin when RX is used
    gpio_utils_en_tsop_receiver(false); // Back to the power-saving idle state

    if (!high_vs_pullup) {
        vh_record("TSOP IR receiver (GPIO6)", VH_FAIL, "output stuck low (shorted, or constant IR interference)");
    } else if (high_vs_pulldown) {
        vh_record("TSOP IR receiver (GPIO6)", VH_PASS, "output drives idle-high");
    } else {
        vh_record("TSOP IR receiver (GPIO6)", VH_WARN,
                "no drive against pull-down (module absent, or its output pull-up is too weak for this test)");
    }

    // Summary banner. Buffered entries print here
    // If the buffer was unavailable they were already logged inline and s_entry_count is 0, so this loop is a no-op
    ESP_LOGI(TAG, "================ HARDWARE VERIFICATION ===============");
    for (int i = 0; i < s_entry_count; i++) {
        vh_log_one(s_entries[i].name, s_entries[i].verdict, s_entries[i].detail);
    }

    int pass = s_counts[VH_PASS];
    int warn = s_counts[VH_WARN];
    int fail = s_counts[VH_FAIL];
    if (fail > 0) {
        ESP_LOGE(TAG, "RESULT: %d FAIL, %d WARN, %d PASS - HARDWARE PROBLEM DETECTED", fail, warn, pass);
    } else if (warn > 0) {
        ESP_LOGW(TAG, "RESULT: 0 FAIL, %d WARN, %d PASS", warn, pass);
    } else {
        ESP_LOGI(TAG, "RESULT: all %d checks passed", pass);
    }
    ESP_LOGI(TAG, "======================================================");

    free(s_entries); // free(NULL) is a no-op if the allocation failed
    s_entries = NULL;
    s_entry_count = 0;
}
