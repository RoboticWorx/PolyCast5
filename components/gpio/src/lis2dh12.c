/*
 * LIS2DH12 3-axis accelerometer driver (I2C).
 * 
 * NOTE: Chip is mounted with a 180-degree rotation on the physical PCB!
 */

#include "lis2dh12.h"
#include "lis2dh12_reg.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "TCA9535.h" // i2c_bus_handle, I2C_MASTER_FREQ_HZ
#include "polycast5_macros.h" // POLYCAST5_DEBUG
#include "gpio_task.h" // xI2CBusMutex

#define TAG "LIS2DH12"

#define LIS2DH12_AUTO_INC_BIT 0x80

#define I2C_TIMEOUT_MS 100 // Per-transaction I2C timeout

#define RAD_TO_DEG 57.295779513082320876f // 180 / pi

static i2c_master_dev_handle_t s_dev = NULL; // I2C device handle (NULL until init succeeds)
static stmdev_ctx_t s_ctx; // ST driver context

// Platform I2C callbacks used by the ST PID driver
static int32_t platform_write(void *handle, uint8_t reg, const uint8_t *buf, uint16_t len)
{
    // Auto-increment the register address when writing more than one byte
    if (len > 1) {
        reg |= LIS2DH12_AUTO_INC_BIT;
    }

    // Stack buffer
    uint8_t tx[8];
    if (len + 1 > sizeof(tx)) { // Refuse writes that would overflow the buffer
        return -1;
    }

    tx[0] = reg; // First byte is the (sub)register address
    memcpy(&tx[1], buf, len); // Remaining bytes are the payload
    return i2c_master_transmit((i2c_master_dev_handle_t)handle, tx, len + 1, I2C_TIMEOUT_MS);
}

static int32_t platform_read(void *handle, uint8_t reg, uint8_t *buf, uint16_t len)
{
    // Auto-increment the register address when reading more than one byte
    if (len > 1) {
        reg |= LIS2DH12_AUTO_INC_BIT;
    }

    // Write the register address then read len bytes back in a single transaction
    return i2c_master_transmit_receive((i2c_master_dev_handle_t)handle, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

// Probe and configure the accelerometer. Must run after the shared I2C bus is up.
esp_err_t lis2dh12_init(void)
{
    // The shared bus must already exist
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Register the LIS2DH12 as a device on the shared I2C bus
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LIS2DH12_I2C_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Point the ST driver context at platform I2C callbacks
    s_ctx.write_reg = platform_write;
    s_ctx.read_reg = platform_read;
    s_ctx.mdelay = NULL;
    s_ctx.handle = s_dev;

    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY); // Lock the I2C bus for the probe + config writes

    // Confirm talking to a LIS2DH12
    uint8_t who = 0;
    if (lis2dh12_device_id_get(&s_ctx, &who) != 0) {
        ESP_LOGE(TAG, "WHO_AM_I read failed");
        ret = ESP_FAIL;
        goto out;
    }
    if (who != LIS2DH12_ID) {
        ESP_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X, expected 0x%02X", who, LIS2DH12_ID);
        ret = ESP_ERR_NOT_FOUND;
        goto out;
    }

    // Configure: block-data-update on, +-2 g full scale, normal (10-bit) mode, 100 Hz ODR.
    // Also enable the on-die temperature sensor (ADC_EN + TEMP_EN); BDU above is required for it.
    if (lis2dh12_block_data_update_set(&s_ctx, PROPERTY_ENABLE) != 0 ||
            lis2dh12_full_scale_set(&s_ctx, LIS2DH12_2g) != 0 ||
            lis2dh12_operating_mode_set(&s_ctx, LIS2DH12_NM_10bit) != 0 ||
            lis2dh12_data_rate_set(&s_ctx, LIS2DH12_ODR_100Hz) != 0 ||
            lis2dh12_temperature_meas_set(&s_ctx, LIS2DH12_TEMP_ENABLE) != 0) {
        ESP_LOGE(TAG, "configuration write failed");
        ret = ESP_FAIL;
        goto out;
    }

#ifdef POLYCAST5_DEBUG
    ESP_LOGI(TAG, "LIS2DH12 initialized (100 Hz, normal mode, +-2 g)");
#endif

out:
    xSemaphoreGive(xI2CBusMutex); // Release the I2C bus
    if (ret != ESP_OK) { // Failed: remove the device so the read guards see s_dev == NULL
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    return ret;
}

// True once boot-time init found and configured the chip (used by the hardware self-test)
bool lis2dh12_is_present(void)
{
    return s_dev != NULL;
}

// The sensor is mounted rotated 180 deg on the PCB:
static inline void remap_axes_i16(int16_t *x, int16_t *y, int16_t *z)
{
    if (x) *x = -*x; // X => -X
    if (y) *y = -*y; // Y => -Y
    (void)z; // Z unchanged
}
static inline void remap_axes_f(float *x, float *y, float *z)
{
    if (x) *x = -*x; // X => -X
    if (y) *y = -*y; // Y => -Y
    (void)z; // Z unchanged
}

esp_err_t lis2dh12_read_raw(int16_t *x, int16_t *y, int16_t *z)
{
    if (s_dev == NULL) { // Not initialized (or init failed)
        return ESP_ERR_INVALID_STATE;
    }

    // Fetch the six output registers under the bus lock
    int16_t raw[3];
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    int32_t r = lis2dh12_acceleration_raw_get(&s_ctx, raw);
    xSemaphoreGive(xI2CBusMutex);
    if (r != 0) { // I2C read failed
        return ESP_FAIL;
    }
    
    // ST driver returns left-justified 16-bit; >>6 yields signed 10-bit normal-mode counts
    if (x) *x = raw[0] >> 6;
    if (y) *y = raw[1] >> 6;
    if (z) *z = raw[2] >> 6;

    remap_axes_i16(x, y, z);

    return ESP_OK;
}

esp_err_t lis2dh12_read_g(float *x, float *y, float *z)
{
    if (s_dev == NULL) { // Not initialized (or init failed)
        return ESP_ERR_INVALID_STATE;
    }

    // Fetch the six output registers under the bus lock
    int16_t raw[3];
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    int32_t r = lis2dh12_acceleration_raw_get(&s_ctx, raw);
    xSemaphoreGive(xI2CBusMutex);
    if (r != 0) { // I2C read failed
        return ESP_FAIL;
    }

    // Helper converts raw left-justified value to mg for +-2 g normal mode
    if (x) *x = lis2dh12_from_fs2_nm_to_mg(raw[0]) / 1000.0f; // mg -> g
    if (y) *y = lis2dh12_from_fs2_nm_to_mg(raw[1]) / 1000.0f;
    if (z) *z = lis2dh12_from_fs2_nm_to_mg(raw[2]) / 1000.0f;

    remap_axes_f(x, y, z);

    return ESP_OK;
}

esp_err_t lis2dh12_read_deg(float *pitch, float *roll)
{
    if (s_dev == NULL) { // Not initialized (or init failed)
        return ESP_ERR_INVALID_STATE;
    }

    // Fetch the six output registers under the bus lock
    int16_t raw[3];
    xSemaphoreTake(xI2CBusMutex, portMAX_DELAY);
    int32_t r = lis2dh12_acceleration_raw_get(&s_ctx, raw);
    xSemaphoreGive(xI2CBusMutex);
    if (r != 0) { // I2C read failed
        return ESP_FAIL;
    }

    float ax = (float)raw[0];
    float ay = (float)raw[1];
    float az = (float)raw[2];

    // pitch: tilt of the X axis; roll: rotation about X (from the Y/Z components)
    if (pitch) {
        *pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * RAD_TO_DEG;
    }
    if (roll) {
        *roll = atan2f(ay, az) * RAD_TO_DEG;
    }

    remap_axes_f(pitch, roll, NULL);

    return ESP_OK;
}

esp_err_t lis2dh12_read_temp_c(float *temp_c)
{
    if (s_dev == NULL) { // Not initialized (or init failed)
        return ESP_ERR_INVALID_STATE;
    }

    // Read the raw temperature under the bus lock
    int16_t raw;
    if (xSemaphoreTake(xI2CBusMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int32_t r = lis2dh12_temperature_raw_get(&s_ctx, &raw); // OUT_TEMP_L/H (0x0C/0x0D)
    xSemaphoreGive(xI2CBusMutex);
    if (r != 0) { // I2C read failed
        return ESP_FAIL;
    }

    // Chip is configured in normal (10-bit) mode -> normal-mode converter (1 C/LSB)
    if (temp_c) *temp_c = lis2dh12_from_lsb_nm_to_celsius(raw);

    return ESP_OK;
}
