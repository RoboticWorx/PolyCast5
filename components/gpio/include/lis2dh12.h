#ifndef LIS2DH12_H
#define LIS2DH12_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// I2C 7-bit address with SA0 tied to VDD
#define LIS2DH12_I2C_ADDR 0x19

typedef struct accel_deg_t {
    float pitch;
    float roll;
} accel_deg_t;

/**
 * @brief Probe the LIS2DH12 and configure it for 100 Hz / normal mode / +-2 g.
 *        Must be called after the shared I2C bus is up (i.e. after TCA9535Init).
 *        Takes xI2CBusMutex internally.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if WHO_AM_I mismatch, or I2C error.
 */
esp_err_t lis2dh12_init(void);

/**
 * @brief Whether boot-time init found the chip (WHO_AM_I matched and config stuck).
 *        Used by the boot hardware self-test.
 *
 * @return true if the accelerometer is present and initialized
 */
bool lis2dh12_is_present(void);

/**
 * @brief Read the latest X/Y/Z acceleration as raw signed 10-bit counts.
 *        In +-2 g normal mode, 1 count = 4 mg.
 *
 * @param [out] x  X axis count (-512..511)
 * @param [out] y  Y axis count
 * @param [out] z  Z axis count
 *
 * @return ESP_OK on success
 */
esp_err_t lis2dh12_read_raw(int16_t *x, int16_t *y, int16_t *z);

/**
 * @brief Read the latest X/Y/Z acceleration converted to g (gravity units).
 *
 * @param [out] x  X axis in g
 * @param [out] y  Y axis in g
 * @param [out] z  Z axis in g
 *
 * @return ESP_OK on success
 */
esp_err_t lis2dh12_read_g(float *x, float *y, float *z);

/**
 * @brief Bounded-latency variant of lis2dh12_read_g() for the LCD render path.
 *        Does not wait on the shared I2C bus (returns ESP_ERR_TIMEOUT if it is busy)
 *        and bounds the I2C transaction itself to I2C_TIMEOUT_FAST_MS, so neither a
 *        busy nor a wedged bus can stall lcd_task. Worst case is ~20 ms rather than the
 *        ~100 ms a task-context read may take. The caller keeps its last value on miss.
 *
 * @param [out] x  X axis in g
 * @param [out] y  Y axis in g
 * @param [out] z  Z axis in g
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if the bus was busy,
 *         ESP_ERR_INVALID_STATE if not initialized, ESP_FAIL on I2C error
 */
esp_err_t lis2dh12_read_g_nonblocking(float *x, float *y, float *z);

/**
 * @brief Read the board tilt as pitch/roll angles in degrees, derived from the
 *        gravity vector. Meaningful only while the device is roughly static
 *        (linear acceleration is indistinguishable from gravity).
 *
 * @param [out] pitch  Pitch in degrees (may be NULL)
 * @param [out] roll   Roll in degrees (may be NULL)
 *
 * @return ESP_OK on success
 */
esp_err_t lis2dh12_read_deg(float *pitch, float *roll);

/**
 * @brief Read the on-die temperature in degrees Celsius. This is a relative
 *        sensor: absolute accuracy is only +-several C and it reads warm due to
 *        board self-heating. Useful for tracking changes, not precise ambient.
 *
 * @param [out] temp_c  Temperature in C (may be NULL)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t lis2dh12_read_temp_c(float *temp_c);

#endif // LIS2DH12_H
