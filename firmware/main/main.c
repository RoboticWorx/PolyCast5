#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"

#include "lora_task.h"
#include "sx126x.h"
#include "sx126x_hal.h"

#include "lcd_funcs.h"
#include "lcd_task.h"
#include "lvgl.h"

#include "infrared_task.h"

// Logging tag
static const char *TAG = "MAIN";

#define SPI_MOSI_PIN 7 // Shared MOSI
#define SPI_SCLK_PIN 6 // Shared SCLK
#define SPI_MISO_PIN 2 // MISO for SX126x (optional for ST7789)

// SPI device handles
spi_device_handle_t spi_sx126x; // For SX126x
spi_device_handle_t spi_st7789; // For ST7789

// Global SX126x instance
sx126x_t sx126x;

// Initialize shared SPI bus
static void spi_shared_init(void) {
	esp_err_t ret;

	// Shared bus configuration
	spi_bus_config_t buscfg = {
		.miso_io_num = SPI_MISO_PIN, // Required for SX126x, ignored by ST7789
		.mosi_io_num = SPI_MOSI_PIN,
		.sclk_io_num = SPI_SCLK_PIN,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4094 // Max of both devices' needs
	};

	// Initialize the SPI bus once
	ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
	ESP_ERROR_CHECK(ret);

	// SX126x device configuration
	spi_device_interface_config_t sx126x_devcfg = {
		.mode = 0,
		.clock_speed_hz = 1 * 1000 * 1000, // 1 MHz for SX126x
		.spics_io_num = SX126X_CS_PIN,	   // Automatic CS control
		.queue_size = 7,
	};
	ret = spi_bus_add_device(SPI2_HOST, &sx126x_devcfg, &spi_sx126x);
	ESP_ERROR_CHECK(ret);

	// ST7789 device configuration
	spi_device_interface_config_t st7789_devcfg = {
		.mode = 0,
		.clock_speed_hz = 10 * 1000 * 1000, // 10 MHz for ST7789
		.spics_io_num = ST7789_CS_PIN,		// Automatic CS control
		.queue_size = 7,
		.flags = SPI_DEVICE_NO_DUMMY,
	};
	ret = spi_bus_add_device(SPI2_HOST, &st7789_devcfg, &spi_st7789);
	ESP_ERROR_CHECK(ret);

	// Configure ST7789 DC and RST pins
	gpio_config_t io_conf = {.pin_bit_mask = (1ULL << ST7789_DC_PIN) |
											 (1ULL << ST7789_RST_PIN),
							 .mode = GPIO_MODE_OUTPUT,
							 .pull_up_en = 0,
							 .pull_down_en = 0,
							 .intr_type = GPIO_INTR_DISABLE};
	gpio_config(&io_conf);
}

// Initialize GPIOs
static void gpio_init(void) {
	gpio_config_t io_conf = {
		.pin_bit_mask =
			(1ULL << ST7789_LEDK_PIN), // | (1ULL << ST7789_RST_PIN),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = 0,
		.pull_down_en = 0,
		.intr_type = GPIO_INTR_DISABLE};

	gpio_config(&io_conf);
}

void app_main(void) {
	
	// Initialize SPI
	//spi_shared_init();
	gpio_init();

	// Initialize the SX126x HAL with the SPI handle
	//sx126x_hal_init(spi_sx126x);

	// Initialize the sx126x_t structure
	sx126x.context = NULL; // Set context to NULL
	sx126x.hal_reset = sx126x_hal_reset;
	sx126x.hal_wakeup = sx126x_hal_wakeup;
	sx126x.hal_write = sx126x_hal_write;
	sx126x.hal_read = sx126x_hal_read;

	// Create tasks
	//lora_task_create();
	//lcd_task_create();
	infrared_task_create();

	ESP_LOGI(TAG, "SX126x initialized and LoRa task created");
	
}