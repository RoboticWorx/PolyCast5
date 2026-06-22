/*
 * MMC5603NJ 3-axis magnetometer driver (I2C).
 *
 * Layers, lowest to highest:
 *   mmc_read / mmc_write_reg          - raw I2C register access (caller holds the bus mutex)
 *   mmc5603_read_raw                  - output registers -> three unsigned 20-bit counts
 *   mmc5603_read_ut                   - counts -> microtesla (zero-field removed, signed)
 *   mmc5603_read_heading              - flat, uncalibrated magnetic heading (atan2 of X/Y)
 *
 * NOTE: The chip is mounted rotated 180 degrees on the physical PCB. mmc5603_read_ut negates the
 *       in-plane X/Y axes so every caller (heading, calibration) works in the true board frame.
 */

#include "mmc5603.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "TCA9535.h" // i2c_bus_handle, I2C_MASTER_FREQ_HZ
#include "polycast5_macros.h" // POLYCAST5_DEBUG
#include "gpio_task.h" // xI2CBusMutex

#define TAG "MMC5603"

#define I2C_TIMEOUT_MS 100 // Per-transaction I2C timeout

#define RAD_TO_DEG 57.295779513082320876f // 180 / pi

/* Register map (datasheet rev. B) */
#define MMC5603_REG_XOUT0      0x00 // First of 9 consecutive output bytes
#define MMC5603_REG_STATUS1    0x18
#define MMC5603_REG_ODR        0x1A
#define MMC5603_REG_CTRL0      0x1B
#define MMC5603_REG_CTRL1      0x1C
#define MMC5603_REG_CTRL2      0x1D
#define MMC5603_REG_PRODUCT_ID 0x39

#define MMC5603_PRODUCT_ID     0x10 // Value returned by the Product ID register

/* Internal Control 0 (0x1B) */
#define MMC5603_CTRL0_AUTO_SR_EN  (1 << 5) // Automatic set/reset per measurement
#define MMC5603_CTRL0_CMM_FREQ_EN (1 << 7) // Calculate continuous-mode period from ODR

/* Internal Control 1 (0x1C) */
#define MMC5603_CTRL1_SW_RESET    (1 << 7) // Software reset (clears regs, re-reads OTP)
// Bandwidth bits [1:0] = 00 -> 6.6 ms measurement time, lowest noise (1.5 mG)

/* Internal Control 2 (0x1D) */
#define MMC5603_CTRL2_PRD_SET_100 (3 << 0) // Prd_set = 011 -> one SET every 100 samples
#define MMC5603_CTRL2_EN_PRD_SET  (1 << 3) // Enable periodic set
#define MMC5603_CTRL2_CMM_EN      (1 << 4) // Enter continuous mode

#define MMC5603_ODR_HZ            50 // Continuous-mode output data rate (<=75 for BW=00 + auto SR)

// 20-bit unsigned output: zero field sits at mid-scale (2^19)
#define MMC5603_NULL_FIELD_OFFSET 524288.0f
// Counts -> uT. Datasheet headline 0.0625 mG/LSB = 0.00625 uT/LSB
// The spec table's 16384 counts/G instead implies ~0.0061 uT/LSB; the ~2.4% gap is within the +-5% sensitivity
#define MMC5603_UT_PER_LSB        0.00625f

// Per-device handle on the shared bus; stays NULL until init succeeds, which the read
// functions use as a "not ready" guard.
static i2c_master_dev_handle_t s_dev = NULL;

// Write one register. On the wire: START, addr+W, register number, value, STOP.
// Caller must hold xI2CBusMutex.
static esp_err_t mmc_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { reg, val }; // byte 0 = target register, byte 1 = value
    return i2c_master_transmit(s_dev, tx, sizeof(tx), I2C_TIMEOUT_MS);
}

// Read len bytes starting at reg: write the register pointer, then repeated-START and read.
// The chip auto-increments its internal address pointer, so one multi-byte read walks
// consecutive registers (we use this to grab all nine output registers at once).
// Caller must hold xI2CBusMutex.
static esp_err_t mmc_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

esp_err_t mmc5603_init(void)
{
    // The shared bus must already exist
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Register the MMC5603 as a device on the shared I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MMC5603_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Software reset to a known state. The mutex is released around the 20 ms power-on wait so
    // the shared bus isn't blocked while the chip reboots (it re-reads its OTP during this time).
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    ret = mmc_write_reg(MMC5603_REG_CTRL1, MMC5603_CTRL1_SW_RESET);
    xSemaphoreGive(xI2CBusMutex);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "software reset failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(s_dev); // undo the add_device above so we don't leak a handle
        s_dev = NULL;
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    // Hold the bus for the rest of init: probe the ID and write the whole config as one unit.
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);

    // WHO_AM_I check: read the Product ID register and confirm it matches the MMC5603.
    uint8_t id = 0;
    ret = mmc_read(MMC5603_REG_PRODUCT_ID, &id, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Product ID read failed: %s", esp_err_to_name(ret));
        goto unlock_fail;
    }
    if (id != MMC5603_PRODUCT_ID) {
        ESP_LOGE(TAG, "Product ID mismatch: got 0x%02X, expected 0x%02X", id, MMC5603_PRODUCT_ID);
        ret = ESP_ERR_NOT_FOUND;
        goto unlock_fail;
    }

    // Bring up continuous mode with offset compensation. Order matters (datasheet): set the
    // bandwidth and ODR first, then Cmm_freq_en (latches the measurement period from the ODR),
    // then Cmm_en (actually starts free-running). The writes are chained with || so each only
    // runs if the previous one ACKed; the first failure short-circuits and leaves ret set.
    //  - CTRL1 = 0x00: bandwidth 00 (6.6 ms, lowest noise), all axes enabled
    //  - ODR   = 50 Hz measurement rate
    //  - CTRL0: automatic set/reset + trigger internal period calculation (Cmm_freq_en)
    //  - CTRL2: start continuous mode (Cmm_en) + periodic SET every 100 samples (clears drift)
    if ((ret = mmc_write_reg(MMC5603_REG_CTRL1, 0x00)) != ESP_OK ||
            (ret = mmc_write_reg(MMC5603_REG_ODR, MMC5603_ODR_HZ)) != ESP_OK ||
            (ret = mmc_write_reg(MMC5603_REG_CTRL0,
                    MMC5603_CTRL0_AUTO_SR_EN | MMC5603_CTRL0_CMM_FREQ_EN)) != ESP_OK ||
            (ret = mmc_write_reg(MMC5603_REG_CTRL2,
                    MMC5603_CTRL2_CMM_EN | MMC5603_CTRL2_EN_PRD_SET | MMC5603_CTRL2_PRD_SET_100)) != ESP_OK) {
        ESP_LOGE(TAG, "configuration write failed: %s", esp_err_to_name(ret));
        goto unlock_fail;
    }

    xSemaphoreGive(xI2CBusMutex); // Success: release the bus, s_dev stays valid

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "MMC5603 initialized (continuous %d Hz, auto set/reset)", MMC5603_ODR_HZ);
#endif
    return ESP_OK;

unlock_fail:
    // Shared exit for any failure after the bus was locked: release it, then tear the device
    // back down so the read functions' "s_dev == NULL" guard reports not-initialised.
    xSemaphoreGive(xI2CBusMutex);
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ret;
}

esp_err_t mmc5603_read_raw(uint32_t *x, uint32_t *y, uint32_t *z)
{
    if (s_dev == NULL) { // Not initialized (or init failed)
        return ESP_ERR_INVALID_STATE;
    }

    // Grab all nine output registers (0x00..0x08) in one auto-incrementing read, under the lock.
    uint8_t buf[9];
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    esp_err_t ret = mmc_read(MMC5603_REG_XOUT0, buf, sizeof(buf));
    xSemaphoreGive(xI2CBusMutex);
    if (ret != ESP_OK) { // I2C read failed
        return ret;
    }

    // Reassemble each axis from its three bytes. The nine bytes are laid out as
    //   buf = [X0 X1 Y0 Y1 Z0 Z1 X2 Y2 Z2]
    // where out0 = bits[19:12], out1 = bits[11:4], and out2 holds bits[3:0] in its UPPER nibble
    // (hence >> 4). Combined into an unsigned 20-bit count (0..0xFFFFF); the cast to uint32_t
    // before shifting keeps the high bits from being lost.
    if (x) *x = ((uint32_t)buf[0] << 12) | ((uint32_t)buf[1] << 4) | ((uint32_t)buf[6] >> 4);
    if (y) *y = ((uint32_t)buf[2] << 12) | ((uint32_t)buf[3] << 4) | ((uint32_t)buf[7] >> 4);
    if (z) *z = ((uint32_t)buf[4] << 12) | ((uint32_t)buf[5] << 4) | ((uint32_t)buf[8] >> 4);

    return ESP_OK;
}

esp_err_t mmc5603_read_ut(float *x, float *y, float *z)
{
    uint32_t rx, ry, rz;
    esp_err_t ret = mmc5603_read_raw(&rx, &ry, &rz);
    if (ret != ESP_OK) {
        return ret;
    }

    // Counts are unsigned with zero field at mid-scale (2^19)
    // Subtract that bias to get a signed deviation, then scale counts -> microtesla
    // (negative result = field along -axis)
    
    // The chip sits rotated 180 deg in the PCB plane:
    // a turn about the board normal negates the in-plane X and Y axes (Z is unchanged)
    // Correct it here so every downstream consumer reads the field in the true board frame.
    if (x) *x = -(((float)rx - MMC5603_NULL_FIELD_OFFSET) * MMC5603_UT_PER_LSB);
    if (y) *y = -(((float)ry - MMC5603_NULL_FIELD_OFFSET) * MMC5603_UT_PER_LSB);
    if (z) *z =  ((float)rz - MMC5603_NULL_FIELD_OFFSET) * MMC5603_UT_PER_LSB;

    return ESP_OK;
}

esp_err_t mmc5603_read_heading(float *heading_deg)
{
    if (heading_deg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    float x, y;
    esp_err_t ret = mmc5603_read_ut(&x, &y, NULL); // Z not needed for a flat heading
    if (ret != ESP_OK) {
        return ret;
    }

    // Simple heading: angle of the X/Y field vector
    // Assumes the board is held flat and ignores hard-iron offset and declination
    // magcel_page compass does its own calibrated heading instead
    float h = atan2f(y, x) * RAD_TO_DEG; // atan2 returns (-180,180];

    // Fold the negative half up so the result is [0,360).
    if (h < 0.0f) {
        h += 360.0f;
    }
    *heading_deg = h;

    return ESP_OK;
}
