#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"

#include "hal/gpio_types.h"
#include "lora_task.h"
#include "sx126x.h"
#include "sx126x_hal.h"

#include "lcd_funcs.h"
#include "lcd_task.h"
#include "lvgl.h"

#include "infrared_task.h"
#include "infrared_funcs.h"

//#include "bluetooth_task.h"
//#include "bluetooth_funcs.h"

#include "gpio_task.h"
#include "gpio_funcs.h"

// Logging tag
static const char *TAG = "MAIN";



// SPI device handles
spi_device_handle_t spi_sx126x; // For SX126x
spi_device_handle_t spi_st7789; // For ST7789

// Global SX126x instance
sx126x_t sx126x;

void spi_sx126x_init(void)
{
    esp_err_t ret;

    // Attach the SX126x device
    spi_device_interface_config_t sx_cfg = {
        .mode           = 0,
        .clock_speed_hz = 1 * 1000 * 1000,   // 1 MHz for LoRa
        .spics_io_num   = SX126X_CS_PIN,
        .queue_size     = 1,
    };
    ret = spi_bus_add_device(SPI2_HOST, &sx_cfg, &spi_sx126x);
    assert(ret == ESP_OK);
}



void app_main(void) {
	
	// Initialize various
	lcd_init_driver();
    lcd_lvgl_init();
	spi_sx126x_init();
	
	ESP_LOGI(TAG, "Initializing GPIO expander...");
    if (gpio_init() != ESP_OK) {
        ESP_LOGE(TAG, "GPIO_Init failed, stopping task");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "GPIO expander ready");
	
	gpio_set_level(ST7789_LEDK_PIN, 1);
	
	// Initialize the SX126x HAL with the SPI handle
	sx126x_hal_init(spi_sx126x);

	// Initialize the sx126x_t structure
	sx126x.context = NULL; // Set context to NULL
	sx126x.hal_reset = sx126x_hal_reset;
	sx126x.hal_wakeup = sx126x_hal_wakeup;
	sx126x.hal_write = sx126x_hal_write;
	sx126x.hal_read = sx126x_hal_read;

	// Create tasks
	//lora_task_create();
	lcd_task_create();
	gpio_task_create();
	//infrared_task_create();
	//ble_hid_task_start_up();

	ESP_LOGI(TAG, "Main initialized and tasks created");
	
}