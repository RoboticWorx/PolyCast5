#ifndef MMC5603_H
#define MMC5603_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// I2C 7-bit address (factory default, three address LSBs = 000)
#define MMC5603_I2C_ADDR 0x30

// One magnetic-field sample (microtesla per axis), as posted to the LCD via queue
typedef struct {
    float x;
    float y;
    float z;
} mmc5603_reading_t;

/**
 * @brief Probe the MMC5603NJ and start it in continuous mode (compass-ready).
 *        Configures lowest-noise bandwidth, automatic set/reset and periodic
 *        set so readings are offset-compensated. Must be called after the
 *        shared I2C bus is up (i.e. after TCA9535Init). Takes xI2CBusMutex
 *        internally.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if the Product ID does not
 *         match, ESP_ERR_INVALID_STATE if the bus is not up, or an I2C error.
 */
esp_err_t mmc5603_init(void);

/**
 * @brief Whether boot-time init found the chip (Product ID matched and config stuck).
 *        Used by the boot hardware self-test.
 *
 * @return true if the magnetometer is present and initialized
 */
bool mmc5603_is_present(void);

/**
 * @brief Read the latest X/Y/Z field as raw 20-bit unsigned output counts.
 *        Zero field sits at mid-scale (2^19 = 524288 counts).
 *
 * @param [out] x  X axis counts (may be NULL)
 * @param [out] y  Y axis counts (may be NULL)
 * @param [out] z  Z axis counts (may be NULL)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t mmc5603_read_raw(uint32_t *x, uint32_t *y, uint32_t *z);

/**
 * @brief Read the latest X/Y/Z magnetic field converted to microtesla (uT).
 *
 * @param [out] x  X axis field in uT (may be NULL)
 * @param [out] y  Y axis field in uT (may be NULL)
 * @param [out] z  Z axis field in uT (may be NULL)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t mmc5603_read_ut(float *x, float *y, float *z);

/**
 * @brief Compute a compass heading from the horizontal (X/Y) field. This is an
 *        uncalibrated magnetic heading: no tilt compensation and no magnetic
 *        declination correction, so the absolute value depends on the board
 *        orientation, but it tracks rotation. Range [0, 360) degrees.
 *
 * @param [out] heading_deg  Heading in degrees (must not be NULL)
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized,
 *         ESP_ERR_INVALID_ARG if heading_deg is NULL
 */
esp_err_t mmc5603_read_heading(float *heading_deg);

#endif // MMC5603_H
