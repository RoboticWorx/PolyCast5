#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/ledc.h"

#include "polycast5_macros.h"

#define LCD_BL_STATE_ON 0 // Active low
#define LCD_BL_STATE_OFF 1 // Active low

#define LCD_LEDC_RESOLUTION LEDC_TIMER_10_BIT // 0-1023
#define LCD_LEDC_FREQ_HZ 5000 // 5kHz PWM
#define LCD_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_LEDC_TIMER LEDC_TIMER_0

// Remote (Screen Mirror) button injection, in ms. POLL_MS in gpio_task.c is 20
#define GPIO_REMOTE_MIN_HOLD_MS 70 // >= 3 polls on the 10ms tick: guarantees one short press
#define GPIO_REMOTE_TAP_HOLD_MS 80 // A single click from the web, and every queued one
#define GPIO_REMOTE_HOLD_MAX_MS 1200 // Watchdog; the browser re-asserts every 400ms
#define GPIO_REMOTE_GAP_MS 40 // Released between two presses of one pin: 2 polls
#define GPIO_REMOTE_QUEUE_MAX 8 // Presses queued across all pins, at most 8; more are dropped

#define HAPTIC_MAX_MS 50
#define HAPTIC_MIN_MS 10

#define RGB_PERIOD_MAX_MS 50
#define RGB_PERIOD_MIN_MS 0
#define RGB_TOTAL_MAX_MS 500
#define RGB_TOTAL_MIN_MS 40

// Battery under-voltage cutoff, in volts as measured through the ADC front end
#define BATT_CUTOFF_VBAT      3.35f // Drop 3V3_EN below this
#define BATT_CUTOFF_RECOVER   3.40f // Re-assert 3V3_EN at or above this
#define BATT_CUTOFF_MIN_VALID 2.15f // Below this the reading is failed/implausible (the conversion floor is ~2.00V)

// RGB LED states
enum {
    RGB_SET_OFF,
    
    RGB_SET_RED,
    RGB_SET_GREEN,
    RGB_SET_BLUE,
    RGB_SET_PURPLE, // B + R
    RGB_SET_TEAL, // B + G
    
    RGB_BLINK_RED,
    RGB_BLINK_GREEN,
    RGB_BLINK_BLUE,
    RGB_BLINK_PURPLE, // B + R
    RGB_BLINK_TEAL, // B + G
};

/** 
 * @brief Initialise NVS flash
 */
void gpio_utils_init_nvs(void);

/**
 * @brief Initialize the TCA9535 I2C expander and configure:
 *          - Port 0 = all inputs
 *          - Port 1 = all outputs
 *
 * @return ESP_OK on success
 */
esp_err_t gpio_utils_init(void);

/**
 * @brief Read one pin on Port 0 (0…7)
 *
 * @param [in] pin Pin index (0…7)
 *
 * @return 0 or 1 state, or –1 if invalid pin
 */
int gpio_utils_read_input(uint8_t pin);

/**
 * @brief Read every TCA9535 port-0 input in one transaction
 *
 *        Preferred over repeated gpio_utils_read_input() calls when more than one pin is
 *        wanted: it takes the shared I2C bus once instead of once per pin.
 *
 * @param [out] inputs  Raw port-0 byte; set to 0xFF (all released) if the read fails
 *
 * @return ESP_OK, or the I2C error (inputs is still written with the released default)
 */
esp_err_t gpio_utils_read_inputs(uint8_t *inputs);

/**
 * @brief Press or release a user button from a remote (Screen Mirror) session
 *
 *        The pin is merged into every gpio_utils_read_inputs() result, so gpio_task derives
 *        the same short press, long press, auto-repeat, held-state globals and haptics as
 *        a real press. A DOWN while that pin's press has had no UP only extends it. Any other
 *        DOWN is queued as gpio_utils_remote_button_tap() is, so a DOWN after an UP never
 *        merges into that press.
 *
 * @param [in] pin     A TCA9535_USER_BUTTON_*_PIN. POWER and the charge indicator are
 *                     rejected: sleep polls POWER through this same overlay
 * @param [in] down    true = press (drive the input low), false = release
 * @param [in] hold_ms On a press, the watchdog, which a re-assert only ever extends
 *                     (0 = GPIO_REMOTE_HOLD_MAX_MS; a queued press always gets that).
 *                     On a release, the minimum hold owed from the press start, so a
 *                     click shorter than the 20ms poll is not lost (0 = GPIO_REMOTE_MIN_HOLD_MS)
 */
void gpio_utils_remote_button_set(uint8_t pin, bool down, uint32_t hold_ms);

/**
 * @brief One complete click of a user button from a remote (Screen Mirror) session
 *
 *        Queued, up to GPIO_REMOTE_QUEUE_MAX across all pins, while that pin is held or in
 *        its GPIO_REMOTE_GAP_MS release, or an earlier press is queued, or another pin's click
 *        is still held or its release not yet polled. Never merged, so bunched clicks reach
 *        gpio_task as separate presses, in arrival order, one release per poll. Another pin's
 *        open hold (no UP yet) never delays it.
 *
 * @param [in] pin     As gpio_utils_remote_button_set()
 * @param [in] hold_ms How long the click holds the pin (0 = GPIO_REMOTE_TAP_HOLD_MS).
 *                     A queued click always holds GPIO_REMOTE_TAP_HOLD_MS
 */
void gpio_utils_remote_button_tap(uint8_t pin, uint32_t hold_ms);

/**
 * @brief Release every remotely held button and drop every queued one (session ended or link lost)
 */
void gpio_utils_remote_buttons_clear(void);

/**
 * @brief Drive one pin on Port 1 (0…7)
 *
 * @param [in] pin Pin index (0…7)
 * @param [in] level true = high, false = low
 *
 * @return ESP_OK on success
 */
esp_err_t gpio_utils_write_output(uint8_t pin, bool level);

/**
 * @brief As gpio_utils_write_output(), but fails instead of waiting for the I2C bus
 *
 *        For callers that must never block - notably FreeRTOS timer callbacks, which stall
 *        every other software timer if they wait on the bus behind a thermal frame read.
 *
 * @param [in] pin    Output pin on TCA9535 port 1 (0-7)
 * @param [in] level  Desired level
 *
 * @return ESP_OK, ESP_ERR_TIMEOUT if the bus was busy, or an I2C error
 */
esp_err_t gpio_utils_write_output_nb(uint8_t pin, bool level);

/** 
 * @brief Cycle through the RGB LED to make sure it is working
 */
void gpio_utils_cycle_rgb(void);

/** 
 * @brief Initalize battery ADC
 */
void gpio_utils_init_battery_adc(void);

/** 
 * @brief De-initalize battery ADC to save power
 */
void gpio_utils_deinit_battery_adc(void);

/** 
 * @brief Enable or disable the TSOP infrared receiver.
 *        This must be enabled in order to receive IR signals, but is turned off to save power otherwise
 *
 * @param [in] enable true = enable, false = disable
 */
void gpio_utils_en_tsop_receiver(bool enable);

/** 
 * @brief Get the raw battery voltage with software averaging
 *
 * @return The value in volts
 */
float gpio_utils_get_battery_voltage(void);

/**
 * @brief Quick battery voltage reading (64 samples) for the boot hardware self-test.
 *        Inits and deinits the ADC internally; must only be called before the
 *        gpio/adc tasks are created (the ADC handles are unguarded)
 *
 * @return The value in volts, or 0.0f if the ADC read/calibration failed
 */
float gpio_utils_battery_selftest_voltage(void);

/** 
 * @brief Convert the raw voltage to a state-of-charge percentage 1-100 based on a typical LiPo discharge curve
 *
 * @param [in] voltage The voltage value to convert
 *
 * @return The value in percent
 */
uint8_t gpio_utils_volts_to_soc(float voltage);

/** 
 * @brief Spins the haptic motor for a given duration
 *
 * @param [in] ms Time on in milliseconds
 */
void gpio_utils_spin_haptic(uint32_t ms);

/** 
 * @brief Indicate HW state via the built-in RGB LED
 *
 * @param [in] rgb_data The state to indicate
 */
void gpio_utils_rgb_indicate(uint8_t rgb_data);

#endif // GPIO_FUNCS_H
