#ifndef IR_EXP_H
#define IR_EXP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Low-level drivers for the infrared expansion module: three I2C devices sharing the board bus
// with the TCA9535 expander.
//   MLX90642ESF-BCB  0x66  32x24 thermal array, 45 x 35 deg FOV  - the live image
//   VL53L4CX         0x29  Time-of-Flight rangefinder            - tape measure
//   MLX90632SLD-DCB  0x3A  medical IR thermometer, 50 deg FOV    - spot temperature

// I2C 7-bit addresses
#define MLX90642_I2C_ADDR 0x66
#define MLX90632_I2C_ADDR 0x3A
#define VL53L4CX_I2C_ADDR 0x29

// Thermal array geometry
#define MLX90642_COLS   32
#define MLX90642_ROWS   24
#define MLX90642_PIXELS (MLX90642_COLS * MLX90642_ROWS)

// Reported (-273.16 C) for every pixel until the array's first valid frame, and for any pixel it
// could not resolve - defective ones included, since its own correction needs firmware 1.18.0.
// Filter before the auto-range min/max, or one such pixel collapses the whole palette span.
#define MLX90642_INVALID_RAW ((int16_t)0xCAA6)

// Returned by vl53l4cx_read_mm() when no reading passed the validity gate
#define VL53L4CX_NO_TARGET (-1)

// Refresh-rate codes for bits 2:0 of the array's EEPROM word 0x11F0
#define MLX90642_REFRESH_2HZ  0x02
#define MLX90642_REFRESH_4HZ  0x03
#define MLX90642_REFRESH_8HZ  0x04 // Factory default
#define MLX90642_REFRESH_16HZ 0x05

/* =============== MLX90642 =============== */

/**
 * @brief Add the thermal array to the shared I2C bus and confirm it responds.
 *        Wakes the device and verifies its firmware-version word.
 *
 * @param [in] scl_hz  Bus clock for this device handle (400000 preferred, 100000 fallback)
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if the ID read is implausible,
 *         ESP_ERR_INVALID_STATE if the shared bus is not up, or an I2C error
 */
esp_err_t mlx90642_init(uint32_t scl_hz);

/**
 * @brief Remove the device handle from the bus (does not change the sensor's power state)
 */
void mlx90642_deinit(void);

/**
 * @brief Whether init() succeeded and the handle is live
 */
bool mlx90642_is_present(void);

/**
 * @brief Force the sensor's refresh rate, in both directions
 *
 *        The 100 kHz bus cannot carry 8 Hz (a full frame read takes ~139 ms) so that path drops to
 *        4 Hz; the rate lives in EEPROM and survives power cycles, so the 400 kHz path has to put
 *        it back. Writes EEPROM only when the rate actually differs - the '642 allows 100k write
 *        cycles against the '632's ten. Releases and re-takes xI2CBusMutex around the readback
 *        poll: an in-flight measurement wins, and the part can take 500 ms to apply the change.
 *
 * @param [in] code  One of the MLX90642_REFRESH_* codes
 *
 * @return ESP_OK once the sensor reports the requested rate, ESP_ERR_NOT_SUPPORTED if
 *         the firmware predates the configuration command (1.16.5), ESP_ERR_TIMEOUT if
 *         the readback never agreed, or an I2C error
 */
esp_err_t mlx90642_set_refresh(uint8_t code);

/**
 * @brief Frame period in ms implied by the refresh rate the sensor is actually running,
 *        cached at init and updated by mlx90642_set_refresh().
 *
 *        Ask the sensor rather than inferring the rate from the bus clock: the two disagree
 *        whenever a rate change was refused or the part came up with non-default EEPROM.
 *
 * @return 125 at 8 Hz, 250 at 4 Hz, etc. Falls back to 125 if the rate is unknown.
 */
uint16_t mlx90642_frame_period_ms(void);

/**
 * @brief Poll the status flags for a completed, non-updating frame.
 *
 * @param [out] ready  true when READY is set and a frame update is not in progress
 *
 * @return ESP_OK on success, or an I2C error
 */
esp_err_t mlx90642_frame_ready(bool *ready);

/**
 * @brief Read the whole frame in one transaction - 769 words, the 768 pixels plus the die
 *        temperature after them. Reading from the frame base clears READY, so never chunk this.
 *
 * @param [out] pixels      MLX90642_PIXELS entries, 0.02 C per LSB, row-major
 * @param [out] t_die_c100  Die temperature in 0.01 C (may be NULL). NOT ambient -
 *                          typically runs 8-10 C above it.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized, or an I2C error
 */
esp_err_t mlx90642_read_frame(int16_t *pixels, int16_t *t_die_c100);

/**
 * @brief Sleep the array without initialising it, for use when no session owns it
 *
 *        The array powers on running (continuous is the factory EEPROM default) and sleep is its
 *        only low-power state, so a module whose page was never opened draws ~28 mA. Sends only
 *        the sleep command: never wakes the part, never touches EEPROM, safe when absent.
 *
 * @param [in] scl_hz  Bus speed for the temporary device handle
 *
 * @return ESP_OK if the command was acknowledged, an I2C error otherwise (absent module)
 */
esp_err_t mlx90642_park_cold(uint32_t scl_hz);

/**
 * @brief Put the array into its ~2 uA sleep state (it draws ~28 mA running)
 */
esp_err_t mlx90642_sleep(void);

/**
 * @brief Wake the array from sleep. First valid data follows ~205 ms later, and full
 *        absolute accuracy needs up to 180 s of thermal stabilization.
 */
esp_err_t mlx90642_wake(void);

/* =============== MLX90632 =============== */

/**
 * @brief Add the medical IR thermometer to the bus, log its product code, cache its
 *        factory calibration constants and force continuous measurement mode.
 *
 *        The product code is logged, not gated on: its field layout is ambiguous enough that a
 *        strict match risks rejecting a good part. Presence comes from the read ACKing at all and
 *        from plausible calibration constants, so a silent part is still caught.
 *
 * @param [in] scl_hz  Bus clock for this device handle
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_RESPONSE if the calibration constants read back
 *         implausibly, ESP_ERR_INVALID_STATE if the shared bus is not up, or an I2C error
 */
esp_err_t mlx90632_init(uint32_t scl_hz);

/**
 * @brief Remove the device handle from the bus
 */
void mlx90632_deinit(void);

/**
 * @brief Whether init() succeeded and the calibration constants are cached
 */
bool mlx90632_is_present(void);

/**
 * @brief Read a new object temperature if one is ready. Medical mode interleaves two measurement
 *        cycles, so a pair takes about a second; returns ESP_ERR_NOT_FINISHED until both land.
 *
 * @param [out] t_obj_c  Object temperature in C (may be NULL)
 * @param [out] t_amb_c  Sensor ambient temperature in C (may be NULL)
 *
 * @return ESP_OK when a fresh result was produced, ESP_ERR_NOT_FINISHED while waiting
 *         on new data or the second cycle, ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t mlx90632_read_object(float *t_obj_c, float *t_amb_c);

/**
 * @brief Drop the spot sensor to sleeping-step without initialising it
 *
 *        REG_CONTROL loads from EEPROM at power-on and continuous (~1 mA) is the factory default,
 *        so an untouched part draws it indefinitely. Unlike mlx90632_init() this never forces
 *        continuous mode on the way in, and is safe to call when the module is absent.
 *
 * @param [in] scl_hz  Bus speed for the temporary device handle
 *
 * @return ESP_OK if the write was acknowledged, an I2C error otherwise (absent module)
 */
esp_err_t mlx90632_park_cold(uint32_t scl_hz);

/**
 * @brief Switch between continuous measurement and the low-power sleeping-step mode.
 *        Writes REG_CONTROL only - never EEPROM, which is rated for just 10 re-writes.
 *
 * @param [in] on  true for continuous, false for sleeping-step
 */
esp_err_t mlx90632_set_continuous(bool on);

/* =============== VL53L4CX =============== */

/**
 * @brief Add the rangefinder to the bus, wait for its firmware to boot and load the default
 *        configuration. Ranging is left stopped - call vl53l4cx_start().
 *
 *        XSHUT is tied off on this module, so there is no hardware reset. A boot poll that times
 *        out gets a SYSTEM__SOFT_RESET and one more poll; that is the only recovery available,
 *        since the 3V3 rail is latched on and never cycled by the firmware.
 *
 * @param [in] scl_hz  Bus clock for this device handle
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if the firmware never reports ready even after the
 *         soft reset, ESP_ERR_INVALID_RESPONSE if ST's init or configuration is refused,
 *         ESP_ERR_NO_MEM if the driver state could not be allocated, or an I2C error from adding
 *         the device
 */
esp_err_t vl53l4cx_init(uint32_t scl_hz);

/**
 * @brief Remove the device handle from the bus
 */
void vl53l4cx_deinit(void);

/**
 * @brief Whether init() succeeded and the handle is live
 */
bool vl53l4cx_is_present(void);

/**
 * @brief Halt ranging without initialising the part, for a teardown that was refused
 *
 *        A stop the part did not acknowledge still costs the handle, leaving it ranging at
 *        19-21 mA unreachable until the page is next opened. Issues only the two raw writes that
 *        force a known state - no firmware boot, no DataInit - so a later full init still works.
 *
 * @param [in] scl_hz  Bus speed for the temporary device handle
 *
 * @return ESP_OK if the halt was acknowledged, an I2C error otherwise (absent module)
 */
esp_err_t vl53l4cx_park_cold(uint32_t scl_hz);

/**
 * @brief Begin continuous back-to-back ranging
 */
esp_err_t vl53l4cx_start(void);

/**
 * @brief Stop ranging, dropping the part from ~20 mA of active ranging into SW STANDBY at 4-9 uA.
 *        XSHUT (HW STANDBY, 3-7 uA) is not routed on this module and would save nothing over this.
 */
esp_err_t vl53l4cx_stop(void);

/**
 * @brief Read the latest range if one is ready and passes the validity gate
 *        (status, SPAD count, sigma and a trusted distance window).
 *
 * @param [out] dist_mm  Offset-corrected distance in mm, or VL53L4CX_NO_TARGET
 *                       when the reading failed the gate
 *
 * @return ESP_OK when a new measurement was consumed (valid or not),
 *         ESP_ERR_NOT_FINISHED when no measurement was ready,
 *         ESP_ERR_INVALID_STATE if not initialized, or an I2C error
 */
esp_err_t vl53l4cx_read_mm(int32_t *dist_mm);

/**
 * @brief Stop then restart ranging, which re-runs the sensor's internal
 *        self-calibration. Used to cancel the 1.3 mm/C thermal offset drift.
 */
esp_err_t vl53l4cx_recalibrate(void);

/* =============== Detail view =============== */

// Everything the rangefinder reports beyond the single distance the tape measure shows. Already
// computed for every measurement and free of bus traffic: VL53LX_GetAdditionalData() is a memcpy
// out of the driver's own RAM.

#define IR_EXP_TOF_MAX_TARGETS 4  // VL53LX_MAX_RANGE_RESULTS
#define IR_EXP_TOF_HIST_BINS  24  // VL53LX_HISTOGRAM_BUFFER_SIZE

typedef struct {
    int16_t  dist_mm;      // Range as reported, BEFORE the trusted-window gate
    int16_t  min_mm;       // Lower bound of the part's own confidence interval
    int16_t  max_mm;       // Upper bound; max-min is the error bar to draw
    uint16_t sigma_mm_q8;  // Noise estimate, 8.8 fixed point millimetres
    uint32_t signal_kcps;  // Return rate for this target
    uint32_t ambient_kcps; // Background rate seen alongside it
    uint8_t  status;       // VL53LX_RANGESTATUS_*; 0 is the only trustworthy one
} ir_exp_tof_target_t;

typedef struct {
    uint8_t  n_targets;    // 0..IR_EXP_TOF_MAX_TARGETS
    int8_t   chosen;       // Index the tape measure picked, or -1 if it reported nothing
    uint8_t  ambient_bins; // Leading bins that hold ambient only, no return
    uint8_t  merge_nb;     // How many frames the histogram has been summed over (1..6)
    uint16_t spads;        // Effective SPADs in use, whole count
    uint16_t zero_phase;   // Phase at zero range - the origin for the bin/mm mapping
    uint32_t pll_mm_q15;   // Millimetres per phase LSB, 17.15 fixed point
    int32_t  ambient_lvl;  // Per-bin ambient floor; signal is the area above this
    int32_t  bin_max;      // Largest bin this frame, for scaling the plot
    int32_t  bin[IR_EXP_TOF_HIST_BINS]; // The raw time-of-flight histogram
    ir_exp_tof_target_t target[IR_EXP_TOF_MAX_TARGETS];
} ir_exp_tof_detail_t;

/**
 * @brief Snapshot every target and the raw histogram from the last measurement
 *
 *        Call straight after a successful vl53l4cx_read_mm(), while the driver's result block still
 *        holds that frame. Costs no I2C, but call it from the task that holds xI2CBusMutex anyway
 *        to stay clear of ST's unlocked driver state.
 *
 * @param [out] out  Filled in on success
 */
esp_err_t vl53l4cx_get_detail(ir_exp_tof_detail_t *out);

#endif // IR_EXP_H
