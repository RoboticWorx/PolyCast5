#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/ledc.h"

#define SPI_MISO_PIN 2 // MISO for SX126x
#define SPI_MOSI_PIN 7 // SPI2 MOSI
#define SPI_SCLK_PIN 6 // SPI2 SCLK
#define ST7789_CS_PIN 1 // CS
#define ST7789_DC_PIN 8 // D/C
#define ST7789_RST_PIN 9 // RESET
#define ST7789_LEDA_PIN 3 // Backlight

#define USER_BUTTON_LEFT 3
#define USER_BUTTON_UP 1
#define USER_BUTTON_RIGHT 5
#define USER_BUTTON_HOME 2
#define USER_BUTTON_DOWN 6
#define USER_BUTTON_SELECT 4
#define USER_BUTTON_POWER 0
#define CHG_IND_PIN 7

#define RED_RGB_LED_PIN 0
#define GREEN_RGB_LED_PIN 1
#define BLUE_RGB_LED_PIN 2

#define HAPTIC_PIN 10
#define ADC_PIN 5

#define LCD_LEDC_RESOLUTION LEDC_TIMER_10_BIT // 0-1023
#define LCD_LEDC_FREQ_HZ 5000 // 5kHz PWM
#define LCD_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_LEDC_TIMER LEDC_TIMER_0

#define HAPTIC_MAX_MS 50
#define HAPTIC_MIN_MS 10

#define RGB_PERIOD_MAX_MS 50
#define RGB_PERIOD_MIN_MS 0
#define RGB_TOTAL_MAX_MS 500
#define RGB_TOTAL_MIN_MS 40

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
void gpio_init_nvs(void);

/**
 * @brief Initialize the TCA9535 I2C expander and configure:
 *          - Port 0 = all inputs
 *          - Port 1 = all outputs
 *
 * @return ESP_OK on success
 */
esp_err_t gpio_init(void);

/**
 * @brief Read one pin on Port 0 (0…7)
 *
 * @param [in] pin Pin index (0…7)
 *
 * @return 0 or 1 state, or –1 if invalid pin
 */
int gpio_read_input(uint8_t pin);

/**
 * @brief Drive one pin on Port 1 (0…7)
 *
 * @param [in] pin Pin index (0…7)
 * @param [in] level true = high, false = low
 *
 * @return ESP_OK on success
 */
esp_err_t gpio_write_output(uint8_t pin, bool level);

/** 
 * @brief Cycle through the RGB LED to make sure it is working
 */
void gpio_cycle_rgb(void);

/** 
 * @brief Initalize battery ADC
 */
void gpio_init_battery_adc(void);

/** 
 * @brief De-initalize battery ADC to save power
 */
void gpio_deinit_battery_adc(void);

/** 
 * @brief Get the raw battery voltage with software averaging
 *
 * @return The value in volts
 */
float gpio_get_battery_voltage(void);

/** 
 * @brief Convert the raw voltage to a state-of-charge percentage 0-100 based on a typical LiPo discharge curve
 *
 * @param [in] voltage The voltage value to convert
 *
 * @return The value in percent
 */
uint8_t gpio_volts_to_soc(float voltage);

/** 
 * @brief Spins the haptic motor for a given duration
 *
 * @param [in] ms Time on in milliseconds
 */
void gpio_spin_haptic(uint32_t ms);

/** 
 * @brief Indicate HW state via the built-in RGB LED
 *
 * @param [in] rgb_data The state to indicate
 */
void gpio_rgb_indicate(uint8_t rgb_data);

#endif // GPIO_FUNCS_H
