#include "ir_exp.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "TCA9535.h" // i2c_bus_handle
#include "gpio_task.h" // xI2CBusMutex
#include "polycast5_macros.h" // POLYCAST5_DEBUG

#define TAG "MLX90642"

#define I2C_TIMEOUT_MS       100 // Per-transaction timeout for short accesses
#define I2C_FRAME_TIMEOUT_MS 300 // Frame read is ~35 ms at 400 kHz, ~139 ms at 100 kHz

// Register map
#define MLX_REG_CMD          0x0180 // Opcode register: sleep / sync / reset
#define MLX_REG_EE_REFRESH   0x11F0 // EEPROM: refresh rate in bits 2:0
#define MLX_REG_EE_APP_CFG   0x11F4 // EEPROM: application config
#define MLX_CFG_RAW_OUTPUT   (1u << 8)  // Set = 0x342C carries normalised raw IR, not To
#define MLX_CFG_STEP_MODE    (1u << 11) // Set = measures only when triggered, never free-runs
#define MLX_REG_FRAME_BASE   0x342C // First pixel (To, 0.02 C per LSB)
#define MLX_REG_FLAGS        0x3C14 // BUSY / READY / frame-update status
#define MLX_REG_FW_VER_HI    0xFFF8 // Major version in the MSByte (LSByte unused)
#define MLX_REG_FW_VER_LO    0xFFFA // Patch version in the MSByte, minor in the LSByte

#define MLX_OPCODE_CONFIG    0x3A2E // Prefix for the EEPROM configuration command

// Commands written to MLX_REG_CMD
#define MLX_CMD_SYNC         0x0001
#define MLX_CMD_RESET        0x0006
#define MLX_CMD_SLEEP        0x0007

#define MLX_WAKE_BYTE        0x57 // Wake is a bare single byte, not a register write

// MLX_REG_FLAGS bits
#define MLX_FLAG_BUSY        (1u << 0) // Temperatures are being calculated
#define MLX_FLAG_READY       (1u << 8) // Results available; cleared by reading the frame base
#define MLX_FLAG_FRAME_UPD   (1u << 9) // Frame data is mid-update, overlap possible

#define MLX_REFRESH_MASK     0x07 // Bits 2:0 of MLX_REG_EE_REFRESH; the rest are reserved

// The sensor's own MCU applies the config command and an in-flight measurement wins first, so a new value can take up to 500 ms
#define MLX_CONFIG_APPLY_MS  700 // 500 ms plus margin
#define MLX_CONFIG_POLL_MS   50

// 768 pixels plus the die temperature, which sits in the word right after the last pixel, so one contiguous read gets both
#define MLX_FRAME_WORDS (MLX90642_PIXELS + 1)
#define MLX_FRAME_BYTES (MLX_FRAME_WORDS * 2)

// Per-device handle on the shared bus; NULL until init succeeds, which the read functions use as a not-ready guard
static i2c_master_dev_handle_t s_dev = NULL;

// Receive staging buffer, internal RAM because the I2C ISR fills it from the hardware FIFO; only allocated while the page is open
static uint8_t *s_rx = NULL;

// Firmware revision, cached at init for the capability check in mlx90642_set_refresh()
static uint8_t s_fw_major = 0;
static uint8_t s_fw_minor = 0;
static uint8_t s_fw_patch = 0;

// Refresh rate the sensor is actually running, read from EEPROM at init. The frame period must come from this:
// inferring it from the bus clock goes silently wrong when a rate change was refused or the part kept a non-default setting
static uint8_t s_refresh_code = MLX90642_REFRESH_8HZ;

// Hand the shared bus back around a wait: holding xI2CBusMutex across a multi-hundred-millisecond sleep freezes
// gpio_task's button polling and the haptic-off timer callback, which reads as a hung device with the motor stuck on.
// Caller must hold xI2CBusMutex; it still holds it on return.
static void mlx_bus_yield(uint32_t ms)
{
    xSemaphoreGive(xI2CBusMutex);
    vTaskDelay(pdMS_TO_TICKS(ms));
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
}

// Read len bytes from a 16-bit register address: write the address MSByte-first, then repeated-START and read.
// These are BYTE addresses stepping by 2 per word and they auto-increment, so one read walks consecutive words.
// Caller must hold xI2CBusMutex.
static esp_err_t mlx_read(uint16_t reg, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), buf, len, timeout_ms);
}

// Read a single 16-bit word. Caller must hold xI2CBusMutex.
static esp_err_t mlx_read_word(uint16_t reg, uint16_t *out)
{
    uint8_t buf[2];
    esp_err_t ret = mlx_read(reg, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        *out = ((uint16_t)buf[0] << 8) | buf[1]; // MSByte first on the wire
    }
    return ret;
}

// Write one 16-bit register: address MSByte-first, then value MSByte-first. Caller must hold xI2CBusMutex.
static esp_err_t mlx_write_reg(uint16_t reg, uint16_t val)
{
    uint8_t tx[4] = {
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
        (uint8_t)(val >> 8), (uint8_t)(val & 0xFF),
    };
    return i2c_master_transmit(s_dev, tx, sizeof(tx), I2C_TIMEOUT_MS);
}

// EEPROM configuration command: fixed opcode, then the target address and value. The internal MCU runs the
// erase/write itself, taking up to MLX_CONFIG_APPLY_MS. Caller must hold xI2CBusMutex.
static esp_err_t mlx_write_config(uint16_t reg, uint16_t val)
{
    uint8_t tx[6] = {
        (uint8_t)(MLX_OPCODE_CONFIG >> 8), (uint8_t)(MLX_OPCODE_CONFIG & 0xFF),
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
        (uint8_t)(val >> 8), (uint8_t)(val & 0xFF),
    };
    return i2c_master_transmit(s_dev, tx, sizeof(tx), I2C_TIMEOUT_MS);
}

esp_err_t mlx90642_init(uint32_t scl_hz)
{
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dev != NULL) { // Already up
        return ESP_OK;
    }

    // scl_speed_hz is per-device, so registering the array at 400 kHz leaves the 100 kHz TCA9535 untouched
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MLX90642_I2C_ADDR,
        .scl_speed_hz = scl_hz,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        s_dev = NULL;
        return ret;
    }

    if (s_rx == NULL) {
        s_rx = heap_caps_malloc(MLX_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_rx == NULL) {
            ESP_LOGE(TAG, "Failed to alloc %d B frame staging buffer", MLX_FRAME_BYTES);
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    // The part may be asleep from a previous session; the wake byte is harmless if not.
    (void)mlx90642_wake();
    mlx_bus_yield(20); // Nothing to say to the array while it boots - let the bus work

    // Both version words are read so the full revision is known: the config command used by the 100 kHz fallback
    // needs 1.16.5 or later, defective-pixel correction needs 1.18.0. Per the datasheet the MAJOR version is the
    // MSByte of 0xFFF8, the patch the MSByte of 0xFFFA and the minor its LSByte; a working part reads 0x0100 / 0x0510 as 1.16.5
    uint16_t fw_hi = 0;
    uint16_t fw_lo = 0;
    ret = mlx_read_word(MLX_REG_FW_VER_HI, &fw_hi);
    if (ret == ESP_OK) {
        ret = mlx_read_word(MLX_REG_FW_VER_LO, &fw_lo);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FW version read failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    // Only the degenerate bus patterns mean nothing answered: an unmapped address reads back all ones, a line held
    // low reads back all zeros. Anything else is a real device, so do not second-guess the version number itself
    if (fw_hi == 0xFFFF || fw_hi == 0x0000) {
        ESP_LOGE(TAG, "No response from the array (FW word 0x%04X)", fw_hi);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    s_fw_major = (uint8_t)(fw_hi >> 8);
    s_fw_minor = (uint8_t)(fw_lo & 0xFF);
    s_fw_patch = (uint8_t)(fw_lo >> 8);

    // Both bits are EEPROM-backed, so they survive power cycles and a mis-programmed part stays mis-programmed.
    // Checked rather than corrected: a write costs an EEPROM cycle and neither state is one this firmware sets.
    uint16_t app_cfg = 0;
    if (mlx_read_word(MLX_REG_EE_APP_CFG, &app_cfg) == ESP_OK) {
        // Warned about, NOT refused. The datasheet marks a default on the step-mode row and on neither
        // output-format row, so "0 means temperatures" is inferred from the part being sold factory-calibrated,
        // not stated. Refusing on an inferred default would permanently kill the page on a part this firmware
        // has no way to reprogram, so the image is left to speak for itself: raw output renders as nonsense
        // temperatures, and this line says why.
        if (app_cfg & MLX_CFG_RAW_OUTPUT) {
            ESP_LOGW(TAG, "Array may be set to raw output (cfg 0x%04X); readings will be meaningless", app_cfg);
        }
        // Refused, because this default IS stated and the consequence is unambiguous: nothing here ever sends a
        // measurement trigger, so the part would sit at WARMING UP forever with no other explanation.
        if (app_cfg & MLX_CFG_STEP_MODE) {
            ESP_LOGE(TAG, "Array is in step mode (cfg 0x%04X); it will not free-run", app_cfg);
            ret = ESP_ERR_NOT_SUPPORTED;
            goto fail;
        }
    }

    // Cache the rate the part is really running. The setting lives in EEPROM, so it carries over from whatever the
    // last session left behind, including a 4 Hz fallback written months ago on a different bus speed
    uint16_t cfg = 0;
    if (mlx_read_word(MLX_REG_EE_REFRESH, &cfg) == ESP_OK) {
        s_refresh_code = (uint8_t)(cfg & MLX_REFRESH_MASK);
    } else {
        s_refresh_code = MLX90642_REFRESH_8HZ; // Unknown: assume the factory default
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "MLX90642 up at %lu Hz (FW %u.%u.%u, refresh code %u, %u ms frames)",
            (unsigned long)scl_hz, s_fw_major, s_fw_minor, s_fw_patch,
            s_refresh_code, mlx90642_frame_period_ms());
#endif
    return ESP_OK;

fail:
    // Back to s_dev == NULL so the guards report not-initialised
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    heap_caps_free(s_rx);
    s_rx = NULL;
    return ret;
}

void mlx90642_deinit(void)
{
    if (s_dev != NULL) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    heap_caps_free(s_rx);
    s_rx = NULL;
}

bool mlx90642_is_present(void)
{
    return s_dev != NULL;
}

uint16_t mlx90642_frame_period_ms(void)
{
    switch (s_refresh_code) {
    case MLX90642_REFRESH_2HZ:  return 500;
    case MLX90642_REFRESH_4HZ:  return 250;
    case MLX90642_REFRESH_16HZ: return 63; // 15.2 Hz in absolute-temperature mode
    case MLX90642_REFRESH_8HZ:
    default:                    return 125;
    }
}

esp_err_t mlx90642_set_refresh(uint8_t code)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    code &= MLX_REFRESH_MASK;

    // Read-modify-write: only bits 2:0 are ours, the rest are reserved and must survive
    uint16_t cfg = 0;
    esp_err_t ret = mlx_read_word(MLX_REG_EE_REFRESH, &cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((cfg & MLX_REFRESH_MASK) == code) { // Already there: do not burn an EEPROM cycle
        s_refresh_code = code;
        return ESP_OK;
    }

    // The configuration command only exists from firmware 1.16.5 onward; checked here so an already-correct rate still succeeds on old parts
    const uint32_t fw = ((uint32_t)s_fw_major << 16) | ((uint32_t)s_fw_minor << 8) | s_fw_patch;
    if (fw < ((1u << 16) | (16u << 8) | 5u)) {
        ESP_LOGW(TAG, "FW %u.%u.%u predates the config command; staying at refresh code %u",
                s_fw_major, s_fw_minor, s_fw_patch, (unsigned)(cfg & MLX_REFRESH_MASK));
        s_refresh_code = (uint8_t)(cfg & MLX_REFRESH_MASK);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ret = mlx_write_config(MLX_REG_EE_REFRESH, (uint16_t)((cfg & ~(uint16_t)MLX_REFRESH_MASK) | code));
    if (ret != ESP_OK) {
        return ret;
    }

    // Poll the value back rather than a flat 500 ms sleep: the write only lands once the measurement in flight ends, and the old rate stays live
    for (uint32_t waited = 0; waited < MLX_CONFIG_APPLY_MS; waited += MLX_CONFIG_POLL_MS) {
        mlx_bus_yield(MLX_CONFIG_POLL_MS);
        if (mlx_read_word(MLX_REG_EE_REFRESH, &cfg) == ESP_OK &&
                (cfg & MLX_REFRESH_MASK) == code) {
            s_refresh_code = code;
#ifdef POLYCAST5_DEBUG
            ESP_LOGI(TAG, "Refresh code now %u (%u ms frames) after %lu ms",
                    (unsigned)code, mlx90642_frame_period_ms(),
                    (unsigned long)(waited + MLX_CONFIG_POLL_MS));
#endif
            return ESP_OK;
        }
    }

    // Never confirmed. Keep the frame period honest by reporting what it last read back.
    s_refresh_code = (uint8_t)(cfg & MLX_REFRESH_MASK);
    ESP_LOGE(TAG, "Refresh code %u never took effect (still %u)",
            (unsigned)code, (unsigned)s_refresh_code);
    return ESP_ERR_TIMEOUT;
}

esp_err_t mlx90642_frame_ready(bool *ready)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t flags = 0;
    esp_err_t ret = mlx_read_word(MLX_REG_FLAGS, &flags);
    if (ret != ESP_OK) {
        return ret;
    }

    // READY plus frame-update-finished: reading mid-update is allowed but can straddle two frames, which shows as tearing
    if (ready) {
        *ready = (flags & MLX_FLAG_READY) && !(flags & MLX_FLAG_FRAME_UPD);
    }
    return ESP_OK;
}

esp_err_t mlx90642_read_frame(int16_t *pixels, int16_t *t_die_c100)
{
    if (s_dev == NULL || s_rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // One transaction for the whole frame, never split: reading from the frame base is what clears READY, so a
    // failure partway through would strand half a frame with no way to re-arm the flag
    esp_err_t ret = mlx_read(MLX_REG_FRAME_BASE, s_rx, MLX_FRAME_BYTES, I2C_FRAME_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    // Each word arrives MSByte first, two's complement, 0.02 C per LSB
    for (int i = 0; i < MLX90642_PIXELS; ++i) {
        pixels[i] = (int16_t)(((uint16_t)s_rx[i * 2] << 8) | s_rx[i * 2 + 1]);
    }

    // The word straight after the last pixel is the die temperature, 0.01 C per LSB
    if (t_die_c100) {
        *t_die_c100 = (int16_t)(((uint16_t)s_rx[MLX90642_PIXELS * 2] << 8) |
                                s_rx[MLX90642_PIXELS * 2 + 1]);
    }

    return ESP_OK;
}

esp_err_t mlx90642_park_cold(uint32_t scl_hz)
{
    // Not mlx90642_init(): that unconditionally sends the wake byte, turning a 2 uA part back into a 28 mA one every
    // time the device sleeps. Nothing here reads the part, so no identity probe and no firmware check - the one
    // command sent is the state we want it in, which is also what makes the call idempotent. An absent module NACKs
    if (s_dev != NULL) { // A session owns the handle; use the ordinary path
        return mlx90642_sleep();
    }

    if (i2c_bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MLX90642_I2C_ADDR,
        .scl_speed_hz = scl_hz,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &dev);
    if (ret != ESP_OK) {
        return ret;
    }

    // Borrow the handle for the single write, then hand it straight back; s_dev is left NULL so mlx90642_is_present() keeps meaning "a session owns it"
    s_dev = dev;
    ret = mlx_write_reg(MLX_REG_CMD, MLX_CMD_SLEEP);
    s_dev = NULL;

    i2c_master_bus_rm_device(dev);
    return ret;
}

esp_err_t mlx90642_sleep(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return mlx_write_reg(MLX_REG_CMD, MLX_CMD_SLEEP);
}

esp_err_t mlx90642_wake(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Wake is a bare byte with no register address, unlike every other command here
    uint8_t tx = MLX_WAKE_BYTE;
    return i2c_master_transmit(s_dev, &tx, 1, I2C_TIMEOUT_MS);
}
