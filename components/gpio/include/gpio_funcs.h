#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

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
