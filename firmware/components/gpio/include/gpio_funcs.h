#ifndef GPIO_FUNCS_H
#define GPIO_FUNCS_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief  Initialize the TCA9535 I²C expander and configure:
 *           • Port 0 = all inputs
 *           • Port 1 = all outputs
 * @return ESP_OK on success
 */
esp_err_t gpio_init(void);

/**
 * @brief  Read one pin on Port 0 (0…7)
 * @param  pin  Pin index (0…7)
 * @return 0 or 1, or –1 if invalid pin
 */
int gpio_read_input(uint8_t pin);

/**
 * @brief  Drive one pin on Port 1 (0…7)
 * @param  pin    Pin index (0…7)
 * @param  level  true = high, false = low
 * @return ESP_OK on success
 */
esp_err_t gpio_write_output(uint8_t pin, bool level);

#endif // GPIO_FUNCS_H
