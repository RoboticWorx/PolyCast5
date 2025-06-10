#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_log.h"
#include "esp_psram.h"

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

#include "espnow_task.h"
#include "espnow_funcs.h"

// Logging tag
static const char *TAG = "MAIN";



// SPI device handles
spi_device_handle_t spi_sx126x; // For SX126x
spi_device_handle_t spi_st7789; // For ST7789

// Global SX126x instance
sx126x_t sx126x;

// MOVE TO LORA_FUNCS ><
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
	
	/*
	// prints PSRAM chip size
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI("PSRAM", "Detected PSRAM size = %u KB", psram_size/1024);

    // prints how much of that is free for malloc()
    size_t free_ext = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI("PSRAM", "Free PSRAM heap = %u KB", free_ext/1024);

    // also show internal
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI("PSRAM", "Free internal heap = %u KB", free_int/1024);
    */
    
    // Initialize NVS flash
    gpio_init_nvs();
    
    // Allocate Wi-Fi buffers now without fragmentation
    ESP_ERROR_CHECK(esp_funcs_wifi_driver_init());
    // Turn off radio to save power
    ESP_ERROR_CHECK(esp_funcs_wifi_radio_stop());
	
	// Initialize various
	lcd_init_driver();
    lcd_lvgl_init();
	spi_sx126x_init();
	
	xGpioEventSemaphore = xSemaphoreCreateBinary();
    if (gpio_init() != ESP_OK) {
        ESP_LOGE(TAG, "GPIO_Init failed, stopping task");
        vTaskDelete(NULL);
        return;
    }
	
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
	lora_task_create();
	lcd_task_create();
	gpio_task_create();
	infrared_task_create();
	espnow_task_create();
	//ble_hid_task_start_up();

	ESP_LOGI(TAG, "Main initialized and tasks created");
	
}