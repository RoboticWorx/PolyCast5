#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define SPI_MISO_PIN 2 // MISO for SX126x
#define SPI_MOSI_PIN 7 // SPI2 MOSI
#define SPI_SCLK_PIN 6 // SPI2 SCLK
#define ST7789_CS_PIN 11 // CS
#define ST7789_DC_PIN 25 // D/C
#define ST7789_RST_PIN 26 // RESET
#define ST7789_LEDK_PIN 10 // Backlight

#define USER_BUTTON_LEFT 1
#define USER_BUTTON_UP 0
#define USER_BUTTON_RIGHT 2
#define USER_BUTTON_HOME 3
#define USER_BUTTON_DOWN 4
#define USER_BUTTON_SELECT 5
#define USER_BUTTON_POWER 1

#define HAPTIC_PIN 15 // 15 is NC: placeholder

/** 
 * @brief Initialise NVS flash
 */
void gpio_init_nvs(void);

/**
 * @brief Initialize the TCA9535 I²C expander and configure:
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

#ifdef POLYCAST5_CYCLE_RGB_ON_BOOT
	/** 
	 * @brief Cycle through the RGB LED to make sure it is working
	 */
	void gpio_cycle_rgb(void);
#endif

#endif // GPIO_FUNCS_H
