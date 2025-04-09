#include "sx126x_hal.h"

#include "driver/gpio.h"	   // For GPIO control
#include "driver/spi_master.h" // For SPI communication

#include "esp_log.h"		   // For logging
#include "freertos/FreeRTOS.h" // For vTaskDelay
#include "freertos/task.h"	   // For task delays

// Logging tag for debugging
static const char *TAG = "SX126X_HAL";

// Global SPI device handle
static spi_device_handle_t sx126x_spi = NULL;

// Initialize the SX126x HAL with the SPI handle and configure GPIO pins
void sx126x_hal_init(spi_device_handle_t spi) {
	sx126x_spi = spi;

	// Configure GPIO pins
	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << SX126X_CS_PIN) | (1ULL << SX126X_NRST_PIN),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&io_conf);

	io_conf.pin_bit_mask = (1ULL << SX126X_BUSY_PIN) | (1ULL << SX126X_DIO1_PIN);
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pull_up_en =
		GPIO_PULLUP_ENABLE; // Optional: enable pull-up for stability
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	io_conf.intr_type = GPIO_INTR_DISABLE; // Interrupts will be handled
										   // separately in lora_task.c
	gpio_config(&io_conf);

	// Set initial states
	gpio_set_level(SX126X_CS_PIN, 1);	// CS high (inactive)
	gpio_set_level(SX126X_NRST_PIN, 1); // Reset high (inactive)
}

// Reset the SX126x chip
sx126x_hal_status_t sx126x_hal_reset(const void *context) {
	// Ignore the context parameter
	(void)context;

	// Set reset pin low
	gpio_set_level(SX126X_NRST_PIN, 0);
	vTaskDelay(pdMS_TO_TICKS(10)); // Delay 10ms

	// Set reset pin high
	gpio_set_level(SX126X_NRST_PIN, 1);
	vTaskDelay(pdMS_TO_TICKS(10)); // Delay 10ms

	return SX126X_HAL_STATUS_OK;
}

// Wake up the SX126x chip
sx126x_hal_status_t sx126x_hal_wakeup(const void *context) {
	// Ignore the context parameter
	(void)context;

	if (sx126x_spi == NULL) {
		ESP_LOGE(TAG, "SPI not initialized");
		return SX126X_HAL_STATUS_ERROR;
	}

	// CS low to begin communication
	gpio_set_level(SX126X_CS_PIN, 0);

	// Send a dummy byte (NOP command)
	uint8_t dummy_byte = SX126X_NOP;
	spi_transaction_t trans = {
		.tx_buffer = &dummy_byte,
		.length = 8, // 1 byte (8 bits)
	};

	esp_err_t ret = spi_device_transmit(sx126x_spi, &trans);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "SPI transmit failed: %s", esp_err_to_name(ret));
		gpio_set_level(SX126X_CS_PIN, 1); // CS high on failure
		return SX126X_HAL_STATUS_ERROR;
	}

	// CS high when done
	gpio_set_level(SX126X_CS_PIN, 1);

	// Wait for SX126x to be ready (BUSY pin goes low)
	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1)); // Small delay to avoid busy-waiting
	}

	return SX126X_HAL_STATUS_OK;
}

// Write data to the SX126x chip
sx126x_hal_status_t sx126x_hal_write(const void *context,
									 const uint8_t *command,
									 const uint16_t command_length,
									 const uint8_t *data,
									 const uint16_t data_length) {
	// Ignore the context parameter
	(void)context;

	if (sx126x_spi == NULL) {
		ESP_LOGE(TAG, "SPI not initialized");
		return SX126X_HAL_STATUS_ERROR;
	}

	// Wait for SX126x to be ready (BUSY pin goes low)
	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	// CS low to begin communication
	gpio_set_level(SX126X_CS_PIN, 0);

	// Transmit command
	spi_transaction_t trans = {
		.tx_buffer = command,
		.length = command_length * 8, // Length in bits
	};

	esp_err_t ret = spi_device_transmit(sx126x_spi, &trans);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "SPI command transmit failed: %s", esp_err_to_name(ret));
		gpio_set_level(SX126X_CS_PIN, 1); // CS high on failure
		return SX126X_HAL_STATUS_ERROR;
	}

	// Transmit data (if any)
	if (data != NULL && data_length > 0) {
		trans.tx_buffer = data;
		trans.length = data_length * 8; // Length in bits
		ret = spi_device_transmit(sx126x_spi, &trans);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "SPI data transmit failed: %s", esp_err_to_name(ret));
			gpio_set_level(SX126X_CS_PIN, 1); // CS high on failure
			return SX126X_HAL_STATUS_ERROR;
		}
	}

	// CS high when done
	gpio_set_level(SX126X_CS_PIN, 1);

	return SX126X_HAL_STATUS_OK;
}

// Read data from the SX126x chip
sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command,
									const uint16_t command_length,
									uint8_t *data, const uint16_t data_length) {
	// Ignore the context parameter
	(void)context;

	if (sx126x_spi == NULL) {
		ESP_LOGE(TAG, "SPI not initialized");
		return SX126X_HAL_STATUS_ERROR;
	}

	// Wait for SX126x to be ready (BUSY pin goes low)
	while (gpio_get_level(SX126X_BUSY_PIN) == 1) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	// CS low to begin communication
	gpio_set_level(SX126X_CS_PIN, 0);

	// Transmit command
	spi_transaction_t trans = {
		.tx_buffer = command,
		.length = command_length * 8, // Length in bits
	};

	esp_err_t ret = spi_device_transmit(sx126x_spi, &trans);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "SPI command transmit failed: %s", esp_err_to_name(ret));
		gpio_set_level(SX126X_CS_PIN, 1); // CS high on failure
		return SX126X_HAL_STATUS_ERROR;
	}

	// Receive data (if any)
	if (data != NULL && data_length > 0) {
		uint8_t dummy_byte = SX126X_NOP;
		trans.tx_buffer = &dummy_byte; // Send NOP while receiving
		trans.rx_buffer = data;
		trans.length = data_length * 8;	  // Length in bits
		trans.rxlength = data_length * 8; // Receive length in bits
		ret = spi_device_transmit(sx126x_spi, &trans);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "SPI data receive failed: %s", esp_err_to_name(ret));
			gpio_set_level(SX126X_CS_PIN, 1); // CS high on failure
			return SX126X_HAL_STATUS_ERROR;
		}
	}

	// CS high when done
	gpio_set_level(SX126X_CS_PIN, 1);

	return SX126X_HAL_STATUS_OK;
}